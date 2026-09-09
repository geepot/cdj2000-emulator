# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate and replay a connected MAIN-to-DSP HPI event transcript.

This replays only captured external transport state. It never fabricates DSP
execution or peripheral responses. The DSP core resumes separately from an
ABI-compatible connected checkpoint via tools.cdj_dsp.replay.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

from .inventory import BASE as L2_BASE, CHECKPOINT_HEADER as HEADER, read_input

STATE_PREFIX = struct.Struct('<IIQQQ')
L2_SIZE = 0x40000


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def checkpoint_start(path: Path) -> dict:
    data = path.read_bytes()
    memories, info = read_input(data)
    fields = HEADER.unpack_from(data)
    header_size, state_size = fields[3:5]
    if info['kind'] != 'checkpoint' or state_size < 100:
        raise ValueError('checkpoint state prefix is incompatible')
    start = header_size
    hpi_address, phase, words, event_sequence, checkpoint_sequence = \
        STATE_PREFIX.unpack_from(data, start)
    reason = data[start + 36:start + 36 + 64].split(b'\0', 1)[0].decode('ascii')
    l2 = memories.get(L2_BASE)
    if l2 is None or len(l2) != L2_SIZE:
        raise ValueError('checkpoint L2 payload is incomplete')
    return dict(reason=reason, hpi_address=hpi_address, boot_phase=phase,
                words=words, event_sequence=event_sequence,
                checkpoint_sequence=checkpoint_sequence, schema=info['schema'], l2=l2)


def host_control(state: dict, value: int) -> None:
    value = (value | value >> 16) & 0xffff
    state['hpirst'] = bool(value & 0x80)
    state['hwob'] = bool(value & 1)
    state['dual_hpia'] = bool(value & 0x200)
    state['hpiasel'] = bool(value & 0x800)
    if value & 4:
        state['hint'] = False
    if value & 2:
        state['dspint'] = True
    if state['hpirst']:
        state['hint'] = state['dspint'] = False


def replay(events: list[dict], start_checkpoint: dict) -> dict:
    state = dict(address=0, boot_phase=0, hpirst=True, hwob=False,
                 dual_hpia=False, hpiasel=False, hint=False, dspint=False)
    l2 = bytearray(L2_SIZE)
    words = 0
    expected_sequence = 0
    hint_acks = dsp_hints = dspint_edges = 0
    chunks: list[dict] = []
    active_chunk = None
    start_verified = False
    known = {'reset_assert', 'rom_hpi_ready', 'boot_phase', 'dsp_start',
             'dsp_stop', 'dsp_hpic_write', 'hpi_host_control_write',
             'hpi_host_address_write', 'hpi_host_data_autoincrement_write',
             'hpi_host_data_fixed_write', 'hpi_host_data_read'}
    for event in events:
        expected_sequence += 1
        if event.get('sequence') != expected_sequence:
            raise ValueError(f'event sequence gap at {expected_sequence}')
        kind = event.get('event')
        if kind not in known:
            raise ValueError(f'unsupported event type: {kind!r}')
        old_hint, old_dspint = state['hint'], state['dspint']
        if kind == 'reset_assert':
            state.update(hpirst=True, hint=False, dspint=False)
        elif kind == 'rom_hpi_ready':
            state.update(hpirst=False, hint=True, dspint=False)
        elif kind == 'boot_phase':
            phase = event['value']
            if not 0 <= phase <= 7:
                raise ValueError('invalid boot phase')
            state['boot_phase'] = phase
        elif kind == 'hpi_host_control_write':
            host_control(state, event['value'])
            hint_acks += old_hint and not state['hint']
            dspint_edges += not old_dspint and state['dspint']
        elif kind == 'dsp_hpic_write':
            value = event['value']
            state['hpirst'] = bool(value & 0x80)
            if value & 2:
                state['dspint'] = False
            if value & 4:
                state['hint'] = True
            if state['hpirst']:
                state['hint'] = state['dspint'] = False
            dsp_hints += not old_hint and state['hint']
        elif kind == 'hpi_host_address_write':
            state['address'] = event['value'] & 0xffffffff
            active_chunk = None
        elif kind in {'hpi_host_data_autoincrement_write',
                      'hpi_host_data_fixed_write'}:
            address = event['address']
            if address != state['address'] or event['size'] != 4 or address & 3:
                raise ValueError(f'HPI data/address mismatch at event {expected_sequence}')
            if not L2_BASE <= address <= L2_BASE + L2_SIZE - 4:
                raise ValueError(f'HPI write outside L2 at event {expected_sequence}')
            offset = address - L2_BASE
            l2[offset:offset + 4] = int(event['value']).to_bytes(4, 'little')
            words += 1
            if active_chunk is None or active_chunk['next_address'] != address:
                active_chunk = dict(first_sequence=expected_sequence,
                                    address=address, words=0,
                                    next_address=address)
                chunks.append(active_chunk)
            active_chunk['words'] += 1
            active_chunk['next_address'] = address + 4
            if kind == 'hpi_host_data_autoincrement_write':
                state['address'] += 4
        elif kind == 'hpi_host_data_read':
            if event['address'] != state['address']:
                raise ValueError(f'HPI read/address mismatch at event {expected_sequence}')
            valid = (state['hwob'] and not state['hpirst'] and
                     L2_BASE <= state['address'] <= L2_BASE + L2_SIZE - 4 and
                     not state['address'] & 3)
            if valid and event['offset'] == 0x80000:
                state['address'] += 4
        elif kind == 'dsp_start' and not start_verified:
            if event['sequence'] != start_checkpoint['event_sequence']:
                raise ValueError('DSP-start checkpoint/event sequence mismatch')
            if (state['address'] != start_checkpoint['hpi_address'] or
                    state['boot_phase'] != start_checkpoint['boot_phase'] or
                    words != start_checkpoint['words'] or
                    bytes(l2) != start_checkpoint['l2']):
                raise ValueError('replayed initial HPI upload does not match DSP-start checkpoint')
            start_verified = True
        if kind in {'reset_assert', 'rom_hpi_ready', 'hpi_host_control_write',
                    'dsp_hpic_write'}:
            if state['hint'] != event['hint'] or state['dspint'] != event['dspint']:
                raise ValueError(f'HPI control state mismatch at event {expected_sequence}')
    if not start_verified:
        raise ValueError('transcript never matched a DSP-start checkpoint')
    for chunk in chunks:
        chunk['end_address'] = chunk.pop('next_address')
    return dict(events=expected_sequence, uploaded_words=words,
                initial_upload_l2_sha256=sha256(start_checkpoint['l2']),
                hint_acknowledgements=hint_acks, dsp_hint_edges=dsp_hints,
                dspint_edges=dspint_edges, chunks=chunks,
                final_boot_phase=state['boot_phase'],
                scope='captured external HPI transport replay; DSP execution resumes from checkpoints')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path, help='connected run directory')
    parser.add_argument('output', type=Path, help='new output directory')
    parser.add_argument('--verify-repeat', action='store_true')
    args = parser.parse_args()
    checkpoint_dir = args.run / 'dsp-checkpoints'
    manifest_path = checkpoint_dir / 'manifest.json'
    events_path = args.run / 'dsp-events.jsonl'
    if not manifest_path.is_file() or not events_path.is_file():
        parser.error('run must contain a checkpoint manifest and event transcript')
    manifest = json.loads(manifest_path.read_text())
    event_data = events_path.read_bytes()
    if (not manifest.get('complete') or
            sha256(event_data) != manifest['event_transcript']['sha256']):
        parser.error('event transcript is incomplete or does not match its manifest')
    starts = []
    for item in manifest['checkpoints']:
        checkpoint = checkpoint_start(checkpoint_dir / item['file'])
        if checkpoint['reason'] == 'DSP start boundary':
            starts.append(checkpoint)
    if len(starts) != 1:
        parser.error('expected exactly one DSP-start checkpoint')
    try:
        events = [json.loads(line) for line in event_data.splitlines()]
        summary = replay(events, starts[0])
        repeated = replay(events, starts[0]) if args.verify_repeat else None
    except (KeyError, TypeError, ValueError) as error:
        parser.error(str(error))
    args.output.mkdir(parents=True, exist_ok=False)
    encoded = (json.dumps(summary, sort_keys=True, separators=(',', ':')) + '\n').encode()
    (args.output / 'replay.json').write_bytes(encoded)
    gate = dict(passed=True, replay_sha256=sha256(encoded),
                input_event_sha256=sha256(event_data), scope=summary['scope'])
    if repeated is not None:
        repeated_encoded = (json.dumps(repeated, sort_keys=True, separators=(',', ':')) + '\n').encode()
        (args.output / 'repeat.json').write_bytes(repeated_encoded)
        gate['repeat_matches'] = encoded == repeated_encoded
        gate['passed'] = gate['repeat_matches']
    (args.output / 'gate.json').write_text(json.dumps(gate, indent=2) + '\n')
    print(json.dumps({key: summary[key] for key in
                      ('events', 'uploaded_words', 'hint_acknowledgements',
                       'dsp_hint_edges', 'dspint_edges', 'final_boot_phase')}))
    return 0 if gate['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
