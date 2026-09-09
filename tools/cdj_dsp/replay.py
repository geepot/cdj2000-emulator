# SPDX-License-Identifier: GPL-2.0-or-later
"""Replay a pre-execution NXS DSP L2 dump and capture a deterministic JSONL trace.

This uses the partial C674x core and SYSCFG model, not the missing boot ROM.
Faults and step limits are diagnostic outcomes, never evidence of boot success.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import sys

from .coverage import build_coverage

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [ROOT / 'tools/cdj_dsp/replay.c', *[
    ROOT / 'emulator/qemu' / name for name in
    ('cdj_c674x.c', 'cdj_c674x_loop.c', 'cdj_c6747_syscfg.c', 'cdj_c6747_psc.c',
     'cdj_c6747_mcasp.c', 'cdj_c6747_gpio.c', 'cdj_c6747_i2c.c', 'cdj_c6747_pll.c',
     'cdj_c6747_hpi.c', 'cdj_c6747_emifb.c', 'cdj_c6747_intc.c',
     'cdj_c6747_timer.c',
     'cdj_dsp_checkpoint.c')]]

CHECKPOINT_HEADER = struct.Struct('<8sIIII9I5IQQ')
CHECKPOINT_MAGIC = {1: b'CDJDSP1\0', 2: b'CDJDSP2\0', 3: b'CDJDSP3\0',
                    4: b'CDJDSP4\0'}
SHARED_RAM_SIZE = 0x20000
DEFAULT_FORMATS = ROOT / 'build/gdb-17.2/include/opcode/tic6x-insn-formats.h'
ANALYSIS_SOURCES = [ROOT / 'tools/cdj_dsp/coverage.py',
                    ROOT / 'tools/cdj_dsp/inventory.py']


def _fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def checkpoint_info(data: bytes) -> dict:
    if len(data) < CHECKPOINT_HEADER.size:
        raise ValueError('checkpoint header is incomplete')
    fields = CHECKPOINT_HEADER.unpack_from(data)
    magic, schema, endian, header_size, state_size = fields[:5]
    l2_size, sdram_size, page_size, page_count, present_pages = fields[14:19]
    payload_size, payload_checksum = fields[19:21]
    if (CHECKPOINT_MAGIC.get(schema) != magic or endian != 0x01020304 or
            header_size != CHECKPOINT_HEADER.size or len(data) != header_size + payload_size or
            l2_size != 0x40000 or sdram_size != 0x2000000 or page_size != 4096 or
            page_count != sdram_size // page_size or present_pages > page_count or
            _fnv1a(memoryview(data)[header_size:]) != payload_checksum):
        raise ValueError('checkpoint is incompatible or incomplete')
    l2_start = header_size + state_size
    l2 = data[l2_start:l2_start + l2_size]
    shared_size = SHARED_RAM_SIZE if schema >= 2 else 0
    shared_start = l2_start + l2_size
    shared = data[shared_start:shared_start + shared_size]
    bitmap_size = (page_count + 7) // 8
    bitmap_start = shared_start + shared_size
    bitmap = data[bitmap_start:bitmap_start + bitmap_size]
    pages = memoryview(data)[bitmap_start + bitmap_size:]
    sdram_hash = hashlib.sha256()
    zero_page = bytes(page_size)
    offset = 0
    count = 0
    for page in range(page_count):
        if bitmap[page // 8] & (1 << (page % 8)):
            sdram_hash.update(pages[offset:offset + page_size])
            offset += page_size
            count += 1
        else:
            sdram_hash.update(zero_page)
    if count != present_pages or offset != len(pages):
        raise ValueError('checkpoint sparse SDRAM payload is incomplete')
    return dict(schema=schema, byte_order='little', state_size=state_size,
                component_sizes=list(fields[5:14]),
                checkpoint_sha256=hashlib.sha256(data).hexdigest(),
                l2_sha256=hashlib.sha256(l2).hexdigest(),
                shared_ram_captured=schema >= 2,
                shared_ram_sha256=(hashlib.sha256(shared).hexdigest()
                                   if schema >= 2 else None),
                sdram_sha256=sdram_hash.hexdigest(), present_pages=present_pages)


def checkpoint_provenance(path: Path, data: bytes, info: dict) -> dict:
    """Validate a connected or exact-repeat checkpoint's local provenance."""
    manifest_path = path.parent / 'manifest.json'
    if not manifest_path.is_file():
        raise ValueError('checkpoint requires its manifest.json provenance file')
    manifest_data = manifest_path.read_bytes()
    capture_manifest = json.loads(manifest_data)
    matching = [item for item in capture_manifest.get('checkpoints', [])
                if item.get('file') == path.name]
    result = dict(capture_manifest=capture_manifest,
                  manifest_sha256=hashlib.sha256(manifest_data).hexdigest(),
                  gate_sha256=None)
    if (capture_manifest.get('complete') and len(matching) == 1 and
            matching[0].get('sha256') == info['checkpoint_sha256']):
        result['origin'] = 'connected_checkpoint'
        return result
    gate_path = path.parent / 'gate.json'
    gate_data = gate_path.read_bytes() if gate_path.is_file() else b''
    gate = json.loads(gate_data) if gate_data else {}
    key = ('final_checkpoint' if path.name == 'final.cdjdsp' else
           'repeat_final_checkpoint' if path.name == 'repeat-final.cdjdsp' else None)
    recorded = gate.get(key, {}) if key else {}
    if (not gate.get('passed') or not gate.get('repeat_matches') or
            not gate.get('final_state_and_memory_match') or
            recorded.get('checkpoint_sha256') != info['checkpoint_sha256']):
        raise ValueError('checkpoint is absent from a complete connected manifest or exact replay gate')
    result.update(origin='deterministic_replay_checkpoint',
                  gate_sha256=hashlib.sha256(gate_data).hexdigest())
    return result


def newest_checkpoint(directory: Path):
    """Return newest structurally valid, provenance-bearing checkpoint below a directory."""
    candidates = sorted((path for path in directory.rglob('*.cdjdsp')
                         if path.name != 'repeat-final.cdjdsp'),
                        key=lambda path: (path.stat().st_mtime_ns, str(path)), reverse=True)
    failures = []
    for path in candidates:
        try:
            data = path.read_bytes()
            info = checkpoint_info(data)
            provenance = checkpoint_provenance(path, data, info)
            return path, data, info, provenance
        except (OSError, ValueError, json.JSONDecodeError) as error:
            if len(failures) < 3:
                failures.append(f'{path}: {error}')
    detail = '; '.join(failures) if failures else 'no .cdjdsp files found'
    raise ValueError(f'no compatible provenance-bearing checkpoint under {directory}: {detail}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path,
                        help='checkpoint/raw L2 file, or directory whose newest valid checkpoint is selected')
    parser.add_argument('output', type=Path, help='new directory for manifest and trace')
    parser.add_argument('--steps', type=int, default=10000)
    parser.add_argument('--break-pc', type=lambda value: int(value, 0), default=0,
                        help='stop before executing this program counter (0 disables)')
    parser.add_argument('--boot-phase', type=lambda value: int(value, 0), default=0,
                        help='fixed external MAIN boot-phase GPIO value, 0..7 (default: captured phase 0)')
    parser.add_argument('--verify-repeat', action='store_true',
                        help='run the same compiled binary twice and gate on identical traces')
    parser.add_argument('--expect-trace', type=Path,
                        help='also require byte-identical output to this saved trace; not a boot test')
    parser.add_argument('--events', type=Path,
                        help='inject later events from the checkpoint connected-run transcript')
    parser.add_argument('--formats', type=Path, default=DEFAULT_FORMATS,
                        help='GNU tic6x-insn-formats.h used for automatic coverage')
    args = parser.parse_args()
    if (not 0 < args.steps <= 100000000 or not 0 <= args.break_pc <= 0xffffffff or
            not 0 <= args.boot_phase <= 7):
        parser.error('steps must be 1..100000000, breakpoint must fit 32 bits, and boot phase must be 0..7')
    selected_checkpoint = None
    selected_provenance = None
    if args.dump.is_dir():
        try:
            selected_path, data, selected_checkpoint, selected_provenance = newest_checkpoint(args.dump)
        except ValueError as error:
            parser.error(str(error))
        args.dump = selected_path
        print(f'Selected newest compatible checkpoint: {args.dump}', file=sys.stderr)
    elif not args.dump.is_file():
        parser.error('input dump/checkpoint must be an existing file or directory')
    if args.expect_trace is not None and not args.expect_trace.is_file():
        parser.error('expected trace must be an existing file')
    if args.events is not None and not args.events.is_file():
        parser.error('event transcript must be an existing file')
    if not args.formats.is_file():
        parser.error('C6x format header is required (build dependencies per BUILD.md or use --formats)')
    cc = shutil.which('cc')
    if not cc:
        parser.error('C compiler required (install Xcode command line tools)')
    # Snapshot input so hashing and execution always describe the same bytes.
    data = data if selected_checkpoint is not None else args.dump.read_bytes()
    checkpoint = data.startswith(tuple(CHECKPOINT_MAGIC.values()))
    capture_manifest = None
    input_checkpoint = None
    checkpoint_origin = None
    checkpoint_manifest_sha256 = None
    checkpoint_gate_sha256 = None
    if checkpoint:
        try:
            input_checkpoint = selected_checkpoint or checkpoint_info(data)
            provenance = selected_provenance or checkpoint_provenance(
                args.dump, data, input_checkpoint)
        except (ValueError, json.JSONDecodeError) as error:
            parser.error(str(error))
        capture_manifest = provenance['capture_manifest']
        checkpoint_manifest_sha256 = provenance['manifest_sha256']
        checkpoint_origin = provenance['origin']
        checkpoint_gate_sha256 = provenance['gate_sha256']
    elif len(data) != 0x40000:
        parser.error('legacy dump must be exactly 256 KiB')
    event_data = args.events.read_bytes() if args.events is not None else None
    if event_data is not None:
        if not checkpoint:
            parser.error('event injection requires a connected checkpoint')
        transcript = capture_manifest.get('event_transcript')
        expected_event_hash = (transcript.get('sha256') if isinstance(transcript, dict)
                               else capture_manifest.get('event_transcript_sha256'))
        if not isinstance(expected_event_hash, str):
            parser.error('checkpoint manifest has no complete event-transcript provenance')
        if hashlib.sha256(event_data).hexdigest() != expected_event_hash:
            parser.error('event transcript does not match the checkpoint manifest')
    expected = args.expect_trace.read_bytes() if args.expect_trace is not None else None
    # Compile the exact source/header bytes whose hashes are recorded. A later
    # worktree edit must not make the manifest describe a different binary.
    inputs = SOURCES + [p.with_suffix('.h') for p in SOURCES[1:]]
    source_data = {p: p.read_bytes() for p in inputs}
    format_data = args.formats.read_bytes()
    analysis_data = {path: path.read_bytes() for path in ANALYSIS_SOURCES}
    with tempfile.TemporaryDirectory(prefix='cdj-dsp-replay-') as temp:
        binary = Path(temp) / 'replay'
        snapshot = Path(temp) / 'l2.bin'
        snapshot.write_bytes(data)
        event_snapshot = Path(temp) / 'events.jsonl'
        if event_data is not None:
            event_snapshot.write_bytes(event_data)
        for path, content in source_data.items():
            (Path(temp) / path.name).write_bytes(content)
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', temp, *[str(Path(temp) / p.name) for p in SOURCES],
                        '-o', str(binary)], check=True)
        args.output.mkdir(parents=True, exist_ok=False)
        external_event_assumption = (
            'ordered post-checkpoint MAIN/HPI events injected and gated against every connected DSP stop'
            if event_data is not None else
            'no later MAIN/HPI events injected; replay stops when an external event is required')
        manifest = dict(dump_sha256=hashlib.sha256(data).hexdigest(),
                        dump_path=str(args.dump.resolve()), steps=args.steps,
                        break_pc=args.break_pc, boot_phase=args.boot_phase,
                        input_kind=checkpoint_origin if checkpoint else 'legacy_l2_dump',
                        input_checkpoint=input_checkpoint,
                        input_manifest_sha256=checkpoint_manifest_sha256,
                        input_gate_sha256=checkpoint_gate_sha256,
                        event_transcript_sha256=(hashlib.sha256(event_data).hexdigest()
                                                 if event_data is not None else None),
                        capture_source_sha256=(capture_manifest.get('source_sha256') or
                                               capture_manifest.get('capture_source_sha256'))
                                              if capture_manifest else None,
                        capture_firmware_sha256=(capture_manifest.get('firmware_sha256') or
                                                 capture_manifest.get('capture_firmware_sha256'))
                                                if capture_manifest else None,
                        boot_rom_executed=False,
                        pll_assumptions=['POR configuration at ROM handoff',
                                         'initial bypass; NXS OSCIN 16934400 Hz, active SYSCLK1 division',
                                         'catalog PLL reset/lock bounds applied to custom DSP; not measured lock',
                                         'early PLL enable latches and is flagged; analog acquisition not simulated',
                                         external_event_assumption,
                                         ('schema-1 input did not capture shared RAM; restored zero before its first observed use'
                                          if checkpoint and input_checkpoint['schema'] == 1 else
                                          '128 KiB shared RAM captured losslessly in schema-2+ checkpoints'),
                                         'EMIFB register readback and 32 MiB storage modeled; SDRAM command timing and arbitration omitted',
                                         f'MAIN-to-DSP GPIO boot phase fixed at {args.boot_phase}; other external GPIO inputs default low',
                                         'oscillator counter complete at handoff, not PLL lock',
                                         'legacy PLLCTL bit 4 writable latch; C6747 effect unverified',
                                         'divider GO completes after eight subsequent DSP cycles; not physical clock timing'],
                        sources={str(p.relative_to(ROOT)): hashlib.sha256(content).hexdigest()
                                 for p, content in source_data.items()},
                        analysis_sources={
                            **{str(path.relative_to(ROOT)): hashlib.sha256(content).hexdigest()
                               for path, content in analysis_data.items()},
                            str(args.formats.resolve()): hashlib.sha256(format_data).hexdigest(),
                        })
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        command = [str(binary), str(snapshot), str(args.steps), str(args.break_pc),
                   str(args.boot_phase), str(args.output / 'final.cdjdsp')]
        if event_data is not None:
            command.append(str(event_snapshot))
        with (args.output / 'trace.jsonl').open('w') as trace:
            subprocess.run(command, stdout=trace, check=True)
        if args.verify_repeat:
            repeat_command = command.copy()
            repeat_command[5] = str(args.output / 'repeat-final.cdjdsp')
            with (args.output / 'repeat.jsonl').open('w') as trace:
                subprocess.run(repeat_command, stdout=trace, check=True)
    coverage_data = {}
    for trace_name, checkpoint_name, output_name in [
            ('trace.jsonl', 'final.cdjdsp', 'coverage.json'),
            *(([('repeat.jsonl', 'repeat-final.cdjdsp', 'repeat-coverage.json')]
               if args.verify_repeat else []))]:
        checkpoint_bytes = (args.output / checkpoint_name).read_bytes()
        trace_bytes = (args.output / trace_name).read_bytes()
        try:
            coverage = build_coverage(checkpoint_bytes, trace_bytes, format_data)
        except (ValueError, json.JSONDecodeError) as error:
            parser.error(f'coverage generation failed: {error}')
        coverage['sha256'] = {
            'checkpoint': hashlib.sha256(checkpoint_bytes).hexdigest(),
            'trace': hashlib.sha256(trace_bytes).hexdigest(),
            'formats': hashlib.sha256(format_data).hexdigest(),
            **{str(path.relative_to(ROOT)): hashlib.sha256(content).hexdigest()
               for path, content in analysis_data.items()},
        }
        serialized = (json.dumps(coverage, indent=2) + '\n').encode()
        (args.output / output_name).write_bytes(serialized)
        coverage_data[output_name] = (coverage, serialized)
    manifest['coverage'] = {
        'file': 'coverage.json',
        'sha256': hashlib.sha256(coverage_data['coverage.json'][1]).hexdigest(),
        'counts': coverage_data['coverage.json'][0]['counts'],
        'validation_eligible': coverage_data['coverage.json'][0]['validation_eligible'],
    }
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    with (args.output / 'trace.jsonl').open() as trace:
        last = None
        for line in trace:
            last = line
    print(last.strip() if last else 'No trace output')
    if args.verify_repeat or expected is not None:
        actual = (args.output / 'trace.jsonl').read_bytes()
        gate = dict(scope='trace equivalence only; not architectural correctness or boot',
                    trace_sha256=hashlib.sha256(actual).hexdigest(),
                    coverage_sha256=manifest['coverage']['sha256'],
                    coverage_counts=manifest['coverage']['counts'],
                    coverage_validation_eligible=manifest['coverage']['validation_eligible'],
                    passed=True)
        if event_data is not None:
            verified_stops = sum(
                json.loads(line).get('event') == 'verified_connected_stop'
                for line in actual.decode().splitlines())
            gate['verified_connected_stops'] = verified_stops
            gate['passed'] &= verified_stops > 0
        if args.verify_repeat:
            repeated = (args.output / 'repeat.jsonl').read_bytes()
            gate['repeat_matches'] = actual == repeated
            gate['repeat_sha256'] = hashlib.sha256(repeated).hexdigest()
            gate['passed'] &= gate['repeat_matches']
            final = (args.output / 'final.cdjdsp').read_bytes()
            repeat_final = (args.output / 'repeat-final.cdjdsp').read_bytes()
            gate['final_checkpoint'] = checkpoint_info(final)
            gate['repeat_final_checkpoint'] = checkpoint_info(repeat_final)
            gate['final_state_and_memory_match'] = final == repeat_final
            gate['passed'] &= gate['final_state_and_memory_match']
            repeated_coverage = coverage_data['repeat-coverage.json'][1]
            gate['repeat_coverage_sha256'] = hashlib.sha256(repeated_coverage).hexdigest()
            gate['repeat_coverage_matches'] = coverage_data['coverage.json'][1] == repeated_coverage
            gate['passed'] &= gate['repeat_coverage_matches']
        if expected is not None:
            gate['expected_path'] = str(args.expect_trace.resolve())
            gate['expected_sha256'] = hashlib.sha256(expected).hexdigest()
            gate['expected_matches'] = actual == expected
            gate['passed'] &= gate['expected_matches']
        (args.output / 'gate.json').write_text(json.dumps(gate, indent=2) + '\n')
        if not gate['passed']:
            print('Replay equivalence gate failed; inspect gate.json and saved traces', file=sys.stderr)
            raise SystemExit(1)


if __name__ == '__main__':
    main()
