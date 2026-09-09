# SPDX-License-Identifier: GPL-2.0-or-later
"""Inventory candidate C6x format families without executing or skipping code.

Reads GNU's external tic6x-insn-formats.h as data and scans either a raw L2
image or the L2/SDRAM regions in a schema-1 checkpoint. This is a format scan,
not a disassembler or proof of reachability, valid operands, or emulator
support. Explicit address ranges are required because firmware images also
contain data.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct

BASE = 0x11800000
SHARED_RAM_BASE = 0x80000000
SHARED_RAM_SIZE = 0x20000
SDRAM_BASE = 0xc0000000
CHECKPOINT_HEADER = struct.Struct('<8sIIII9I5IQQ')
CHECKPOINT_MAGIC = {1: b'CDJDSP1\0', 2: b'CDJDSP2\0'}


def _fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def read_input(data):
    """Return address-keyed memory images from raw L2 or a schema-1 checkpoint."""
    if len(data) == 0x40000 and not data.startswith((b'CDJDSP1\0', b'CDJDSP2\0')):
        return {BASE: data}, dict(kind='raw_l2', schema=None)
    if len(data) < CHECKPOINT_HEADER.size:
        raise ValueError('input must be 256 KiB of L2 or a complete checkpoint')
    fields = CHECKPOINT_HEADER.unpack_from(data)
    magic, schema, endian, header_size, state_size = fields[:5]
    component_sizes = fields[5:14]
    l2_size, sdram_size, page_size, page_count, present_pages = fields[14:19]
    payload_size, payload_checksum = fields[19:21]
    if (CHECKPOINT_MAGIC.get(schema) != magic or endian != 0x01020304 or
            header_size != CHECKPOINT_HEADER.size or not state_size or
            not all(component_sizes) or l2_size != 0x40000 or
            sdram_size != 0x2000000 or page_size != 4096 or
            page_count != sdram_size // page_size or
            present_pages > page_count or len(data) != header_size + payload_size):
        raise ValueError('checkpoint structure is incompatible or incomplete')
    payload = memoryview(data)[header_size:]
    if _fnv1a(payload) != payload_checksum:
        raise ValueError('checkpoint payload checksum does not match')
    l2_start = state_size
    shared_size = SHARED_RAM_SIZE if schema >= 2 else 0
    shared_start = l2_start + l2_size
    bitmap_size = (page_count + 7) // 8
    bitmap_start = shared_start + shared_size
    pages_start = bitmap_start + bitmap_size
    expected_size = pages_start + present_pages * page_size
    if len(payload) != expected_size:
        raise ValueError('checkpoint sparse memory layout is incomplete')
    l2 = bytes(payload[l2_start:bitmap_start])
    if shared_size:
        l2 = bytes(payload[l2_start:shared_start])
        shared_ram = bytes(payload[shared_start:bitmap_start])
    else:
        shared_ram = None
    bitmap = payload[bitmap_start:pages_start]
    sdram = bytearray(sdram_size)
    offset = pages_start
    count = 0
    for page in range(page_count):
        if bitmap[page // 8] & (1 << (page % 8)):
            begin = page * page_size
            sdram[begin:begin + page_size] = payload[offset:offset + page_size]
            offset += page_size
            count += 1
    if count != present_pages or offset != len(payload):
        raise ValueError('checkpoint sparse page count does not match')
    memories = {BASE: l2, SDRAM_BASE: sdram}
    if shared_ram is not None: memories[SHARED_RAM_BASE] = shared_ram
    return memories, dict(
        kind='checkpoint', schema=schema, state_size=state_size,
        component_sizes=list(component_sizes),
        shared_ram_captured=shared_ram is not None,
        present_sdram_pages=present_pages)


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


def scan(data, base, start, end, formats):
    pc = start
    while pc < end:
        offset = pc - base
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
    memories = {BASE: data} if isinstance(data, (bytes, bytearray, memoryview)) else data
    rows = []
    for start, end in ranges:
        matches = [(base, image) for base, image in memories.items()
                   if base <= start < end <= base + len(image)]
        if len(matches) != 1 or start % 2 or end % 2:
            raise ValueError('ranges must be aligned halfwords inside one available memory region')
        base, image = matches[0]
        rows.extend(scan(image, base, start, end, formats))
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
    parser.add_argument('dump', type=Path, help='256 KiB L2 dump or schema-1 checkpoint')
    parser.add_argument('output', type=Path, help='new JSON report')
    parser.add_argument('--formats', type=Path, required=True,
                        help='GNU include/opcode/tic6x-insn-formats.h')
    parser.add_argument('--range', dest='ranges', action='append', required=True,
                        help='global L2 or SDRAM START:END (exclusive); repeatable')
    parser.add_argument('--trace', type=Path, help='optional deterministic replay JSONL')
    args = parser.parse_args()
    try:
        data = args.dump.read_bytes()
        memories, input_info = read_input(data)
        ranges = [tuple(int(x, 0) for x in item.split(':')) for item in args.ranges]
        if any(len(r) != 2 for r in ranges):
            raise ValueError('ranges must be START:END pairs')
        source = args.formats.read_bytes()
        trace_data = args.trace.read_bytes() if args.trace else b''
        trace = [json.loads(line) for line in trace_data.splitlines()]
        report = build_report(memories, ranges, read_formats(source.decode()), trace)
        report['input'] = input_info
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
