"""Summarize only completed nxs-dsp-playback windows from live QEMU logs."""
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics

WINDOW = re.compile(r'nxs-dsp-playback: final=1 virtual=([\d.]+)s cpu=([\d.]+)s '
                    r'packets=(\d+) background=(\d+) slots=(\d+) underruns=(\d+) position=(\d+)')


def read_window(path: Path) -> dict:
    rows = WINDOW.findall(path.read_text(errors='replace'))
    if len(rows) != 1:
        raise ValueError(f'{path}: expected one completed playback window, found {len(rows)}')
    virtual, cpu = map(float, rows[0][:2])
    packets, background, slots, underruns, position = map(int, rows[0][2:])
    if not (virtual > 0 and cpu > 0 and packets > 0 and slots > 0 and
            background <= packets and underruns <= slots):
        raise ValueError(f'{path}: invalid/empty playback counters')
    return dict(log=str(path.resolve()), log_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                virtual_seconds=virtual, dsp_thread_cpu_seconds=cpu,
                packets=packets, background_packets=background, slots=slots,
                underruns=underruns, final_position=position,
                packets_per_cpu_second=packets / cpu,
                packets_per_virtual_second=packets / virtual,
                background_per_virtual_second=background / virtual,
                background_per_cpu_second=background / cpu,
                underrun_fraction=underruns / slots)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='append', required=True, metavar='LABEL=LOG')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rows = []
    for item in args.run:
        label, separator, path = item.partition('=')
        if not label or not separator:
            parser.error('--run expects LABEL=LOG')
        rows.append(dict(label=label, **read_window(Path(path))))
    metrics = ('packets_per_cpu_second', 'packets_per_virtual_second',
               'background_per_virtual_second', 'background_per_cpu_second',
               'underrun_fraction')
    groups = {label: dict(trials=sum(r['label'] == label for r in rows),
                         **{metric: statistics.median(r[metric] for r in rows
                            if r['label'] == label) for metric in metrics})
              for label in sorted({r['label'] for r in rows})}
    args.output.write_text(json.dumps(dict(runs=rows, medians=groups), indent=2) + '\n')
    print(json.dumps(groups, indent=2))


if __name__ == '__main__':
    main()
