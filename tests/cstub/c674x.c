/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"
#include "cdj_c674x_loop.h"
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
    if ((size != 1 && size != 2 && size != 4 && size != 8) || address < 0x1000 ||
        address > 0x1100 - size) return false;
    if (commit) {
        for (unsigned i = 0; i < size; ++i) {
            unsigned offset = address - 0x1000 + i, shift = (offset & 3) * 8;
            memory[offset / 4] = (memory[offset / 4] & ~(255u << shift)) |
                                 ((uint32_t)((value >> (8 * i)) & 255) << shift);
        }
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
    /* Relative branches use fetch-packet base, signed offsets and five
     * delay cycles. A backward target must not use the opcode address. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1028);
    memory[10] = (0x1ffffcu << 7) | 0x10; /* fetch 1020 - 16 = 1010 */
    memory[11] = 8u << 13; /* NOP 9 ends early at the branch */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x102c && c.branch_target == 0x1010);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1010 && c.cycles == 6 && !c.branch_due);
    cdj_c674x_reset(&c, 0x1028);
    memory[10] |= 6u << 29; /* false A0 predicate */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.branch_due && c.pc == 0x102c);

    /* Compact moves in both directions between full and subset registers.
     * Parallel source reads see the old value, including across register files. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][27] = 0xabcdef01; c.r[0][17] = 0x1234;
    uint32_t to = (1u << 13) | (1u << 12) | (3u << 10) | (3u << 7) | 6;
    uint32_t from = (6u << 13) | (3u << 10) | (1u << 7) | 0x46;
    memory[0] = to | from << 16; memory[7] = 0xe0280001;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][17] == 0xabcdef01 && c.r[0][30] == 0x1234);

    /* Firmware's low-bank L and cross-bank D move forms. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[0][0] = 7; c.r[1][3] = 0x11223344;
    memory[0] = 0xb5d62046; memory[7] = 0xe0200001;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][1] == 7 && c.r[0][13] == 0x11223344);
    /* LDW postincrement updates its pointer in E1, samples RAM in E3,
     * and makes its destination visible only after E5. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 0x10c0; c.r[1][2] = 99;
    memory[0] = (2u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x66;
    memory[48] = 123;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][10] == 0x10c4 && c.r[1][2] == 99 && c.load_count == 1);
    memory[48] = 456;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    memory[48] = 789; /* E3 already captured 456 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][2] == 99);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][2] == 456 && !c.load_count && c.cycles == 5);

    /* PROT applies to full-width loads in a header-bearing fetch packet. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][10] = 0x10c8; c.r[1][4] = 2;
    memory[0] = (3u << 23) | (10u << 18) | (4u << 13) | (12u << 9) | 0xe4;
    memory[7] = 0xe0100000; memory[48] = 0x12345678;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][10] == 0x10c0 && c.r[0][3] == 0x12345678 && c.cycles == 5);

    /* False loads cannot touch an unmapped address or modify a pointer. */
    cdj_c674x_reset(&c, 0x1000);
    memory[0] |= 6u << 29; memory[7] = 0;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.load_count && c.r[1][10] == 0);

    /* A parallel decode failure must roll back pointer and load queue. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 0x10c0;
    memory[0] = (10u << 18) | (1u << 13) | (11u << 9) | 0x65;
    memory[1] = 0xffffffff;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][10] == 0x10c0 && !c.load_count && !c.cycles);
    /* An E1 write must not silently overwrite a simultaneous E5 result. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 0x10c0;
    memory[0] = (2u << 23) | (10u << 18) | 0x264;
    memory[4] = mvk(0, 2, 99);
    for (unsigned j = 0; j < 4; ++j)
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 4 && c.load_count == 1 && c.r[0][2] == 0);

    /* A load and store accessing the same address in E3 are unsupported,
     * rather than silently assuming a RAM arbitration order. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = c.r[0][10] = 0x10c0;
    memory[0] = (2u << 23) | (10u << 18) | 0x265;
    memory[1] = 0x3577; memory[7] = 0xe0400000;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.load_count && !c.store_count && !c.cycles && c.r[1][15] == 0x10c0);
    /* Compact BNOP uses halfword displacement and waits on false predicates. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = (5u << 13) | (17u << 6) | 0x2a; /* [A0], +34 bytes */
    memory[7] = 0xe0208000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1002 && c.cycles == 6 && !c.branch_due);
    cdj_c674x_reset(&c, 0x1000); c.r[0][0] = 1;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1022 && c.cycles == 6 && !c.branch_due);

    /* Negative signed offset, B0 inverted predicate and five NOPs. */
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = (5u << 13) | (127u << 6) | 0x3b;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0xffe && c.cycles == 6);

    /* Unsigned 8-bit offset must not sign extend values above 127. */
    cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0xc00a | (200u << 6);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1190 && c.cycles == 6);

    /* Full-width ADD wraps at 32 bits and samples parallel operands. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][4] = 0xffffffff; c.r[0][1] = 2;
    memory[0] = (2u << 23) | (4u << 18) | (1u << 13) | 0x1059;
    memory[1] = (3u << 23) | (4u << 18) | (1u << 13) | 0x1078;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][2] == 0 && c.r[0][3] == 1);
    memory[2] = (4u << 23) | (31u << 13) | 0x58;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][4] == 0xffffffff);
    /* Little-endian byte lanes and signed/unsigned extension. PROT waits
     * for E5, while postincrement scales by the loaded element size. */
    for (unsigned lane = 0; lane < 4; ++lane) {
        for (unsigned sign = 0; sign < 2; ++sign) {
            memset(memory, 0, sizeof(memory));
            cdj_c674x_reset(&c, 0x1000); c.r[1][10] = 0x10c0 + lane;
            memory[48] = 0xff807f01;
            memory[0] = (2u << 23) | (10u << 18) | (1u << 13) |
                        (11u << 9) | (sign ? 0xa4 : 0x94);
            memory[7] = 0xe0100000;
            assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            const uint32_t unsigned_values[] = {1, 127, 128, 255};
            const uint32_t signed_values[] = {1, 127, 0xffffff80, 0xffffffff};
            assert(c.r[0][2] == (sign ? signed_values[lane] : unsigned_values[lane]));
            assert(c.r[1][10] == 0x10c1 + lane && c.cycles == 5);
        }
    }
    for (unsigned sign = 0; sign < 2; ++sign) {
        memset(memory, 0, sizeof(memory));
        cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 0x10c0;
        memory[48] = 0x8001ffff;
        memory[0] = (3u << 23) | (10u << 18) | (1u << 13) |
                    (9u << 9) | (sign ? 0x46 : 0x06);
        memory[7] = 0xe0100000;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[1][3] == (sign ? 0xffff8001u : 0x8001u));
        assert(c.r[0][10] == 0x10c2);
    }
    cdj_c674x_reset(&c, 0x1000); c.r[0][10] = 0x10c1;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.load_count && c.r[0][10] == 0x10c1);

    /* OR's immediate-zero form is the firmware's full-width move alias. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000); c.r[1][4] = 0x87654321;
    memory[0] = 0x04901fd8;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][9] == 0x87654321);
    memory[1] = (2u << 23) | (31u << 13) | 0xfd8;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][2] == 0xffffffff);
    /* Register shifts mask to six bits; counts >=32 must not invoke C UB. */
    const unsigned counts[] = {0, 1, 31, 32, 40, 63, 64};
    const uint32_t right[] = {0x80000001, 0x40000000, 1, 0, 0, 0, 0x80000001};
    const uint32_t arithmetic[] = {0x80000001, 0xc0000000, 0xffffffff, 0xffffffff,
                                  0xffffffff, 0xffffffff, 0x80000001};
    const uint32_t left[] = {0x80000001, 2, 0x80000000, 0, 0, 0, 0x80000001};
    for (unsigned j = 0; j < 7; ++j) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][1] = counts[j]; c.r[1][2] = 0x80000001;
        memory[0] = (3u << 23) | (2u << 18) | (1u << 13) | 0x19e0;
        memory[1] = (4u << 23) | (2u << 18) | (1u << 13) | 0x1de0;
        memory[2] = (5u << 23) | (2u << 18) | (1u << 13) | 0x1ce0;
        for (unsigned k = 0; k < 3; ++k) assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[0][3] == right[j] && c.r[0][4] == arithmetic[j] && c.r[0][5] == left[j]);
    }
    /* STB/STH/STW preserve neighboring bytes and scale pointer increments. */
    for (unsigned size = 1; size <= 4; size *= 2) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][3] = 0x12345678; c.r[1][10] = 0x10c0 + size;
        memory[48] = memory[49] = 0xaabbccdd;
        unsigned op = size == 1 ? 0x34 : size == 2 ? 0x54 : 0x74;
        memory[0] = (3u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x80 | op;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.r[1][10] == 0x10c0 + 2 * size && memory[48] == 0xaabbccdd);
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(memory[48] == (size == 1 ? 0xaabb78dd : size == 2 ? 0x5678ccdd : 0xaabbccdd));
        assert(memory[49] == (size == 4 ? 0x12345678 : 0xaabbccdd));
    }
    /* LDNW joins adjacent little-endian words at every possible alignment. */
    const uint32_t joined[] = {0x44332211, 0x55443322, 0x66554433, 0x77665544};
    for (unsigned lane = 0; lane < 4; ++lane) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[1][10] = 0x10c0 + lane;
        memory[48] = 0x44332211; memory[49] = 0x88776655;
        memory[0] = (2u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x1b4;
        memory[7] = 0xe0100000;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.r[0][2] == joined[lane] && c.r[1][10] == 0x10c4 + lane && c.cycles == 5);
    }
    /* STNW crosses a word boundary without changing surrounding bytes. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = 0x12345678; c.r[1][10] = 0x10c3;
    memory[48] = 0xaabbccdd; memory[49] = 0xeeff0011;
    memory[0] = (3u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x1d4;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][10] == 0x10c7 && memory[48] == 0xaabbccdd);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(memory[48] == 0x78bbccdd && memory[49] == 0xee123456);

    /* A missing second word is detected before modifying the pointer. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][10] = 0x10fd;
    memory[0] = (2u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x1b4;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.load_count && !c.cycles && c.r[1][10] == 0x10fd);

    /* Nonaligned access excludes all other memory operations in its packet. */
    cdj_c674x_reset(&c, 0x1000); c.r[1][10] = 0x10c1; c.r[0][10] = 0x10d0;
    memory[0] |= 1;
    memory[1] = (3u << 23) | (10u << 18) | 0x264;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.load_count && c.r[1][10] == 0x10c1 && !c.cycles);
    /* LDNDW spans three bus words; bit 23 scales the offset, not the
     * destination pair. Both halves appear together in E5. */
    for (unsigned scaled = 0; scaled < 2; ++scaled) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[1][10] = 0x10c1;
        memory[48] = 0x44332211; memory[49] = 0x88776655; memory[50] = 0xccbbaa99;
        memory[0] = (6u << 23) | (scaled << 23) | (10u << 18) |
                    (1u << 13) | (11u << 9) | 0x1a4;
        memory[7] = 0xe0100000;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.r[0][6] == 0x55443322 && c.r[0][7] == 0x99887766);
        assert(c.r[1][10] == 0x10c1 + (scaled ? 8 : 1) && c.cycles == 5);
    }
    /* STNDW samples the pair and preserves bytes on either side. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][6] = 0x44332211; c.r[0][7] = 0x88776655; c.r[1][10] = 0x10c3;
    memory[48] = memory[49] = memory[50] = 0xaaaaaaaa;
    memory[0] = (6u << 23) | (10u << 18) | (1u << 13) | (11u << 9) | 0x1f4;
    for (unsigned j = 0; j < 3; ++j) assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][10] == 0x10c4 && memory[48] == 0x11aaaaaa);
    assert(memory[49] == 0x55443322 && memory[50] == 0xaa887766);

    /* Aligned LDDW scales by eight and rejects odd register-pair indices. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][10] = 0x10b8; memory[48] = 0x12345678; memory[49] = 0x90abcdef;
    memory[0] = (6u << 23) | (10u << 18) | (1u << 13) | (9u << 9) | 0x1e4;
    memory[7] = 0xe0100000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][10] == 0x10c0 && c.r[0][6] == 0x12345678 && c.r[0][7] == 0x90abcdef);
    cdj_c674x_reset(&c, 0x1000); memory[0] |= 1u << 23;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.load_count && !c.cycles);

    /* The high half participates in write-hazard checks. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][10] = 0x10c0;
    memory[0] = (6u << 23) | (10u << 18) | 0x3e4;
    memory[4] = mvk(0, 7, 9);
    for (unsigned j = 0; j < 4; ++j) assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 4 && c.load_count == 1 && c.r[0][7] == 0);
    /* Firmware decrement alias: ADD.S -1, B0, B0. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 1; memory[0] = 0x2003e1a3; memory[1] = 0;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][0] == 0);
    memory[2] = (2u << 23) | (31u << 13) | 0x42; /* MVK.D2 -1,B2 */
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][2] == 0xffffffff);

    /* SUB uses src1-src2, including immediate-first and cross-path forms. */
    const unsigned subops[] = {0xd8, 0x5a0, 0xf8, 0x5e0};
    for (unsigned j = 0; j < 4; ++j) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][1] = 0x80000000; c.r[1][2] = 1;
        memory[0] = (3u << 23) | (2u << 18) | ((j < 2 ? 31u : 1u) << 13) | 0x1000 | subops[j];
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[0][3] == (j < 2 ? 0xfffffffe : 0x7fffffff));
    }
    /* Signed comparison at both extremes, and a negative immediate. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x7fffffff; c.r[1][2] = 0x80000000;
    memory[0] = (3u << 23) | (2u << 18) | (1u << 13) | 0x18f8;
    memory[1] = (4u << 23) | (2u << 18) | (31u << 13) | 0x18d8;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][3] == 1 && c.r[0][4] == 1);
    c.r[1][2] = 0;
    memory[2] = memory[1];
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][4] == 0);
    /* Compact MVC reads B0 (or B16 with RS), with four-cycle availability
     * for the loop engine. Parallel writes still see pre-packet registers. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][0] = 7; memory[0] = mvk(1, 0, 99) | 1;
    memory[1] = 0xd86f; memory[7] = 0xe0400000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[13] == 7 && c.control_ready[13] == 4 && c.r[1][0] == 99);
    cdj_c674x_reset(&c, 0x1004); c.r[1][16] = 123;
    memory[7] |= 1u << 19;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[13] == 123 && c.control_ready[13] == 4);

    /* Full-width MVC to ILC/RILC preserves predicates and source cross path. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][2] = 17;
    memory[0] = (13u << 23) | (2u << 18) | 0x13a2;
    memory[1] = (14u << 23) | (2u << 18) | 0x13a2;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.control[13] == 17 && c.control[14] == 17);
    assert(c.control_ready[13] == 4 && c.control_ready[14] == 5);

    /* Two writes to ILC in one packet cannot silently choose a winner. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = (13u << 23) | 0x3a3;
    memory[1] = 0xd86f; memory[7] = 0xe0400000;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && !c.control_ready[13]);
    /* Execute TI's copy-loop schedule with the real instruction core. The
     * replayed load, move and store share the same pre-cycle register state. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x1080; c.r[1][0] = 0x10c0;
    for (unsigned j = 0; j < 8; ++j) memory[32 + j] = 0x12340000 + j;
    CdjC674xInstruction loop_insns[] = {
        {.word = (2u << 23) | (1u << 18) | (1u << 13) | (11u << 9) | 0x64, .pc = 0x1000},
        {.word = (2u << 23) | (2u << 18) | 0x1fda, .pc = 0x1010},
        {.word = (2u << 23) | (1u << 13) | (11u << 9) | 0xf6, .pc = 0x1014}
    };
    CdjC674xLoop loop;
    assert(cdj_c674x_loop_init(&loop, 1, 8));
    for (unsigned t = 0; t < 17; ++t) {
        uint32_t tag = t == 0 ? 0 : t == 5 ? 1 : 2;
        if (t <= 6)
            assert(cdj_c674x_loop_load(&loop, &tag, (t == 0 || t >= 5) ? 1 : 0, t == 6, 6));
        uint32_t tags[8]; unsigned count; bool post, drained;
        assert(cdj_c674x_loop_issue(&loop, tags, &count, &post, &drained));
        CdjC674xPacket packet = {.count = count, .next_pc = 0x1020};
        for (unsigned j = 0; j < count; ++j) packet.instructions[j] = loop_insns[tags[j]];
        assert(cdj_c674x_execute(&c, &packet, read_word, write_memory, NULL));
    }
    assert(c.r[0][1] == 0x10a0 && c.r[1][0] == 0x10e0);
    for (unsigned j = 0; j < 8; ++j) assert(memory[48 + j] == memory[32 + j]);
    assert(!c.load_count && !c.store_count);

    /* An overlaid fault names its original program PC, not the replay PC,
     * and rolls back earlier instructions in the composite packet. */
    cdj_c674x_reset(&c, 0x1040);
    CdjC674xPacket composite = {.count = 2, .next_pc = 0x1044};
    composite.instructions[0] = (CdjC674xInstruction){.word = mvk(0, 0, 99), .pc = 0x1000};
    composite.instructions[1] = (CdjC674xInstruction){.word = 0xffffffff, .pc = 0x1010};
    assert(!cdj_c674x_execute(&c, &composite, read_word, write_memory, NULL));
    assert(c.fault_pc == 0x1010 && c.pc == 0x1040 && c.r[0][0] == 0 && !c.cycles);
    /* Decode SPLOOP/SPKERNEL from RAM and execute the complete copy loop,
     * without manually feeding the scheduler. Also exercise zero iterations. */
    for (unsigned iterations = 0; iterations <= 8; iterations += 8) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[13] = iterations; c.r[0][1] = 0x1080; c.r[1][0] = 0x10c0;
        for (unsigned j = 0; j < 8; ++j) memory[32 + j] = 0x56780000 + j;
        memory[0] = 0x38000; /* SPLOOP 1 */
        memory[1] = loop_insns[0].word;
        memory[2] = 3u << 13; /* NOP 4 */
        memory[3] = loop_insns[1].word;
        memory[4] = (24u << 22) | 0x34001; /* SPKERNEL 6,0: reversed stage bits */
        memory[5] = loop_insns[2].word;
        memory[6] = mvk(0, 10, 42);
        unsigned steps = 0;
        do {
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(++steps < 30);
        } while (c.loop_active || c.r[0][10] != 42 || c.store_count);
        assert(c.control[13] == 0 && c.r[0][1] == 0x1080 + iterations * 4);
        assert(c.r[1][0] == 0x10c0 + iterations * 4);
        for (unsigned j = 0; j < 8; ++j)
            assert(memory[48 + j] == (iterations ? memory[32 + j] : 0));
    }
    /* ILC availability is enforced before starting a loop. */
    cdj_c674x_reset(&c, 0x1000); c.control_ready[13] = 4;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.loop_active && !c.cycles);

    /* BNOP must insert its NOP cycles even if its predicate is false. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = (6u << 29) | 0x008ca362;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 6 && c.pc == 0x1004 && !c.branch_due);
    puts("C674x sign extension, parallel reads, branch delay, NOP and atomic fault passed");
}
