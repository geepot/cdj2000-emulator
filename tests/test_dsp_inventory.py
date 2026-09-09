"""Discovery scans must respect mixed fetch layout and never imply execution."""
import struct

import pytest

from tools.cdj_dsp.inventory import (
    BASE, CHECKPOINT_HEADER, SDRAM_BASE, _fnv1a, build_report, expression,
    read_formats, read_input,
)


def test_mixed_layout_header_flags_and_trace_evidence():
    data = bytearray(0x40000)
    struct.pack_into('<IHHI', data, 0x20, 0x4683e000, 0xec6e, 0x2d66, 0x02809428)
    struct.pack_into('<I', data, 0x3c, 0xe0400008)
    formats = read_formats('''
        FMT(nop, 16, 0x0c6e, 0x1fff, ignored)
        FMT(mask, 16, 0x2c66, 0x3c7e, ignored)
        FMT(loop, 32, 0x3e000, 0x7ffffe, ignored)
        FMT(mvk, 32, 0x28, 0x7c, ignored)
    ''')
    report = build_report(data, [(BASE + 0x20, BASE + 0x40)], formats,
                          [{'event': 'step', 'pc': 0x00800026}])
    rows = report['instructions']
    assert [r['width'] for r in rows[:4]] == [32, 16, 16, 32]
    assert [r['families'] for r in rows[:4]] == [['loop'], ['nop'], ['mask'], ['mvk']]
    assert rows[2]['parallel'] and rows[2]['trace_pc_visits'] == 1
    assert all(r['pc'] != BASE + 0x3c for r in rows)
    assert report['discovery_only'] and not report['execution_performed']
    assert expression('DSZ(4) | SAT(1) | BR(1) | 0x4') == 0x130004
    with pytest.raises(ValueError):
        expression('__import__("os")')
    with pytest.raises(ValueError):
        build_report(data, [(BASE + 0x22, BASE + 0x24)], formats, [])


def test_header_bits_select_distinct_formats():
    data = bytearray(0x40000)
    struct.pack_into('<I', data, 28, 0xe0208000)
    formats = read_formats('''FMT(arithmetic, 16, BR(0), BR(1), ignored)
                             FMT(branch, 16, BR(1), BR(1), ignored)''')
    report = build_report(data, [(BASE, BASE + 2)], formats, [])
    assert report['instructions'][0]['families'] == ['branch']


def test_checkpoint_sparse_sdram_inventory_and_corruption_rejection():
    state = bytes(16)
    l2 = bytes(0x40000)
    bitmap = bytearray(1024)
    bitmap[2 // 8] |= 1 << (2 % 8)
    page = bytearray(4096)
    struct.pack_into('<I', page, 0, 0x12345678)
    payload = state + l2 + bitmap + page
    header = CHECKPOINT_HEADER.pack(
        b'CDJDSP1\0', 1, 0x01020304, CHECKPOINT_HEADER.size, len(state),
        *([1] * 9), 0x40000, 0x2000000, 4096, 8192, 1,
        len(payload), _fnv1a(payload),
    )
    memories, info = read_input(header + payload)
    assert info['kind'] == 'checkpoint' and info['present_sdram_pages'] == 1
    formats = read_formats('FMT(test, 32, 0x12345678, 0xffffffff, ignored)')
    start = SDRAM_BASE + 2 * 4096
    report = build_report(memories, [(start, start + 4)], formats,
                          [{'event': 'step', 'pc': start}])
    assert report['instructions'][0]['families'] == ['test']
    assert report['instructions'][0]['trace_pc_visits'] == 1
    damaged = bytearray(header + payload)
    damaged[-1] ^= 1
    with pytest.raises(ValueError, match='checksum'):
        read_input(damaged)
