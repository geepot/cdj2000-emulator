# SPDX-License-Identifier: GPL-2.0-or-later
"""Disassemble only confirmed source addresses; never infer code from raw data."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from .inventory import read_input


def assembly_fields(assembly):
    text = assembly.strip()
    if text.startswith('||'):
        text = text[2:].strip()
    predicate = None
    if text.startswith('['):
        match = re.match(r'\[([^]]+)\]\s*(.*)', text)
        if not match:
            raise ValueError('malformed disassembly predicate')
        predicate, text = match.groups()
    match = re.fullmatch(r'([a-z][a-z0-9]*)\s*(?:([.][A-Z][12][A-Z0-9]*)\s*)?(.*)', text)
    if not match:
        return dict(mnemonic=None, unit=None, operands=None, predicate=predicate)
    mnemonic, unit, operands = match.groups()
    return dict(mnemonic=mnemonic, unit=unit, operands=operands.strip(), predicate=predicate)


def parse_disassembly(text, expected):
    decoded = {}
    for line in text.splitlines():
        match = re.fullmatch(r'0x([0-9a-fA-F]+):\t(.*)\t([0-9]+)', line)
        if not match:
            raise ValueError(f'malformed disassembler output: {line!r}')
        pc, assembly, width = int(match[1], 16), match[2], int(match[3]) * 8
        if pc not in expected or pc in decoded or width != expected[pc]['width']:
            raise ValueError(f'disassembly address/width mismatch at {pc:#x}')
        decoded[pc] = dict(assembly=assembly, **assembly_fields(assembly))
    if decoded.keys() != expected.keys():
        raise ValueError('disassembler did not return every requested address')
    return decoded


def build_inventory(checkpoint, coverage_path, disassembler):
    checkpoint_data = checkpoint.read_bytes()
    coverage_data = coverage_path.read_bytes()
    coverage = json.loads(coverage_data)
    checkpoint_hash = hashlib.sha256(checkpoint_data).hexdigest()
    if coverage.get('sha256', {}).get('checkpoint') != checkpoint_hash:
        raise ValueError('coverage does not identify this exact checkpoint')
    memories, _ = read_input(checkpoint_data)
    rows = coverage['confirmed_instructions']
    expected = {row['pc']: row for row in rows}
    if len(expected) != len(rows):
        raise ValueError('duplicate confirmed instruction addresses')
    decoded = {}
    with tempfile.TemporaryDirectory(prefix='cdj-semantic-inventory-') as temporary:
        for base, memory in memories.items():
            selected = {pc: row for pc, row in expected.items() if base <= pc < base + len(memory)}
            if not selected:
                continue
            for pc, row in selected.items():
                offset, size = pc - base, row['width'] // 8
                if size not in (2, 4) or int.from_bytes(memory[offset:offset + size], 'little') != row['word']:
                    raise ValueError(f'captured opcode differs from checkpoint at {pc:#x}')
                if row['header']:
                    header_offset = (offset & ~31) + 28
                    if int.from_bytes(memory[header_offset:header_offset + 4], 'little') != row['header']:
                        raise ValueError(f'captured header differs from checkpoint at {pc:#x}')
            image = Path(temporary) / f'{base:08x}.bin'
            image.write_bytes(memory)
            result = subprocess.run([str(disassembler), str(image), hex(base), '--stdin'],
                input=''.join(f'{pc:#x}\n' for pc in sorted(selected)),
                text=True, capture_output=True, check=True, timeout=60)
            decoded.update(parse_disassembly(result.stdout, selected))
    if decoded.keys() != expected.keys():
        raise ValueError('some confirmed instructions are outside captured memory')
    instructions = [{**row, **decoded[row['pc']]} for row in sorted(rows, key=lambda row: row['pc'])]
    grouped = defaultdict(list)
    for row in instructions:
        grouped[(row['mnemonic'], row['unit'], row['compact'])].append(row)
    groups = []
    for (mnemonic, unit, compact), members in grouped.items():
        groups.append(dict(mnemonic=mnemonic, unit=unit, compact=compact,
            instruction_addresses=len(members),
            source_fetch_observations=sum(row['source_fetches'] for row in members),
            false_only_addresses=sum(row.get('source_predicate_outcomes') == [False] for row in members),
            encodings=sorted({row['word'] for row in members}),
            examples=[row['pc'] for row in members[:8]],
            semantic_validation='not inferred from disassembly; reference-backed tests required'))
    groups.sort(key=lambda row: (-row['false_only_addresses'], -row['source_fetch_observations'],
                                 row['mnemonic'] or '', row['unit'] or '', row['compact']))
    return dict(schema=1, validation_eligible=False,
        scope='GNU disassembly of confirmed source addresses only; no reachability expansion',
        caveats=['Mnemonic/operand decoding is not emulator semantic validation.',
                 'Source fetch observations are not buffered instruction issue frequencies.',
                 'False-only counts prioritize semantic tests, not missing-opcode claims.',
                 'Mnemonic aliases are retained; they are not independent hardware instruction families.',
                 'Probable code and unclassified memory are not promoted to confirmed code.'],
        sha256=dict(checkpoint=checkpoint_hash, coverage=hashlib.sha256(coverage_data).hexdigest(),
                    disassembler=hashlib.sha256(disassembler.read_bytes()).hexdigest(),
                    analyzer=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()),
        counts=dict(instruction_addresses=len(instructions), groups=len(groups),
                    mnemonics=len({row['mnemonic'] for row in instructions if row['mnemonic']}),
                    unresolved_addresses=sum(row['mnemonic'] is None for row in instructions)),
        groups=groups, instructions=instructions)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkpoint', type=Path)
    parser.add_argument('coverage', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--disassembler', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = build_inventory(args.checkpoint, args.coverage, args.disassembler.resolve())
        with args.output.open('x') as stream:
            json.dump(report, stream, indent=2)
            stream.write('\n')
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    print(json.dumps(report['counts']))


if __name__ == '__main__':
    main()
