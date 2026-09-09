# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate deterministic captures of genuine McASP XBUF transmit words."""
from collections import Counter
import hashlib
import json
from pathlib import Path


def tx_capture_metadata(path: Path) -> dict:
    counts = Counter()
    sequence = 0
    digest = hashlib.sha256()
    with path.open('rb') as raw:
        for raw_line in raw:
            digest.update(raw_line)
            sequence += 1
            if not raw_line.endswith(b'\n'):
                raise ValueError(f'truncated DSP transmit capture record {sequence}')
            event = json.loads(raw_line)
            required = {'sequence', 'instance', 'slot', 'serializer', 'word',
                        'xbuf_sequence', 'packets', 'cycles', 'source', 'clock'}
            if (set(event) != required or event['sequence'] != sequence or
                    event['instance'] not in (1, 2) or
                    not 0 <= event['slot'] <= 0x17f or
                    not 0 <= event['serializer'] < 16 or
                    not 0 <= event['word'] <= 0xffffffff or
                    event['xbuf_sequence'] <= 0 or event['packets'] < 0 or
                    event['cycles'] < 0 or event['source'] != 'genuine_xbuf' or
                    event['clock'] != 'functional-coarse-packet-slot'):
                raise ValueError(f'invalid DSP transmit capture record {sequence}')
            counts[f"mcasp{event['instance']}.serializer{event['serializer']}"] += 1
    return dict(schema=1, format='canonical JSONL genuine McASP XBUF slot words',
                file=path.name, sha256=digest.hexdigest(), records=sequence,
                counts=dict(sorted(counts.items())),
                sample_encoding='unsigned 32-bit serializer word; not PCM interpretation',
                timing='functional coarse packet slots; not serializer-clock or audio timing',
                synthesized_samples=False)
