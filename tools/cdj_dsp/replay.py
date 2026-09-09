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
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [ROOT / 'tools/cdj_dsp/replay.c', *[
    ROOT / 'emulator/qemu' / name for name in
    ('cdj_c674x.c', 'cdj_c674x_loop.c', 'cdj_c6747_syscfg.c')]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('output', type=Path, help='new directory for manifest and trace')
    parser.add_argument('--steps', type=int, default=10000)
    parser.add_argument('--break-pc', type=lambda value: int(value, 0), default=0,
                        help='stop before executing this program counter (0 disables)')
    args = parser.parse_args()
    if not 0 < args.steps <= 100000000 or not 0 <= args.break_pc <= 0xffffffff:
        parser.error('steps must be 1..100000000 and breakpoint must fit 32 bits')
    if not args.dump.is_file() or args.dump.stat().st_size != 0x40000:
        parser.error('dump must be exactly 256 KiB')
    cc = shutil.which('cc')
    if not cc:
        parser.error('C compiler required (install Xcode command line tools)')
    # Snapshot input so hashing and execution always describe the same bytes.
    data = args.dump.read_bytes()
    with tempfile.TemporaryDirectory(prefix='cdj-dsp-replay-') as temp:
        binary = Path(temp) / 'replay'
        snapshot = Path(temp) / 'l2.bin'
        snapshot.write_bytes(data)
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'emulator/qemu'), *map(str, SOURCES),
                        '-o', str(binary)], check=True)
        args.output.mkdir(parents=True, exist_ok=False)
        inputs = SOURCES + [p.with_suffix('.h') for p in SOURCES[1:]]
        manifest = dict(dump_sha256=hashlib.sha256(data).hexdigest(),
                        dump_path=str(args.dump.resolve()), steps=args.steps,
                        break_pc=args.break_pc, boot_rom_executed=False,
                        sources={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in inputs})
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        with (args.output / 'trace.jsonl').open('w') as trace:
            subprocess.run([str(binary), str(snapshot), str(args.steps), str(args.break_pc)],
                           stdout=trace, check=True)
    with (args.output / 'trace.jsonl').open() as trace:
        last = None
        for line in trace:
            last = line
    print(last.strip() if last else 'No trace output')


if __name__ == '__main__':
    main()
