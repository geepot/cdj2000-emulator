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
static uint32_t mvk(unsigned side, unsigned dst, int value)
{ return dst << 23 | ((uint32_t)value & 0xffff) << 7 | 0x28 | side << 1; }
int main(void)
{
    CdjC674x c;
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = mvk(1, 15, -8);
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.r[1][15] == 0xfffffff8);
    memory[1] = (15u << 23) | (0x1180u << 7) | 0x6a;
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.r[1][15] == 0x1180fff8);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][0] = 7;
    memory[0] = mvk(1, 0, 9) | 1;
    memory[1] = (1u << 23) | (31u << 13) | (1u << 12) | 0xf58;
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.r[0][1] == 7 && c.r[1][0] == 9);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362;
    for (unsigned i = 1; i <= 6; ++i) memory[i] = mvk(0, 0, i);
    for (unsigned i = 0; i < 6; ++i) assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.pc == 0x1040 && c.r[0][0] == 5 && c.cycles == 6);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362; memory[1] = 8u << 13;
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.pc == 0x1040 && c.cycles == 6);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x1234567f;
    memory[0] = (15u << 23) | (15u << 18) | (24u << 13) | 0x9f2;
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.r[1][15] == 0x12345678);
    memory[1] = mvk(0, 3, 99) | (6u << 29);
    memory[2] = mvk(0, 3, 42) | (6u << 29) | (1u << 28);
    assert(cdj_c674x_step(&c, read_word, NULL)); assert(c.r[0][3] == 0);
    assert(cdj_c674x_step(&c, read_word, NULL)); assert(c.r[0][3] == 42);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362;
    memory[1] = (3u << 23) | (3u << 16) | (4u << 13) | 0x162;
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.pc == 0x1040 && c.cycles == 6 && c.r[1][3] == 0x100c);

    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = mvk(0, 0, 99) | 1; memory[1] = 0xffffffff;
    assert(!cdj_c674x_step(&c, read_word, NULL));
    assert(c.r[0][0] == 0 && c.pc == 0x1000 && c.packets == 0);
    /* Header p bits, not opcode bit zero, join compact instructions. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 7; c.r[1][2] = 5;
    uint32_t add = (1u << 13) | (1u << 12) | (2u << 7) | (3u << 4);
    uint32_t sub = (3u << 13) | (1u << 12) | (2u << 7) | (4u << 4) | 0x800;
    memory[0] = add | (sub << 16);
    memory[7] = 0xe0200001; /* word 0 compact; first half parallel */
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.r[0][3] == 12 && c.r[0][4] == (uint32_t)-5);
    assert(c.pc == 0x1004 && c.cycles == 1);

    /* RS applies to both operands and result; sequential halfword PCs. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][17] = 9; c.r[1][18] = 4;
    memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.r[0][19] == 13 && c.pc == 0x1002);
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.r[0][20] == 9 && c.pc == 0x1004);

    /* Full instructions retain their p bit; packets skip the header. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1018);
    memory[6] = mvk(0, 1, 42) | 1;
    memory[7] = 0xe0000000;
    memory[8] = mvk(0, 2, 73);
    assert(cdj_c674x_step(&c, read_word, NULL));
    assert(c.pc == 0x1024 && c.r[0][1] == 42 && c.r[0][2] == 73);

    /* Signed immediate equality, cross path and predicate. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][4] = 0xffffffff;
    memory[0] = (4u << 18) | (31u << 13) | (1u << 12) | 0xa58;
    assert(cdj_c674x_step(&c, read_word, NULL)); assert(c.r[0][0] == 1);
    memory[1] = (6u << 29) | (1u << 23) | (4u << 18) | (1u << 12) | 0xa78;
    assert(cdj_c674x_step(&c, read_word, NULL)); assert(c.r[0][1] == 0);

    /* A later unknown compact instruction rolls back the whole packet. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = mvk(0, 0, 99) | 1;
    memory[1] = 0x3577; memory[7] = 0xe0400000;
    assert(!cdj_c674x_step(&c, read_word, NULL));
    assert(c.fault_pc == 0x1004 && c.fault_word == 0x3577);
    assert(c.r[0][0] == 0 && c.pc == 0x1000 && c.cycles == 0);
    puts("C674x sign extension, parallel reads, branch delay, NOP and atomic fault passed");
}
