/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"
static uint32_t memory[64];
static bool read_word(void *unused, uint32_t address, uint32_t *value)
{
    (void)unused;
    if (address < 0x1000 || address >= 0x1100 || (address & 3)) return false;
    *value = memory[(address - 0x1000) / 4]; return true;
}
static bool write_memory(void *unused, uint32_t address, uint64_t value,
                         unsigned size, bool commit)
{
    (void)unused;
    if ((size != 4 && size != 8) || address < 0x1000 ||
        address > 0x1100 - size || (address & (size - 1))) return false;
    if (commit) {
        memory[(address - 0x1000) / 4] = value;
        if (size == 8) memory[(address - 0x1000) / 4 + 1] = value >> 32;
    }
    return true;
}
static uint32_t mvk(unsigned side, unsigned dst, int value)
{ return dst << 23 | ((uint32_t)value & 0xffff) << 7 | 0x28 | side << 1; }
int main(void)
{
    CdjC674x c;
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = mvk(1, 15, -8);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][15] == 0xfffffff8);
    memory[1] = (15u << 23) | (0x1180u << 7) | 0x6a;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][15] == 0x1180fff8);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][0] = 7;
    memory[0] = mvk(1, 0, 9) | 1;
    memory[1] = (1u << 23) | (31u << 13) | (1u << 12) | 0xf58;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][1] == 7 && c.r[1][0] == 9);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362;
    for (unsigned i = 1; i <= 6; ++i) memory[i] = mvk(0, 0, i);
    for (unsigned i = 0; i < 6; ++i) assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.r[0][0] == 5 && c.cycles == 6);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362; memory[1] = 8u << 13;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.cycles == 6);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x1234567f;
    memory[0] = (15u << 23) | (15u << 18) | (24u << 13) | 0x9f2;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][15] == 0x12345678);
    memory[1] = mvk(0, 3, 99) | (6u << 29);
    memory[2] = mvk(0, 3, 42) | (6u << 29) | (1u << 28);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL)); assert(c.r[0][3] == 0);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL)); assert(c.r[0][3] == 42);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362;
    memory[1] = (3u << 23) | (3u << 16) | (4u << 13) | 0x162;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.cycles == 6 && c.r[1][3] == 0x100c);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = mvk(0, 0, 99) | 1; memory[1] = 0xffffffff;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][0] == 0 && c.pc == 0x1000 && c.packets == 0);
    /* Header p bits, not opcode bit zero, join compact instructions. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 7; c.r[1][2] = 5;
    uint32_t add = (1u << 13) | (1u << 12) | (2u << 7) | (3u << 4);
    uint32_t sub = (3u << 13) | (1u << 12) | (2u << 7) | (4u << 4) | 0x800;
    memory[0] = add | (sub << 16);
    memory[7] = 0xe0200001; /* word 0 compact; first half parallel */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 12 && c.r[0][4] == (uint32_t)-5);
    assert(c.pc == 0x1004 && c.cycles == 1);

    /* RS applies to both operands and result; sequential halfword PCs. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][17] = 9; c.r[1][18] = 4;
    memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][19] == 13 && c.pc == 0x1002);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][20] == 9 && c.pc == 0x1004);

    /* Full instructions retain their p bit; packets skip the header. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1018);
    memory[6] = mvk(0, 1, 42) | 1;
    memory[7] = 0xe0000000;
    memory[8] = mvk(0, 2, 73);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1024 && c.r[0][1] == 42 && c.r[0][2] == 73);

    /* Signed immediate equality, cross path and predicate. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][4] = 0xffffffff;
    memory[0] = (4u << 18) | (31u << 13) | (1u << 12) | 0xa58;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL)); assert(c.r[0][0] == 1);
    memory[1] = (6u << 29) | (1u << 23) | (4u << 18) | (1u << 12) | 0xa78;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL)); assert(c.r[0][1] == 0);

    /* A later unknown compact instruction rolls back the whole packet. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = mvk(0, 0, 99) | 1;
    memory[1] = 0xffff; memory[7] = 0xe0400000;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.fault_pc == 0x1004 && c.fault_word == 0xffff);
    assert(c.r[0][0] == 0 && c.pc == 0x1000 && c.cycles == 0);
    /* Stack stores sample old registers and update B15 in E1, RAM in E3.
     * RS is ignored by Dpp. Two pushes can be in flight simultaneously. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][15] = 0x10f8; c.r[1][10] = 0x12345678;
    c.r[0][12] = 0x89abcdef; c.r[0][13] = 0x76543210;
    memory[0] = 0x86773577; memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10f0 && c.store_count == 1 && memory[62] == 0);
    c.r[1][10] = 0; /* subsequent changes cannot alter the pending value */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10e8 && c.store_count == 2 && memory[62] == 0);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(memory[62] == 0x12345678 && memory[60] == 0 && c.store_count == 1);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(memory[60] == 0x89abcdef && memory[61] == 0x76543210 && !c.store_count);

    /* Unsupported parallel operation must not enqueue the earlier store. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x10f8;
    memory[0] = 0xffff3577; memory[7] = 0xe0200001;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10f8 && !c.store_count && !c.cycles && !memory[62]);

    /* Bounds and doubleword alignment fail before changing the stack. */
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x1100;
    memory[0] = 0x3577; memory[7] = 0xe0200000;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x1100 && !c.store_count);
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x10f4;
    memory[0] = 0x8677;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10f4 && !c.store_count);
    puts("C674x sign extension, parallel reads, branch delay, NOP and atomic fault passed");
}
