"""Synthetic RAM structures only; no proprietary firmware fixture required."""
import struct

import pytest

from tools.cdj_main.nxs_gui_state import (
    LayoutError, Memory, MAILBOX_TABLE, POOL_TABLE, TASK_TABLE, inspect,
)


@pytest.fixture
def ram():
    data = bytearray(14 * 1024 * 1024)
    memory = Memory(data)

    def put(address, value):
        struct.pack_into('<I', data, address - memory.base, value)

    for number, count, size in [(52, 8, 4096), (60, 35, 2160)]:
        control = 0x04010000 + number * 32
        descriptor = control + 0x10000
        put(POOL_TABLE + number * 4, control)
        for offset, value in [(0, descriptor), (8, 0x04100000 + number * 0x10000),
                              (12, count * size), (16, count)]:
            put(control + offset, value)
        put(descriptor + 4, count)
        put(descriptor + 8, size)
    for number in [85, 102, 103]:
        control = 0x04040000 + number * 32
        stack = control + 0x10000
        put(TASK_TABLE + number * 4, control)
        put(control, stack)
        put(control + 4, 0xD000 if number != 103 else 0x1000)
        put(stack + 56, {85: 0x0414D5FA, 102: 0x0425998C, 103: 0x04259F2E}[number])
    for number in [44, 50]:
        put(MAILBOX_TABLE + number * 4, 0x04070000 + number * 32)
    return memory, put


def test_empty_queues_are_not_boot_proof(ram):
    result = inspect(ram[0])
    assert not result['known_pool_stall_signature']
    assert result['pools'][52]['free'] == 8
    assert result['mailboxes'][50]['complete']
    assert 'does not prove' in result['meaning']


def test_known_stall_and_response_queue(ram):
    memory, put = ram
    for number in [52, 60]:
        put(memory.u32(POOL_TABLE + number * 4) + 16, 0)
    pool = memory.u32(POOL_TABLE + 52 * 4)
    block = memory.u32(pool + 8)
    mailbox = memory.u32(MAILBOX_TABLE + 50 * 4)
    put(mailbox + 12, block)
    put(mailbox + 16, block)
    put(block + 8, 0x13BE)
    put(block + 24, 52)
    result = inspect(memory)
    assert result['known_pool_stall_signature']
    assert result['mailboxes'][50]['records'][0]['command'] == 0x13BE


def test_reject_wrong_layout(ram):
    memory, put = ram
    descriptor = memory.u32(memory.u32(POOL_TABLE + 52 * 4))
    put(descriptor + 8, 1024)
    with pytest.raises(LayoutError, match='does not match'):
        inspect(memory)


def test_reject_chain_cycle(ram):
    memory, put = ram
    block = memory.u32(memory.u32(POOL_TABLE + 52 * 4) + 8)
    mailbox = memory.u32(MAILBOX_TABLE + 50 * 4)
    put(mailbox + 12, block)
    put(mailbox + 16, block)
    put(block, block)
    put(block + 24, 52)
    with pytest.raises(LayoutError, match='cycle'):
        inspect(memory)


def test_aliases_and_bounds():
    memory = Memory(bytes(range(16)))
    assert memory.read(0xA4000004, 4) == memory.read(0x84000004, 4)
    with pytest.raises(LayoutError, match='outside dump'):
        memory.read(0xE4000004, 4)
    with pytest.raises(LayoutError, match='outside dump'):
        memory.read(0x0400000F, 2)


def test_short_dump_reports_unavailable_response_records(ram):
    memory, put = ram
    pool = memory.u32(POOL_TABLE + 52 * 4)
    mailbox = memory.u32(MAILBOX_TABLE + 50 * 4)
    put(pool + 8, 0x0A000000)
    put(mailbox + 12, 0x0A000000)
    put(mailbox + 16, 0x0A000000)
    queue = inspect(memory)['mailboxes'][50]
    assert not queue['complete']
    assert 'outside dump' in queue['unavailable']


def test_running_task_with_old_saved_pc_is_not_stall(ram):
    memory, put = ram
    for number in [52, 60]:
        put(memory.u32(POOL_TABLE + number * 4) + 16, 0)
    put(memory.u32(TASK_TABLE + 85 * 4) + 4, 0x1000)
    assert not inspect(memory)['known_pool_stall_signature']


def test_empty_mailbox_can_retain_stale_tail(ram):
    memory, put = ram
    put(memory.u32(MAILBOX_TABLE + 44 * 4) + 16, 0x059A15B4)
    queue = inspect(memory)['mailboxes'][44]
    assert queue['complete'] and not queue['records']


def test_mailbox_can_include_heap_owned_reply(ram):
    memory, put = ram
    mailbox = memory.u32(MAILBOX_TABLE + 50 * 4)
    put(mailbox + 12, 0x04090000)
    put(mailbox + 16, 0x04090000)
    put(0x04090008, 0x416)
    queue = inspect(memory)['mailboxes'][50]
    assert queue['complete']
    assert queue['records'][0]['command'] == 0x416
    assert not queue['records'][0]['known_pool_validated']
