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
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [ROOT / 'tools/cdj_dsp/replay.c', *[
    ROOT / 'emulator/qemu' / name for name in
    ('cdj_c674x.c', 'cdj_c674x_loop.c', 'cdj_c6747_syscfg.c', 'cdj_c6747_psc.c',
     'cdj_c6747_mcasp.c', 'cdj_c6747_gpio.c', 'cdj_c6747_i2c.c', 'cdj_c6747_pll.c')]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('output', type=Path, help='new directory for manifest and trace')
    parser.add_argument('--steps', type=int, default=10000)
    parser.add_argument('--break-pc', type=lambda value: int(value, 0), default=0,
                        help='stop before executing this program counter (0 disables)')
    parser.add_argument('--verify-repeat', action='store_true',
                        help='run the same compiled binary twice and gate on identical traces')
    parser.add_argument('--expect-trace', type=Path,
                        help='also require byte-identical output to this saved trace; not a boot test')
    args = parser.parse_args()
    if not 0 < args.steps <= 100000000 or not 0 <= args.break_pc <= 0xffffffff:
        parser.error('steps must be 1..100000000 and breakpoint must fit 32 bits')
    if not args.dump.is_file() or args.dump.stat().st_size != 0x40000:
        parser.error('dump must be exactly 256 KiB')
    if args.expect_trace is not None and not args.expect_trace.is_file():
        parser.error('expected trace must be an existing file')
    cc = shutil.which('cc')
    if not cc:
        parser.error('C compiler required (install Xcode command line tools)')
    # Snapshot input so hashing and execution always describe the same bytes.
    data = args.dump.read_bytes()
    expected = args.expect_trace.read_bytes() if args.expect_trace is not None else None
    # Compile the exact source/header bytes whose hashes are recorded. A later
    # worktree edit must not make the manifest describe a different binary.
    inputs = SOURCES + [p.with_suffix('.h') for p in SOURCES[1:]]
    source_data = {p: p.read_bytes() for p in inputs}
    with tempfile.TemporaryDirectory(prefix='cdj-dsp-replay-') as temp:
        binary = Path(temp) / 'replay'
        snapshot = Path(temp) / 'l2.bin'
        snapshot.write_bytes(data)
        for path, content in source_data.items():
            (Path(temp) / path.name).write_bytes(content)
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', temp, *[str(Path(temp) / p.name) for p in SOURCES],
                        '-o', str(binary)], check=True)
        args.output.mkdir(parents=True, exist_ok=False)
        manifest = dict(dump_sha256=hashlib.sha256(data).hexdigest(),
                        dump_path=str(args.dump.resolve()), steps=args.steps,
                        break_pc=args.break_pc, boot_rom_executed=False,
                        pll_assumptions=['POR configuration at ROM handoff',
                                         'oscillator counter complete at handoff, not PLL lock',
                                         'legacy PLLCTL bit 4 writable latch; C6747 effect unverified',
                                         'divider GO completes after eight subsequent DSP cycles; not physical clock timing'],
                        sources={str(p.relative_to(ROOT)): hashlib.sha256(content).hexdigest()
                                 for p, content in source_data.items()})
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        with (args.output / 'trace.jsonl').open('w') as trace:
            subprocess.run([str(binary), str(snapshot), str(args.steps), str(args.break_pc)],
                           stdout=trace, check=True)
        if args.verify_repeat:
            with (args.output / 'repeat.jsonl').open('w') as trace:
                subprocess.run([str(binary), str(snapshot), str(args.steps), str(args.break_pc)],
                               stdout=trace, check=True)
    with (args.output / 'trace.jsonl').open() as trace:
        last = None
        for line in trace:
            last = line
    print(last.strip() if last else 'No trace output')
    if args.verify_repeat or expected is not None:
        actual = (args.output / 'trace.jsonl').read_bytes()
        gate = dict(scope='trace equivalence only; not architectural correctness or boot',
                    trace_sha256=hashlib.sha256(actual).hexdigest(), passed=True)
        if args.verify_repeat:
            repeated = (args.output / 'repeat.jsonl').read_bytes()
            gate['repeat_matches'] = actual == repeated
            gate['repeat_sha256'] = hashlib.sha256(repeated).hexdigest()
            gate['passed'] &= gate['repeat_matches']
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
