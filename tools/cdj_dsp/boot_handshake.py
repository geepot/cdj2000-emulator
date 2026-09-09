"""Read-only NXS DSP ready/ack evidence; deliberately not a boot oracle."""
import argparse
import hashlib
import json
from pathlib import Path

MAIN_SHA256 = '02c470e35c944b6107d68a7653b0caabefd6e39650ad5d729e81d59921cb9d85'
WRITES = {'hpi_host_data_fixed_write', 'hpi_host_data_autoincrement_write'}


def analyze_events(events):
    result = dict(ready_reads=0, ready_one_sequence=None, clear_sequence=None,
                  acknowledgement_sequence=None, handshake_observed=False,
                  reset_epochs=0, events=0)
    previous_packets = previous_cycles = 0
    for event in events:
        if not isinstance(event, dict):
            raise ValueError('event must be an object')
        for key in ('sequence', 'offset', 'address', 'value', 'size', 'packets', 'cycles'):
            if type(event.get(key)) is not int or not 0 <= event[key] < 2**64:
                raise ValueError(f'invalid event field: {key}')
        kind = event.get('event')
        if not isinstance(kind, str) or not kind:
            raise ValueError('invalid event type')
        if event['sequence'] != result['events'] + 1:
            raise ValueError('transcript must start at 1 and have contiguous sequences')
        if kind in {'reset_assert', 'dsp_start'}:
            previous_packets = previous_cycles = 0
        if event['packets'] < previous_packets or event['cycles'] < previous_cycles:
            raise ValueError('DSP counters moved backwards outside reset/start')
        previous_packets, previous_cycles = event['packets'], event['cycles']
        result['events'] += 1
        if kind in {'reset_assert', 'dsp_start'}:
            result.update(ready_one_sequence=None, clear_sequence=None,
                          acknowledgement_sequence=None, handshake_observed=False)
        if kind == 'reset_assert':
            result['reset_epochs'] += 1
        address, value = event['address'], event['value']
        if kind == 'hpi_host_data_read' and address == 0x1183fff4:
            if event['size'] != 4 or event['offset'] not in (0x80000, 0xc0000):
                raise ValueError('invalid ready read shape')
            result['ready_reads'] += 1
            if value == 1:
                result.update(ready_one_sequence=event['sequence'],
                              clear_sequence=None, acknowledgement_sequence=None,
                              handshake_observed=False)
            else:
                result.update(ready_one_sequence=None, clear_sequence=None,
                              acknowledgement_sequence=None, handshake_observed=False)
        elif kind in WRITES and address in (0x1183ffec, 0x1183fff0):
            expected_offset = 0xc0000 if kind == 'hpi_host_data_fixed_write' else 0x80000
            if event['size'] != 4 or event['offset'] != expected_offset:
                raise ValueError('invalid handshake write shape')
            if address == 0x1183ffec:
                result['clear_sequence'] = (event['sequence'] if value == 0 and
                                            result['ready_one_sequence'] else None)
                result['acknowledgement_sequence'] = None
                result['handshake_observed'] = False
            elif value == 1 and result['clear_sequence']:
                result['acknowledgement_sequence'] = event['sequence']
                result['handshake_observed'] = True
            else:
                result['acknowledgement_sequence'] = None
                result['handshake_observed'] = False
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('transcript', type=Path)
    args = parser.parse_args()
    digest = hashlib.sha256()

    def events():
        with args.transcript.open('rb') as source:
            for line in source:
                digest.update(line)
                if not line.endswith(b'\n'):
                    raise ValueError('unterminated transcript record')
                yield json.loads(line)
    try:
        result = analyze_events(events())
    except (OSError, ValueError) as error:
        parser.error(str(error))
    result.update(transcript_sha256=digest.hexdigest(),
                  address_map_main_sha256=MAIN_SHA256,
                  scope='Observed ready-read/clear/ack order in the final reset epoch only; '
                        'not firmware authentication, DSP correctness, error-banner absence, '
                        'GUI liveness, full boot, or audio evidence')
    print(json.dumps(result, indent=2))
    return 0 if result['handshake_observed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
