"""Dynamic coverage must distinguish completed packets from probable code."""
import json
import struct

from tools.cdj_dsp.coverage import build_coverage
from tools.cdj_dsp.inventory import CHECKPOINT_HEADER, SHARED_RAM_SIZE, _fnv1a


FORMATS = b'''
FMT(s_mvk, 32, 0x28, 0x3c,
    CFLDS4(FLD(s, 1, 1), FLD(h, 6, 1), FLD(cst, 7, 16), FLD(dst, 23, 5)))
FMT(s_ext_branch_cond_imm, 32, 0x10, 0x7c,
    CFLDS2(FLD(s, 1, 1), FLD(cst, 7, 21)))
FMT(s_call_imm_nop, 32, 0x10, 0xe000007c,
    NFLDS3(FLD(s, 1, 1), FLD(cst, 7, 21), FLD(z, 28, 1)))
'''


def checkpoint(words=()):
    state = bytes(16)
    l2 = bytearray(0x40000)
    for offset, word in words:
        struct.pack_into('<I', l2, offset, word)
    payload = state + l2 + bytes(SHARED_RAM_SIZE) + bytes(1024)
    header = CHECKPOINT_HEADER.pack(
        b'CDJDSP4\0', 4, 0x01020304, CHECKPOINT_HEADER.size, len(state),
        *([1] * 9), 0x40000, 0x2000000, 4096, 8192, 0,
        len(payload), _fnv1a(payload),
    )
    return header + payload


def trace(*events):
    return b''.join((json.dumps(event) + '\n').encode() for event in events)


def test_confirmed_packets_groups_edges_and_loop_scheduler_caveat():
    mvk = (3 << 23) | (123 << 7) | 0x28
    branch = 0x10 | (1 << 7)  # Fetch-packet base + 4: self target at 0x11800024.
    data = trace(
        {'event': 'coverage_summary', 'first_pc': 0x00800020,
         'last_pc': 0x11800024, 'unique_pcs': 2, 'unique_edges': 2,
         'unique_source_pcs': 2, 'source_fetches': 4,
         'scheduler_cycles': 3, 'idle_cycles': 0, 'overflow': False},
        {'event': 'coverage_pc', 'pc': 0x00800020, 'direct_fetches': 1,
         'loop_fetches': 0, 'scheduler_cycles': 0, 'idle_cycles': 0,
         'encoding_changed': False},
        {'event': 'coverage_instruction', 'source_pc': 0x00800020,
         'packet_next_pc': 0x00800024, 'index': 0, 'pc': 0x00800020,
         'word': mvk, 'header': 0, 'compact': False, 'parallel': False},
        {'event': 'coverage_pc', 'pc': 0x11800024, 'direct_fetches': 1,
         'loop_fetches': 2, 'scheduler_cycles': 3, 'idle_cycles': 0,
         'encoding_changed': False},
        {'event': 'coverage_instruction', 'source_pc': 0x11800024,
         'packet_next_pc': 0x11800028, 'index': 0, 'pc': 0x11800024,
         'word': branch, 'header': 0, 'compact': False, 'parallel': False},
        {'event': 'coverage_edge', 'from': 0x00800020, 'to': 0x11800024, 'count': 1},
        {'event': 'coverage_edge', 'from': 0x11800024, 'to': 0x11800024, 'count': 2},
        {'event': 'stop', 'reason': 'step_limit', 'fault': '', 'pc': 0x11800024,
         'fault_pc': 0, 'fault_word': 0},
    )
    report = build_coverage(checkpoint([(0x20, mvk), (0x24, branch)]), data, FORMATS)
    assert report['validation_eligible']
    assert report['entry_points'] == [0x11800020]
    assert report['counts']['confirmed_source_packets'] == 2
    assert report['counts']['confirmed_instruction_addresses'] == 2
    assert report['counts']['self_edges'] == 1
    assert report['probable_code'] == [] and report['unsupported'] == []
    branch_row = next(row for row in report['confirmed_instructions']
                      if row['pc'] == 0x11800024)
    assert branch_row['instruction_family'] == 'control_flow'
    assert branch_row['direct_target'] == 0x11800024
    assert branch_row['delay_slots'] == 5
    assert branch_row['source_fetches'] == 3


def test_faulting_word_is_probable_and_never_confirmed():
    data = trace(
        {'event': 'coverage_summary', 'first_pc': 0, 'last_pc': 0,
         'unique_pcs': 0, 'unique_edges': 0, 'unique_source_pcs': 0,
         'source_fetches': 0, 'scheduler_cycles': 0, 'idle_cycles': 0,
         'overflow': False},
        {'event': 'stop', 'reason': 'fault', 'fault': 'instruction not implemented',
         'pc': 0x00800020, 'fault_pc': 0x00800020, 'fault_word': 0xffffffff},
    )
    report = build_coverage(checkpoint([(0x20, 0xffffffff)]), data, FORMATS)
    assert not report['validation_eligible']
    assert report['confirmed_instructions'] == []
    assert report['unsupported'] == [
        {'pc': 0x11800020, 'word': 0xffffffff, 'reason': 'instruction not implemented'}]
    assert report['probable_code'][0]['pc'] == 0x11800020
