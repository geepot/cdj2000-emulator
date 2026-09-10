"""Check the observed initial stock stereo S16 unpack blocks against a WAV.

Specific bounded experiment: bank 0, initial raw base 118381e0, 588 frames
per block. Does not generalize lifetime, other modes or physical audio.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import wave


def report(run, wav):
    gate = json.loads((run / 'gate.json').read_text())
    trace_hash = hashlib.sha256((run/'trace.jsonl').read_bytes()).hexdigest()
    if (not gate.get('passed') or not gate.get('repeat_matches')
            or not gate.get('final_state_and_memory_match')
            or trace_hash != gate.get('trace_sha256')):
        raise ValueError('requires verified repeat gate')
    with wave.open(str(wav)) as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (2, 2, 44100):
            raise ValueError('this evidence contract requires stereo S16 44100 Hz')
        samples = source.readframes(source.getnframes())
    rows = []
    pending = None
    for line in (run / 'trace.jsonl').open():
        event = json.loads(line)
        if event['event'] != 'pcm_observation':
            continue
        if event['pc'] == 0xc003c698:
            if pending is not None or event['b4'] != 0 or event['b6'] != 2:
                raise ValueError('unsupported/nested bank or class')
            pending = event
        elif event['pc'] == 0xc003c398:
            if pending is None or event['a6'] != 1176:
                raise ValueError('unexpected kernel contract')
            pending['kernel'] = event
        elif event.get('return_observation'):
            if pending is None or 'kernel' not in pending:
                raise ValueError('unpaired return')
            kernel = pending['kernel']
            raw = bytes.fromhex(kernel['raw_banks']['hex'])
            base = kernel['raw_banks']['address']
            offset = kernel['a4'] - base
            expected_offset = pending['a6'] * 588 * 4
            if base != 0x118381e0 or offset != expected_offset:
                raise ValueError('unexpected raw base/block stride')
            source_pcm = samples[offset:offset+2352]
            if len(source_pcm) != 2352:
                raise ValueError('short source block')
            channels = struct.unpack('<1176h', source_pcm)
            input_matches = raw[offset:offset+2352] == source_pcm
            output_matches = []
            for channel in range(2):
                span = event[f'output_plane{channel}']
                expected = struct.pack('<588f', *(x/32768 for x in channels[channel::2]))
                output_matches.append(bytes.fromhex(span['hex']) == expected)
            rows.append(dict(block=pending['a6'], input_address=hex(kernel['a4']),
                             input_matches=input_matches, output_matches=output_matches,
                             output_addresses=[hex(event[f'output_plane{c}']['address']) for c in range(2)],
                             frames=588, entry_packet=pending['packets'], return_packet=event['packets']))
            pending = None
    if pending is not None or not rows:
        raise ValueError('missing complete observed calls')
    return dict(scope='captured genuine stock execution replay; not fresh playback or physical audio',
                wav=str(wav.resolve()), wav_sha256=hashlib.sha256(wav.read_bytes()).hexdigest(),
                trace_sha256=trace_hash,
                verified_connected_stops=gate['verified_connected_stops'], blocks=rows,
                passed=all(r['input_matches'] and all(r['output_matches']) for r in rows))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('wav', type=Path)
    args = parser.parse_args()
    result = report(args.run, args.wav)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['passed'] else 1)
