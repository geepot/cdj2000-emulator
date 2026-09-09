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

ROOT = Path(__file__).resolve().parents[2]
SOURCES = [ROOT / 'tools/cdj_dsp/replay.c', *[
    ROOT / 'emulator/qemu' / name for name in
    ('cdj_c674x.c', 'cdj_c674x_loop.c', 'cdj_c6747_syscfg.c', 'cdj_c6747_psc.c',
     'cdj_c6747_mcasp.c', 'cdj_c6747_gpio.c', 'cdj_c6747_i2c.c', 'cdj_c6747_pll.c',
     'cdj_c6747_hpi.c', 'cdj_c6747_emifb.c', 'cdj_dsp_checkpoint.c')]]

CHECKPOINT_HEADER = struct.Struct('<8sIIII9I5IQQ')


def checkpoint_info(data: bytes) -> dict:
    if len(data) < CHECKPOINT_HEADER.size:
        raise ValueError('checkpoint header is incomplete')
    fields = CHECKPOINT_HEADER.unpack_from(data)
    magic, schema, endian, header_size, state_size = fields[:5]
    l2_size, sdram_size, page_size, page_count, present_pages = fields[14:19]
    payload_size = fields[19]
    if (magic != b'CDJDSP1\0' or schema != 1 or endian != 0x01020304 or
            header_size != CHECKPOINT_HEADER.size or len(data) != header_size + payload_size or
            l2_size != 0x40000 or sdram_size != 0x2000000 or page_size != 4096 or
            page_count != sdram_size // page_size):
        raise ValueError('checkpoint is incompatible or incomplete')
    l2_start = header_size + state_size
    l2 = data[l2_start:l2_start + l2_size]
    bitmap_size = (page_count + 7) // 8
    bitmap_start = l2_start + l2_size
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
                sdram_sha256=sdram_hash.hexdigest(), present_pages=present_pages)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
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
    args = parser.parse_args()
    if (not 0 < args.steps <= 100000000 or not 0 <= args.break_pc <= 0xffffffff or
            not 0 <= args.boot_phase <= 7):
        parser.error('steps must be 1..100000000, breakpoint must fit 32 bits, and boot phase must be 0..7')
    if not args.dump.is_file():
        parser.error('input dump/checkpoint must be an existing file')
    if args.expect_trace is not None and not args.expect_trace.is_file():
        parser.error('expected trace must be an existing file')
    cc = shutil.which('cc')
    if not cc:
        parser.error('C compiler required (install Xcode command line tools)')
    # Snapshot input so hashing and execution always describe the same bytes.
    data = args.dump.read_bytes()
    checkpoint = data.startswith(b'CDJDSP1\0')
    capture_manifest = None
    input_checkpoint = None
    if checkpoint:
        try:
            input_checkpoint = checkpoint_info(data)
        except ValueError as error:
            parser.error(str(error))
        manifest_path = args.dump.parent / 'manifest.json'
        if not manifest_path.is_file():
            parser.error('connected checkpoint requires its manifest.json provenance file')
        capture_manifest = json.loads(manifest_path.read_text())
        matching = [item for item in capture_manifest.get('checkpoints', [])
                    if item.get('file') == args.dump.name]
        if (not capture_manifest.get('complete') or len(matching) != 1 or
                matching[0].get('sha256') != input_checkpoint['checkpoint_sha256']):
            parser.error('checkpoint is absent from, or does not match, its complete manifest')
    elif len(data) != 0x40000:
        parser.error('legacy dump must be exactly 256 KiB')
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
                        break_pc=args.break_pc, boot_phase=args.boot_phase,
                        input_kind='connected_checkpoint' if checkpoint else 'legacy_l2_dump',
                        input_checkpoint=input_checkpoint,
                        capture_source_sha256=capture_manifest.get('source_sha256') if capture_manifest else None,
                        capture_firmware_sha256=capture_manifest.get('firmware_sha256') if capture_manifest else None,
                        boot_rom_executed=False,
                        pll_assumptions=['POR configuration at ROM handoff',
                                         'initial bypass; NXS OSCIN 16934400 Hz, active SYSCLK1 division',
                                         'catalog PLL reset/lock bounds applied to custom DSP; not measured lock',
                                         'early PLL enable latches and is flagged; analog acquisition not simulated',
                                         'HPIC begins after MAIN HWOB setup and DSPINT; no later host events replayed',
                                         'EMIFB register readback and 32 MiB storage modeled; SDRAM command timing and arbitration omitted',
                                         f'MAIN-to-DSP GPIO boot phase fixed at {args.boot_phase}; other external GPIO inputs default low',
                                         'oscillator counter complete at handoff, not PLL lock',
                                         'legacy PLLCTL bit 4 writable latch; C6747 effect unverified',
                                         'divider GO completes after eight subsequent DSP cycles; not physical clock timing'],
                        sources={str(p.relative_to(ROOT)): hashlib.sha256(content).hexdigest()
                                 for p, content in source_data.items()})
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        with (args.output / 'trace.jsonl').open('w') as trace:
            subprocess.run([str(binary), str(snapshot), str(args.steps), str(args.break_pc),
                            str(args.boot_phase), str(args.output / 'final.cdjdsp')],
                           stdout=trace, check=True)
        if args.verify_repeat:
            with (args.output / 'repeat.jsonl').open('w') as trace:
                subprocess.run([str(binary), str(snapshot), str(args.steps), str(args.break_pc),
                                str(args.boot_phase), str(args.output / 'repeat-final.cdjdsp')],
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
            final = (args.output / 'final.cdjdsp').read_bytes()
            repeat_final = (args.output / 'repeat-final.cdjdsp').read_bytes()
            gate['final_checkpoint'] = checkpoint_info(final)
            gate['repeat_final_checkpoint'] = checkpoint_info(repeat_final)
            gate['final_state_and_memory_match'] = final == repeat_final
            gate['passed'] &= gate['final_state_and_memory_match']
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
