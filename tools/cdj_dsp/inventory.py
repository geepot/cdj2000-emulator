# SPDX-License-Identifier: GPL-2.0-or-later
"""Inventory candidate C6x format families without executing or skipping code.

Reads GNU's external tic6x-insn-formats.h as data. This is a format scan, not
a disassembler or proof of reachability, valid operands, or emulator support.
Explicit address ranges are required because uploaded images also contain data.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct

BASE = 0x11800000


def expression(text):
    """Evaluate only the numeric OR and header macros used by GNU's table."""
    value = 0
    for term in text.split('|'):
        term = term.strip()
        if re.fullmatch(r'0x[0-9a-fA-F]+|[0-9]+', term):
            value |= int(term, 0)
            continue
        match = re.fullmatch(r'(SAT|BR|DSZ)\((0x[0-9a-fA-F]+|[0-9]+)\)', term)
        if not match:
            raise ValueError(f'unsupported format expression: {text}')
        # Private scan bits, deliberately independent of GNU's pseudo-bit ABI.
        shift, mask = {'SAT': (16, 1), 'BR': (17, 1), 'DSZ': (18, 7)}[match[1]]
        value |= (int(match[2], 0) & mask) << shift
    return value


def read_formats(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    formats = []
    for name, width, value, mask in re.findall(
            r'\bFMT\(\s*(\w+)\s*,\s*(16|32)\s*,\s*([^,]+),\s*([^,]+),', text):
        formats.append((name, int(width), expression(value), expression(mask)))
    if not formats:
        raise ValueError('no GNU C6x formats found')
    return formats


def scan(data, start, end, formats):
    pc = start
    while pc < end:
        offset = pc - BASE
        header = struct.unpack_from('<I', data, (offset & ~31) + 28)[0]
        mixed = header >> 28 == 14
        slot = (offset & 31) // 4
        if mixed and slot == 7:
            pc = (pc & ~31) + 32
            continue
        compact = mixed and (header >> (21 + slot)) & 1
        width = 16 if compact else 32
        if pc % (width // 8) or pc + width // 8 > end:
            raise ValueError(f'range splits an instruction at {pc:#x}')
        word = int.from_bytes(data[offset:offset + width // 8], 'little')
        expanded = word
        if compact:
            expanded |= ((header >> 14) & 1) << 16
            expanded |= ((header >> 15) & 1) << 17
            expanded |= ((header >> 16) & 7) << 18
        matches = [(name, mask.bit_count()) for name, bits, value, mask in formats
                   if bits == width and expanded & mask == value]
        specificity = max((bits for _, bits in matches), default=0)
        families = sorted(name for name, bits in matches if bits == specificity)
        yield dict(pc=pc, word=word, width=width, header=header if mixed else 0,
                   parallel=bool((header >> ((offset & 31) // 2)) & 1)
                   if compact else bool(word & 1),
                   families=families or ['unclassified'])
        pc += width // 8


def build_report(data, ranges, formats, trace):
    rows = [row for start, end in ranges for row in scan(data, start, end, formats)]
    rows = list({row['pc']: row for row in rows}.values())
    observed = Counter()
    fault = None
    for event in trace:
        if event.get('event') == 'step':
            pc = event['pc']
            if 0x00800000 <= pc < 0x00840000:
                pc += 0x11000000
            observed[pc] += 1
        if event.get('event') == 'stop' and event.get('reason') == 'fault':
            fault = event
    groups = {}
    for row in rows:
        row['trace_pc_visits'] = observed[row['pc']]
        for name in row['families']:
            group = groups.setdefault(name, dict(candidates=0, trace_pc_visits=0,
                                                 examples=[], encodings=set()))
            group['candidates'] += 1
            group['trace_pc_visits'] += row['trace_pc_visits']
            group['encodings'].add(row['word'])
            if len(group['examples']) < 8:
                group['examples'].append(row['pc'])
    for group in groups.values():
        group['unique_encodings'] = len(group.pop('encodings'))
    return dict(schema=1, discovery_only=True, execution_performed=False,
                caveat='Candidate formats may be data or invalid instructions. Trace PC visits '
                       'include idle/loading cycles and do not prove instruction execution. '
                       'Family matches do not establish emulator support.',
                ranges=ranges, candidate_count=len(rows), families=groups,
                observed_fault=fault, instructions=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('output', type=Path, help='new JSON report')
    parser.add_argument('--formats', type=Path, required=True,
                        help='GNU include/opcode/tic6x-insn-formats.h')
    parser.add_argument('--range', dest='ranges', action='append', required=True,
                        help='global L2 START:END (exclusive); repeatable')
    parser.add_argument('--trace', type=Path, help='optional deterministic replay JSONL')
    args = parser.parse_args()
    try:
        data = args.dump.read_bytes()
        if len(data) != 0x40000:
            raise ValueError('dump must be exactly 256 KiB')
        ranges = [tuple(int(x, 0) for x in item.split(':')) for item in args.ranges]
        if any(len(r) != 2 or not BASE <= r[0] < r[1] <= BASE + len(data)
               or r[0] % 2 or r[1] % 2 for r in ranges):
            raise ValueError('ranges must be aligned halfwords within global L2')
        source = args.formats.read_bytes()
        trace_data = args.trace.read_bytes() if args.trace else b''
        trace = [json.loads(line) for line in trace_data.splitlines()]
        report = build_report(data, ranges, read_formats(source.decode()), trace)
        report['sha256'] = {name: hashlib.sha256(content).hexdigest() for name, content in
                            [('dump', data), ('formats', source), ('trace', trace_data),
                             ('inventory.py', Path(__file__).read_bytes())]}
        with args.output.open('x') as output:
            json.dump(report, output, indent=2)
            output.write('\n')
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"{report['candidate_count']} candidates in {len(report['families'])} format families")
    for name, group in sorted(report['families'].items(),
                              key=lambda pair: (-pair[1]['candidates'], pair[0])):
        print(f"{group['candidates']:5} {name:28} {group['unique_encodings']:5} encodings")


if __name__ == '__main__':
    main()
