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
static void test_cycle_tick(void *opaque)
{
    unsigned *ticks = opaque;
    memory[48] = ++*ticks;
}
int main(void)
{
    CdjC674x c;
    /* Board clocks advance on every cycle, including PROT/NOP delays;
     * E3 captures the value on that edge, not the step's final value. */
    unsigned ticks = 0;
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.cycle_tick = test_cycle_tick; c.cycle_opaque = &ticks;
    c.r[0][5] = 0x10c0;
    memory[0] = 0x01940264; memory[7] = 0xe0100000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(ticks == 5 && c.cycles == 5 && c.r[0][3] == 3);
    memory[1] = 7u << 13; /* NOP 8 */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(ticks == 13 && c.cycles == 13);
    memory[2] = 0xffffffff;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(ticks == 13 && c.cycles == 13);
    cdj_c674x_reset(&c, 0x1000);
    assert(!c.cycle_tick && !c.cycle_opaque);
    /* A taken BNOP truncates both inserted NOPs and board ticks. */
    memset(memory, 0, sizeof(memory)); ticks = 0;
    c.cycle_tick = test_cycle_tick; c.cycle_opaque = &ticks;
    memory[0] = (7u << 13) | 0x120;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(ticks == 6 && c.cycles == 6);
    /* PROT in a software loop is equivalent cycle by cycle to LD; NOP 4.
     * Cover overlapping replay (II < 5), masked one-shot loads, and false
     * predicates with invalid addresses. Mutate RAM between E1/E3/E5 to
     * check actual sampling, not merely the final instruction count. */
    for (unsigned ii = 1; ii <= 7; ++ii)
    for (unsigned masked = 0; masked < 2; ++masked)
    for (unsigned enabled = 0; enabled < 2; ++enabled)
    for (unsigned compact = 0; compact < 2; ++compact)
    for (unsigned rs = 0; rs <= compact; ++rs) {
        if (compact && !enabled) continue; /* this compact form is unconditional */
        CdjC674x reference[24];
        for (unsigned prot = 0; prot < 2; ++prot) {
            memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
            ticks = 0; c.cycle_tick = test_cycle_tick; c.cycle_opaque = &ticks;
            c.r[1][1] = 1; c.r[1][0] = enabled;
            c.r[0][5] = enabled ? 0x10c0 : 0xffffffff;
            memory[0] = 0x4003e000 | (ii - 1) << 23; /* [B1] SPLOOPW */
            unsigned slot = 1;
            if (masked) memory[slot++] = 0x430001; /* SPMASK D1 || */
            unsigned load_slot = slot;
            /* [B0] LDW *A5,A3, or compact LDW *A5,A3/A19; NOP 1 */
            memory[slot++] = compact ? 0x0c6e00bc : 0x21940264;
            if (!prot) memory[slot++] = 3u << 13; /* NOP 4 */
            memory[slot++] = 0; /* separate SPKERNEL from multicycle op */
            memory[slot] = 0x34000;
            memory[7] = 0xe0000000 | prot << 20 | rs << 19 |
                        (compact ? 1u << (21 + load_slot) : 0);
            for (unsigned cycle = 0; cycle < 24; ++cycle) {
                assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
                assert(c.cycles == cycle + 1 && !c.idle_cycles);
                assert(ticks == c.cycles);
                if (!prot) reference[cycle] = c;
                else {
                    const CdjC674x *r = &reference[cycle];
                    assert(!memcmp(c.r, r->r, sizeof(c.r)));
                    assert(c.load_count == r->load_count);
                    assert(!memcmp(c.loads, r->loads, c.load_count * sizeof(c.loads[0])));
                    assert(c.loop.cycle == r->loop.cycle && c.loop.length == r->loop.length);
                    assert(c.loop.sealed == r->loop.sealed && c.loop_tags == r->loop_tags);
                    assert(c.loop_pred_history == r->loop_pred_history);
                }
            }
            assert(c.loop_tags == (masked ? 0u : 1u));
            if (!enabled) assert(c.r[0][3 + rs * 16] == 0);
            else if (masked) assert(c.r[0][3 + rs * 16] == 4);
            else assert(c.r[0][3 + rs * 16] > 4);
        }
    }
    /* SPKERNEL cannot share a protected-load packet (SPRUFE8B p481).
     * Nor may this implementation silently combine two multicycle ops. */
    for (unsigned kernel = 0; kernel < 2; ++kernel) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        assert(cdj_c674x_loop_init(&c.loop, 1, 2)); c.loop_active = true;
        c.r[0][5] = 0x10c0;
        memory[0] = kernel ? 0x34001 : 0x01940265;
        memory[1] = kernel ? 0x01940264 : 1u << 13;
        memory[7] = 0xe0100000;
        assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.cycles == 0 && c.loop_tags == 0 && c.loop.length == 0 && !c.load_count);
    }
    /* Full-width immediate BNOP: signed displacement bounds, both units,
     * both fetch layouts and all N counts, with true/false predicates. */
    const int displacements[] = {-2048,-1,0,1,2047};
    for (unsigned layout = 0; layout < 2; ++layout)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned taken = 0; taken < 2; ++taken)
    for (unsigned n = 0; n < 8; ++n)
    for (unsigned d = 0; d < 5; ++d) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1004);
        c.r[1][0] = taken;
        memory[1] = (1u << 29) | (((uint32_t)displacements[d] & 4095) << 16) |
                    (n << 13) | 0x120 | (side << 1);
        if (layout) memory[7] = 0xe0000000;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        unsigned elapsed = taken && n > 5 ? 6 : n + 1;
        assert(c.cycles == elapsed);
        if (taken) {
            while (c.cycles < 6) assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            assert(c.pc == 0x1000u + (uint32_t)(displacements[d] * (layout ? 2 : 4)));
        } else assert(c.pc == 0x1008 && !c.branch_due);
    }
    /* Long-offset scalar loads/stores: all eight opcodes, both data banks,
     * both fixed B bases and displacement boundaries. Reuse real E3/E5 bus. */
    const unsigned long_offsets[] = {0, 1, 31, 256, 32767};
    for (unsigned op = 0; op < 8; ++op)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned y = 0; y < 2; ++y)
    for (unsigned n = 0; n < 5; ++n) {
        unsigned size = op >= 6 ? 4 : (op == 0 || op == 4 || op == 5) ? 2 : 1;
        bool store = op == 3 || op == 5 || op == 7;
        uint32_t base = 0x1080u - long_offsets[n] * size;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[1][14+y] = base; c.r[side][3] = 0xaabbccdd;
        memory[32] = 0x92348081;
        memory[0] = (3u << 23) | (long_offsets[n] << 8) |
                    (y << 7) | (op << 4) | 12 | (side << 1);
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(memory[32] == 0x92348081 && c.r[side][3] == 0xaabbccdd);
        for (unsigned step = 0; step < 4; ++step)
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        if (store) {
            uint32_t expected = size == 1 ? 0x923480dd : size == 2 ? 0x9234ccdd : 0xaabbccdd;
            assert(memory[32] == expected);
        } else {
            const uint32_t results[] = {0x8081,0x81,0xffffff81,0,0xffff8081,0,0x92348081,0};
            assert(c.r[side][3] == results[op]);
        }
        assert(c.r[1][14+y] == base);
    }
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned subset = 0; subset < 2; ++subset)
    for (unsigned op = 0; op < 3; ++op)
    for (unsigned n = 0; n < 32; ++n) {
        unsigned reg = 4 + subset * 16;
        uint32_t source = 0x96a55aa5u;
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][reg] = source;
        memory[0] = ((n & 7) << 13) | ((n >> 3) << 11) |
                    (4u << 7) | (op << 5) | 2 | side;
        memory[7] = 0xe0200000 | (subset << 19);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        uint32_t expected = op == 0 ? (source >> (31-n)) & 1 :
                            op == 1 ? source | (1u << n) : source & ~(1u << n);
        assert(c.r[side][op ? reg : 0] == expected);
        if (!op) assert(c.r[side][reg] == source);
    }
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned subset = 0; subset < 2; ++subset)
    for (unsigned op = 0; op < 4; ++op) {
        const uint32_t expected[] = {0xffff80a5, 0xffffffa5, 0x80a5, 0xa5};
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side][4 + subset*16] = 0x123480a5;
        memory[0] = (5u << 13) | (op << 11) | (4u << 7) | 0x62 | side;
        memory[7] = 0xe0200000 | (subset << 19);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][5 + subset*16] == expected[op]);
    }
    /* Full .S bit-field family: all 1024 parameter pairs, both banks,
     * immediate/register operands and both register cross paths. Expected
     * results use a bit-by-bit oracle rather than the implementation masks. */
    const uint32_t field_ops[] = {0xae0, 0xbe0, 0xee0, 0xfe0};
    for (unsigned op = 0; op < 4; ++op)
    for (unsigned mode = 0; mode < 3; ++mode)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned left = 0; left < 32; ++left)
    for (unsigned right = 0; right < 32; ++right) {
        uint32_t source = 0xa5367e91u, expected = 0;
        for (unsigned bit = 0; bit < 32; ++bit) {
            unsigned set;
            if (op < 2) {
                unsigned from = bit + right;
                if (from >= 32) set = op == 1 ? (source >> (31-left)) & 1 : 0;
                else set = from >= left ? (source >> (from-left)) & 1 : 0;
            } else {
                set = (source >> bit) & 1;
                if (bit >= left && bit <= right) set = op == 2;
            }
            expected |= (uint32_t)set << bit;
        }
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[side ^ (mode == 2)][2] = source;
        c.r[side][3] = left * 32 + right;
        memory[0] = (4u << 23) | (2u << 18) | (side << 1) |
            (mode == 0 ? (left << 13) | (right << 8) | (op << 6) | 8 :
             (3u << 13) | ((mode == 2) << 12) | field_ops[op]);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][4] == expected && c.cycles == 1);
    }
    for (unsigned op = 0; op < 4; ++op) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][3] = 1024;
        memory[0] = (4u << 23) | (2u << 18) | (3u << 13) | field_ops[op];
        assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 0 && c.r[0][4] == 0);
        cdj_c674x_reset(&c, 0x1000); c.r[0][3] = 1024;
        memory[0] |= 2u << 29; /* false [B1] does not fault on unused count */
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[0][4] == 0);
    }
    /* OR/XOR on all three units, immediates/registers, banks and cross paths. */
    const unsigned logic_ops[] = {0xfd8,0xff8,0x6a0,0x6e0,0x8f0,0x8b0,
                                  0xdd8,0xdf8,0x2a0,0x2e0,0xbf0,0xbb0};
    for (unsigned op = 0; op < 12; ++op)
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned cross = 0; cross < 2; ++cross) {
                cdj_c674x_reset(&c, 0x1000);
                c.r[side][31] = 0x12345678; c.r[side ^ cross][5] = 0x87654321;
                CdjC674xPacket p = {.count=1, .next_pc=0x1004,
                    .instructions={{.pc=0x1000, .word=6u<<23 | 5u<<18 | 31u<<13 |
                        cross<<12 | logic_ops[op] | side<<1}}};
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                uint32_t a = op & 1 ? 0x12345678 : UINT32_MAX;
                assert(c.r[side][6] == (op < 6 ? (a | 0x87654321) : (a ^ 0x87654321)));
            }
    /* PROT/BR are fetch-header selectors, not attributes of every opcode. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[13] = 1;
    memory[0] = 0x38000; memory[1] = mvk(0, 4, 99);
    memory[2] = 0x0c6e0012; /* compact MVK 0,A0; NOP */
    memory[3] = 0x34000; memory[7] = 0xe0908000; /* PROT, BR, compact slot 2 */
    unsigned header_steps = 0;
    do {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(++header_steps < 12);
    } while (c.loop_active);
    assert(c.r[0][4] == 99);
    /* Address arithmetic families: unsigned constants, signed-register
     * bit patterns, scaling and modular wrap, on both banks. */
    for (unsigned op = 0x30; op <= 0x3d; ++op)
        for (unsigned side = 0; side < 2; ++side) {
            cdj_c674x_reset(&c, 0x1000);
            c.r[side][5] = 2; c.r[side][31] = 0xffffffff;
            memory[0] = 6u << 23 | 5u << 18 | 31u << 13 | op << 7 | 0x40 | side << 1;
            uint32_t offset = ((op >= 0x3c ? op & 1 : op & 2) ? 31u : 0xffffffffu) * (1u << ((op - 0x30) / 4));
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(c.r[side][6] == ((op < 0x3c && (op & 1)) ? 2 - offset : 2 + offset));
        }
    /* Complete scalar ADD/SUB .D family: same-bank register/unsigned
     * constant forms and cross-path register/signed-constant forms. */
    for (unsigned op = 0x10; op <= 0x13; ++op)
        for (unsigned side = 0; side < 2; ++side) {
            cdj_c674x_reset(&c, 0x1000);
            c.r[side][3] = 5; c.r[side][6] = 2;
            memory[0] = 1u << 23 | 6u << 18 | 3u << 13 |
                        op << 7 | 0x40 | side << 1;
            assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            uint32_t right = op & 2 ? 3 : 5;
            assert(c.r[side][1] == ((op & 1) ? 2 - right : 2 + right));
        }
    const unsigned d_cross_ops[] = {0xab0, 0xaf0, 0xb30};
    for (unsigned n = 0; n < 3; ++n)
        for (unsigned side = 0; side < 2; ++side) {
            cdj_c674x_reset(&c, 0x1000);
            c.r[side][3] = 0xfffffffdu;
            c.r[side ^ 1][6] = 5;
            memory[0] = 1u << 23 | 6u << 18 | 3u << 13 | 1u << 12 |
                        d_cross_ops[n] | side << 1;
            assert(cdj_c674x_step(&c, read_word, NULL, NULL));
            uint32_t left = n == 1 ? 3 : 0xfffffffdu;
            assert(c.r[side][1] == (n == 2 ? left - 5 : left + 5));
        }
    /* The captured [A0] SUB .D1 A3,A6,A1 is atomic when false and wraps
     * exactly like 32-bit C674x integer arithmetic when true. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 99; c.r[0][3] = 5; c.r[0][6] = 2;
    memory[0] = 0xc09868c0;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][1] == 99);
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][0] = 1; c.r[0][3] = 5; c.r[0][6] = 2;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][1] == UINT32_MAX - 2);
    /* CMPLTU scalar forms compare unsigned values, including the captured
     * CMPLTU .L1 15,A1,A0 at 0x118044a4. */
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
            for (unsigned immediate = 0; immediate < 2; ++immediate) {
                cdj_c674x_reset(&c, 0x1000);
                c.r[side][3] = 15;
                c.r[side ^ cross_path][6] = 16;
                memory[0] = 1u << 23 | 6u << 18 | 3u << 13 |
                            cross_path << 12 | (immediate ? 0xbd8 : 0xbf8) |
                            side << 1;
                assert(cdj_c674x_step(&c, read_word, NULL, NULL));
                assert(c.r[side][1] == 1);
                cdj_c674x_reset(&c, 0x1000);
                c.r[side][1] = 99;
                memory[0] |= 6u << 29; /* false A0 predicate */
                assert(cdj_c674x_step(&c, read_word, NULL, NULL));
                assert(c.r[side][1] == 99);
            }
    /* Figure H-5 / GNU nfu_uspl: compact SPLOOP scatters ii-1 across
     * bits 9:7 and 14 and shares the full-width loop scheduler. */
    for (unsigned ii = 1; ii <= 14; ++ii) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[13] = 1;
        unsigned encoded = ii - 1;
        memory[0] = 0x1c660000 | 0x0c66 |
                    (encoded & 7) << 7 | (encoded & 8) << 11;
        memory[7] = 0xe0200000; /* compact SPLOOP; compact SPKERNEL 0,0 */
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.loop_active && c.loop.ii == ii && c.control[13] == 0);
        unsigned steps = 0;
        while (c.loop_active) {
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(++steps < 64);
        }
    }
    /* H-5 op=1 and H-6 are SPLOOPD, not Appendix-G ALU forms. Recognize
     * and reject their four-cycle delayed testing until that scheduler
     * behavior is modeled; the setup packet remains atomic. */
    const unsigned compact_sploopd[] = {0x0c67, 0x8c66, 0x8c67};
    for (unsigned i = 0; i < 3; ++i) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[13] = 3; c.r[0][1] = 99;
        memory[0] = compact_sploopd[i]; memory[7] = 0xe0200000;
        assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.cycles == 0 && !c.loop_active && c.r[0][1] == 99);
        assert(!strcmp(c.fault, "SPLOOPD delayed testing not implemented"));
    }
    /* More than 14 source packets fit when they occupy no functional slots. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[13] = 1;
    memory[0] = 0x06838000; /* SPLOOP 14 */
    memory[21] = 0x34000;
    unsigned long_body_steps = 0;
    do {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(++long_body_steps < 50);
    } while (c.loop_active);
    assert(c.loop_packets == 21 && c.loop_tags == 0);
    /* Figure G-3 predicate MVK: all predicate polarities, units, sides,
     * register subsets and constants. Predicates always use low A0/B0. */
    for (unsigned cc = 0; cc < 4; ++cc)
        for (unsigned unit = 0; unit < 3; ++unit)
            for (unsigned side = 0; side < 2; ++side)
                for (unsigned rs = 0; rs < 2; ++rs)
                    for (unsigned value = 0; value < 2; ++value)
                        for (unsigned predicate = 0; predicate < 2; ++predicate) {
                            cdj_c674x_reset(&c, 0x1000);
                            c.r[cc >> 1][0] = predicate;
                            c.r[side][3 + 16 * rs] = 55;
                            CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
                                .instructions = {{.compact = true, .pc = 0x1000,
                                    .header = rs << 19, .word = 0x0866 | cc << 14 |
                                        value << 13 | 3u << 7 | unit << 3 | side}}};
                            assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                            assert(c.r[side][3 + 16 * rs] == ((predicate ^ (cc & 1)) ? value : 55));
                        }
    /* SPMASK suppresses an existing buffered S1 write before merging a
     * program-memory replacement. Exercise full and compact encodings. */
    for (unsigned compact = 0; compact < 2; ++compact) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.control[13] = 3;
        memory[0] = 0x38000;
        memory[1] = 4u << 23 | 4u << 18 | 1u << 13 | 0x1a0; /* ADD.S1 1,A4,A4 */
        memory[2] = compact ? 0x0c6e2d66 : 0x130001; /* SPMASK S1 */
        memory[3] = mvk(0, 4, 100);
        memory[4] = 0x34000;
        if (compact) memory[7] = 0xe0800030; /* SPMASK || NOP || MVK */
        for (unsigned j = 0; j < 4; ++j)
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.r[0][4] == 101 && c.loop_tags == 1);
        for (unsigned j = 0; j < 3 && c.loop_active; ++j)
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(!c.loop_active && c.r[0][4] == 101);
    }
    /* During the epilog, a program-memory SPMASK replaces a draining S1
     * operation without changing the buffer; it executes again next cycle. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.control[13] = 3;
    memory[0] = 0x38000; memory[1] = 2u << 13; /* SPLOOP 1; NOP 3 */
    memory[2] = 0x34001;
    memory[3] = 4u << 23 | 4u << 18 | 1u << 13 | 0x1a0;
    memory[4] = 0x130001; memory[5] = mvk(0, 4, 100);
    unsigned mask_steps = 0;
    do {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(++mask_steps < 12);
    } while (c.loop_active);
    assert(c.r[0][4] == 101);
    /* Zero masks and idle masks are legal; a misplaced mask is atomic. */
    for (unsigned compact = 0; compact < 2; ++compact) {
        cdj_c674x_reset(&c, 0x1000);
        CdjC674xPacket p = {.count = 2, .next_pc = 0x1008,
            .instructions = {{.pc = 0x1000, .compact = compact,
                               .word = compact ? 0x2c66 : 0x30000},
                              {.pc = 0x1004, .word = mvk(0, 4, 99)}}};
        assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
        assert(c.r[0][4] == 99);
        CdjC674xInstruction first = p.instructions[0];
        p.instructions[0] = p.instructions[1]; p.instructions[1] = first;
        cdj_c674x_reset(&c, 0x1000);
        assert(!cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
        assert(c.r[0][4] == 0 && c.cycles == 0);
    }
    /* All compact L/S/D mask bits: masked moves execute once; unmasked
     * moves remain buffered, including the other side of the same unit. */
    for (unsigned unit = 0; unit < 3; ++unit)
        for (unsigned side = 0; side < 2; ++side) {
            static const unsigned bits[] = {1,128,256,512,16384,32768};
            memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
            c.control[13] = 3; c.r[side][1] = 99; c.r[side ^ 1][1] = 77;
            memory[0] = 0x838000; /* SPLOOP 2 */
            unsigned move = 0x46 | 4u << 13 | 1u << 7 | unit << 3;
            memory[1] = (move | side) << 16 | 0x2c66 | bits[2 * unit + side];
            memory[2] = (move | (side ^ 1)) | 0x0c6e0000;
            memory[3] = 0x34000;
            memory[7] = 0xe0c0000c; /* mask || move || opposite move */
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(c.r[side][4] == 99 && c.r[side ^ 1][4] == 77 && c.loop_tags == 1);
            c.r[side][1] = 55; c.r[side ^ 1][1] = 66;
            unsigned steps = 0;
            while (c.loop_active) {
                assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
                assert(++steps < 12);
            }
            assert(c.r[side][4] == 99 && c.r[side ^ 1][4] == 66);
        }
    /* Figure F-24 scatters an unsigned byte across four fields. Exercise all
     * constants, both banks and register subsets (including high-bit values). */
    for (unsigned rs = 0; rs < 2; ++rs)
        for (unsigned bank = 0; bank < 2; ++bank)
            for (unsigned k = 0; k < 256; ++k) {
                cdj_c674x_reset(&c, 0x1000);
                unsigned dst = k & 7;
                CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
                    .instructions = {{.compact = true, .pc = 0x1000,
                        .header = rs << 19,
                        .word = 0x12 | bank | dst << 7 | (k & 7) << 13 |
                            ((k >> 3) & 3) << 11 | ((k >> 5) & 3) << 5 |
                            ((k >> 7) & 1) << 10}}};
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                assert(c.r[bank][dst + rs * 16] == k && c.cycles == 1);
                assert(c.r[bank ^ 1][dst + rs * 16] == 0);
            }
    for (unsigned k = 0; k < 16; ++k) {
        cdj_c674x_reset(&c, 0x1000);
        c.r[0][20] = 0xffffffff;
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
            .instructions = {{.compact = true, .pc = 0x1000, .header = 1u << 19,
                .word = 0x1441 | (k & 7) << 13 | (k >> 3) << 11 | 4u << 7}}};
        int32_t offset = k & 8 ? (int32_t)(k & 7) - 8 : (k ? (int32_t)k : 8);
        assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
        assert(c.r[1][20] == 0xffffffffu + (uint32_t)offset);
        assert(c.r[0][20] == 0xffffffff);
    }
    /* Appendix H compact NOP stores cycles - 1 in N3. In particular the
     * firmware blocker 0xec6e is NOP 8, not a one-cycle empty operation. */
    for (unsigned n3 = 0; n3 < 8; ++n3) {
        cdj_c674x_reset(&c, 0x1000);
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
            .instructions = {{.compact = true, .pc = 0x1000,
                .word = 0x0c6e | n3 << 13}}};
        assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
        assert(c.cycles == n3 + 1 && c.pc == 0x1002);
    }
    /* Compact immediate-offset transfers share E3/E5 timing with full words.
     * Cover all header size selections and both register subsets. */
    for (unsigned rs = 0; rs < 2; ++rs)
        for (unsigned dsz = 0; dsz < 8; ++dsz)
            for (unsigned secondary = 0; secondary < 2; ++secondary) {
                static const unsigned sizes[8] = {1,1,2,2,4,1,4,2};
                unsigned size = secondary ? sizes[dsz] : (dsz & 4 ? 8 : 4);
                memset(memory, 0, sizeof(memory));
                cdj_c674x_reset(&c, 0x1000);
                c.r[1][4] = 0x1040; c.r[0][2 + rs * 16] = 0x87654321;
                c.r[0][3 + rs * 16] = 0x12345678;
                CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
                    .instructions = {{.compact = true, .pc = 0x1000,
                        .header = rs << 19 | dsz << 16,
                        .word = 0x25 | secondary << 9 | 1u << 13}}};
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                assert(memory[16] == 0 && memory[17] == 0 && memory[18] == 0);
                p.count = 0;
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                assert(memory[(0x40 + size) / 4] != 0);
                c.r[0][2 + rs * 16] = 0;
                c.r[0][3 + rs * 16] = 0;
                p.count = 1; p.instructions[0].word |= 8;
                p.instructions[0].header |= 1u << 20; /* PROT drains load. */
                assert(cdj_c674x_execute(&c, &p, read_word, write_memory, NULL));
                uint32_t expected = size == 1 ? 0x21 : size == 2 ? 0x4321 : 0x87654321;
                assert(c.r[0][2 + rs * 16] == expected);
                if (size == 8) assert(c.r[0][3 + rs * 16] == 0x12345678);
                assert(c.r[1][4] == 0x1040);
            }
    /* Doff4DW nonaligned offsets are bytes, with an even register pair. */
    memset(memory, 0, sizeof(memory));
    cdj_c674x_reset(&c, 0x1000);
    c.r[1][4] = 0x1040; c.r[0][2] = 0x88776655; c.r[0][3] = 0xccbbaa99;
    CdjC674xPacket compact_mem = {.count = 1, .next_pc = 0x1002,
        .instructions = {{.compact = true, .pc = 0x1000, .header = 4u << 16,
            .word = 0x6035}}};
    assert(cdj_c674x_execute(&c, &compact_mem, read_word, write_memory, NULL));
    compact_mem.count = 0;
    assert(cdj_c674x_execute(&c, &compact_mem, read_word, write_memory, NULL));
    assert(cdj_c674x_execute(&c, &compact_mem, read_word, write_memory, NULL));
    assert(memory[16] == 0x55000000 && memory[17] == 0x99887766 && memory[18] == 0x00ccbbaa);
    compact_mem.count = 1; compact_mem.instructions[0].word |= 8;
    compact_mem.instructions[0].header |= 1u << 20;
    c.r[0][2] = c.r[0][3] = 0;
    assert(cdj_c674x_execute(&c, &compact_mem, read_word, write_memory, NULL));
    assert(c.r[0][2] == 0x88776655 && c.r[0][3] == 0xccbbaa99);
    cdj_c674x_reset(&c, 0x1000); c.r[1][4] = 0x10fc;
    assert(!cdj_c674x_execute(&c, &compact_mem, read_word, write_memory, NULL));
    assert(c.fault_word == 0x603d && c.cycles == 0);
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

    /* Scalar .L comparisons share adjacent immediate/register opfields.
     * The signed relations sign-extend scst5; unsigned relations do not. */
    const unsigned compare_immediate[] = {0xa58, 0x8d8, 0x9d8, 0xad8, 0xbd8};
    const unsigned compare_expected[] = {0, 0, 1, 1, 0};
    for (unsigned relation = 0; relation < 5; ++relation)
    for (unsigned immediate = 0; immediate < 2; ++immediate)
    for (unsigned side = 0; side < 2; ++side) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        unsigned left = immediate ? 31 : 3, right = 4, dst = 5;
        c.r[side][left] = 0xffffffff;
        c.r[side ^ 1][right] = 1;
        memory[0] = dst << 23 | right << 18 | left << 13 | 1u << 12 |
                    compare_immediate[relation] | ((!immediate) << 5) | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == compare_expected[relation]);
    }

    /* INTSP/INTSPU form one four-cycle .L conversion family.  Results and
     * sticky FADCR INEX appear in E4; all rounding is integer-derived so the
     * tests do not depend on the host floating-point environment. */
    struct IntSpCase {
        uint32_t source, expected;
        unsigned rmode;
        bool unsigned_source, inexact;
    } int_sp_cases[] = {
        {0,          0x00000000, 0, false, false},
        {1,          0x3f800000, 0, false, false},
        {0xffffffff, 0xbf800000, 0, false, false},
        {0x80000000, 0xcf000000, 0, false, false},
        {0x7fffffff, 0x4f000000, 0, false, true},
        {0x7fffffff, 0x4effffff, 1, false, true},
        {0x7fffffff, 0x4f000000, 2, false, true},
        {0x7fffffff, 0x4effffff, 3, false, true},
        {0xffffffff, 0x4f800000, 0, true,  true},
        {0xffffffff, 0x4f7fffff, 1, true,  true},
        {0xffffffff, 0x4f800000, 2, true,  true},
        {0xffffffff, 0x4f7fffff, 3, true,  true},
        {0x01000001, 0x4b800000, 0, false, true},
        {0x01000003, 0x4b800002, 0, false, true},
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(int_sp_cases) / sizeof(int_sp_cases[0]); ++j) {
        struct IntSpCase tc = int_sp_cases[j];
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        unsigned dst = 4, src = 3, shift = side ? 25 : 9;
        c.r[side][dst] = 0xdeadbeef;
        c.r[side ^ cross_path][src] = tc.source;
        c.control[18] = tc.rmode << shift;
        memory[0] = dst << 23 | src << 18 | cross_path << 12 |
                    (tc.unsigned_source ? 0x938 : 0x958) | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.cycles == 1 && c.load_count == 1 &&
               c.loads[0].due == 4 && c.loads[0].size == 0 &&
               c.r[side][dst] == 0xdeadbeef &&
               c.control[18] == tc.rmode << shift);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == 0xdeadbeef);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == tc.expected && !c.load_count);
        assert(c.control[18] == ((tc.rmode << shift) |
               (tc.inexact ? 1u << (side ? 23 : 7) : 0)));
    }

    /* MPYSP uses FMCR, flushes denormalized operands and underflow outputs as
     * specified by TI, and implements all four directed rounding modes. */
    struct MpySpCase {
        uint32_t left, right, expected, status;
        unsigned rmode;
    } mpy_sp_cases[] = {
        {0x3fc00000, 0x40000000, 0x40400000, 0x000, 0}, /* 1.5 * 2 */
        {0xc0200000, 0x4109999a, 0xc1ac0000, 0x080, 0}, /* TI -2.5 * 8.6 */
        {0x7fc00001, 0x40000000, 0x7fffffff, 0x001, 0}, /* QNaN */
        {0x7f800001, 0x40000000, 0x7fffffff, 0x011, 0}, /* SNaN */
        {0x7f800000, 0x40000000, 0x7f800000, 0x020, 0}, /* infinity */
        {0x7f800000, 0x00000000, 0x7fffffff, 0x010, 0}, /* infinity * zero */
        {0x80000001, 0x40000000, 0x80000000, 0x084, 0}, /* denormal * normal */
        {0x00000001, 0x00000000, 0x00000000, 0x004, 0}, /* denormal * zero */
        {0x7f800000, 0x00000001, 0x7fffffff, 0x018, 0}, /* infinity * denormal */
        {0x7f7fffff, 0x40000000, 0x7f800000, 0x0e0, 0}, /* overflow nearest */
        {0x7f7fffff, 0x40000000, 0x7f7fffff, 0x0c0, 1}, /* overflow truncate */
        {0xff7fffff, 0x40000000, 0xff7fffff, 0x0c0, 2}, /* negative toward +inf */
        {0xff7fffff, 0x40000000, 0xff800000, 0x0e0, 3}, /* negative toward -inf */
        {0x00800000, 0x3f000000, 0x00000000, 0x180, 0}, /* underflow nearest */
        {0x00800000, 0x3f000000, 0x00800000, 0x180, 2}, /* underflow upward */
        {0x80800000, 0x3f000000, 0x80800000, 0x180, 3}, /* underflow downward */
        {0x3f800001, 0x3f800001, 0x3f800002, 0x080, 0}, /* rounded nearest */
        {0x3f800001, 0x3f800001, 0x3f800003, 0x080, 2}, /* rounded upward */
    };
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(mpy_sp_cases) / sizeof(mpy_sp_cases[0]); ++j) {
        struct MpySpCase tc = mpy_sp_cases[j];
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        unsigned dst = 5, left = 3, right = 4, shift = side ? 16 : 0;
        c.r[side][left] = tc.left;
        c.r[side ^ cross_path][right] = tc.right;
        c.r[side][dst] = 0xdeadbeef;
        c.control[20] = tc.rmode << (shift + 9);
        memory[0] = dst << 23 | right << 18 | left << 13 |
                    cross_path << 12 | 0xe00 | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.load_count == 1 && c.loads[0].due == 4 &&
               !c.loads[0].size && c.loads[0].sign_extend &&
               c.r[side][dst] == 0xdeadbeef);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == tc.expected && !c.load_count);
        assert(c.control[20] == ((tc.rmode << (shift + 9)) |
                                (tc.status << shift)));
        assert(!c.control[18]);
    }

    /* SPINT follows FADCR rounding and SPTRUNC ignores it.  NaNs, infinity,
     * denormals, overflow, ties and signed directed rounding set the exact
     * documented sticky status bits with the E4 result. */
    struct SpIntCase {
        uint32_t source, expected, status;
        unsigned rmode;
    } sp_int_cases[] = {
        {0x4109999a, 9,          0x080, 0}, /* 8.6 nearest */
        {0x4109999a, 8,          0x080, 1},
        {0x4109999a, 9,          0x080, 2},
        {0x4109999a, 8,          0x080, 3},
        {0xc109999a, 0xfffffff7, 0x080, 0}, /* -8.6 nearest */
        {0xc109999a, 0xfffffff8, 0x080, 1},
        {0xc109999a, 0xfffffff8, 0x080, 2},
        {0xc109999a, 0xfffffff7, 0x080, 3},
        {0x3f000000, 0,          0x080, 0}, /* 0.5 ties to even */
        {0x3fc00000, 2,          0x080, 0}, /* 1.5 ties to even */
        {0x40200000, 2,          0x080, 0}, /* 2.5 ties to even */
        {0x4effffff, 0x7fffff80, 0x000, 0}, /* largest exact in-range SP */
        {0xcf000000, 0x80000000, 0x000, 0}, /* -2^31 is valid */
        {0x4f000000, 0x7fffffff, 0x0c0, 0}, /* positive overflow */
        {0xcf000001, 0x80000000, 0x0c0, 0}, /* negative overflow */
        {0x7f800000, 0x7fffffff, 0x0c0, 0}, /* infinity */
        {0xff800000, 0x80000000, 0x0c0, 0},
        {0x7fc00001, 0x7fffffff, 0x012, 0}, /* NaN */
        {0xffc00001, 0x80000000, 0x012, 0},
        {0x00000001, 0,          0x088, 0}, /* denormal */
        {0x80000000, 0,          0x000, 0}, /* negative zero */
    };
    for (unsigned truncate = 0; truncate < 2; ++truncate)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross_path = 0; cross_path < 2; ++cross_path)
    for (unsigned j = 0; j < sizeof(sp_int_cases) / sizeof(sp_int_cases[0]); ++j) {
        struct SpIntCase tc = sp_int_cases[j];
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        unsigned dst = 5, src = 4, shift = side ? 16 : 0;
        /* For SPTRUNC, derive the expected finite rounded value by selecting
         * the matching toward-zero row when this table has mode variants. */
        uint32_t expected = tc.expected;
        if (truncate && j < 4) expected = 8;
        if (truncate && j >= 4 && j < 8) expected = 0xfffffff8;
        if (truncate && (j == 9 || j == 10)) expected = j == 9 ? 1 : 2;
        c.r[side ^ cross_path][src] = tc.source;
        c.r[side][dst] = 0xdeadbeef;
        c.control[18] = tc.rmode << (shift + 9);
        memory[0] = dst << 23 | src << 18 | cross_path << 12 |
                    (truncate ? 0x178 : 0x158) | side << 1;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.load_count == 1 && c.loads[0].due == 4 &&
               !c.loads[0].size && !c.loads[0].sign_extend);
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[side][dst] == expected && !c.load_count);
        assert(c.control[18] == ((tc.rmode << (shift + 9)) |
                                (tc.status << shift)));
    }

    /* False predicates create no delayed result or warning; an unsupported
     * parallel instruction rolls the whole issue packet back. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 99; c.r[0][3] = 0x7fffffff;
    memory[0] = 2u << 29 | 4u << 23 | 3u << 18 | 0x958;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][4] == 99 && !c.load_count && !c.control[18]);
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 99; c.r[0][3] = 7;
    memory[0] = 4u << 23 | 3u << 18 | 0x959;
    memory[1] = 0xffffffff;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && c.r[0][4] == 99 && !c.load_count && !c.control[18]);

    /* A same-cycle E1 write to an E4 destination is rejected before either
     * operation commits; the pending conversion remains checkpointable. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[0][4] = 99; c.r[0][3] = 7;
    memory[0] = 4u << 23 | 3u << 18 | 0x958;
    memory[3] = mvk(0, 4, 1);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 3 && c.r[0][4] == 99 && c.load_count == 1 &&
           c.loads[0].size == 0 && c.loads[0].value == 0x40e00000);

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

    /* The paired Dpp loads pre-increment B15, sample RAM in E3 and publish
     * word/doubleword destinations in E5. Register side comes from t; RS is
     * ignored just as it is for the store forms. */
    memory[60] = 0x13579bdf; memory[62] = 0x2468ace0;
    memory[63] = 0xfdb97531;
    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x10e8;
    memory[0] = 0xc17771f7; memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10f0 && c.r[1][3] == 0 && c.load_count == 1);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10f8 && c.r[0][2] == 0 && c.load_count == 2);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][3] == 0 && c.r[0][2] == 0);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][3] == 0 && c.r[0][2] == 0);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][3] == 0x13579bdf && c.r[0][2] == 0);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[0][2] == 0x2468ace0 && c.r[0][3] == 0xfdb97531 &&
           !c.load_count);

    /* Dstk uses B15 plus an unsigned scaled five-bit constant without base
     * update. Cover captured STW *+B15[1],B4 and the matching RS load. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][15] = 0x10e8; c.r[1][4] = 0xa5a55a5a;
    memory[0] = 0xbc45bc4d; memory[7] = 0xe0200000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][15] == 0x10e8 && c.load_count == 1 && c.store_count == 0);
    for (unsigned j = 0; j < 4; ++j)
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    /* The following store captures the old B4 in E1 before the load's E5
     * writeback, and commits it one cycle before that writeback. */
    assert(c.r[1][4] == 0 && c.r[1][15] == 0x10e8 &&
           !c.load_count && !c.store_count);
    assert(memory[(0x10ec - 0x1000) / 4] == 0xa5a55a5a);

    cdj_c674x_reset(&c, 0x1000); c.r[1][15] = 0x10e8;
    memory[(0x10ec - 0x1000) / 4] = 0xface1234;
    memory[0] = 0xbc4d; memory[7] = 0xe0280000 | (1u << 19);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    for (unsigned j = 0; j < 4; ++j)
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][20] == 0xface1234 && c.r[1][4] == 0 &&
           c.r[1][15] == 0x10e8);

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

    /* Figures C-8 through C-15 share the compact .D transfer layout.
     * Exercise every DSZ scalar interpretation plus aligned/nonaligned
     * doublewords across immediate, register, postincrement and predecrement
     * addressing. RS applies to data/register offsets but not A/B4-7 ptrs. */
    struct CompactMemoryCase {
        unsigned dsz, sz, na, size;
        bool pair, sign_extend;
    } compact_memory_cases[] = {
        {0, 0, 0, 4, false, false},
        {0, 1, 0, 1, false, false}, {1, 1, 0, 1, false, true},
        {2, 1, 0, 2, false, false}, {3, 1, 0, 2, false, true},
        {4, 1, 0, 4, false, false}, {5, 1, 0, 1, false, true},
        {6, 1, 0, 4, false, false}, {7, 1, 0, 2, false, true},
        {4, 0, 0, 8, true, false}, {4, 0, 1, 8, true, false},
    };
    for (unsigned family = 0; family < 4; ++family)
    for (unsigned load = 0; load < 2; ++load)
    for (unsigned j = 0; j < sizeof(compact_memory_cases) /
                                    sizeof(compact_memory_cases[0]); ++j) {
        struct CompactMemoryCase tc = compact_memory_cases[j];
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        unsigned reg_field = tc.pair ? 2 : 3;
        unsigned reg = reg_field + 16; /* header RS=1 */
        unsigned scale = tc.size;
        if (tc.pair && tc.na && family < 2) scale = 1; /* C-9/C-11 */
        uint32_t target = tc.na ? 0x10c1 : 0x10c0;
        uint32_t base = family == 3 ? target + 2 * scale : target;
        uint32_t address = family < 2 ? base + 2 * scale : target;
        c.r[0][5] = base; c.r[0][18] = 2;
        c.r[1][reg] = 0x12345678;
        if (tc.pair) c.r[1][reg + 1] = 0x9abcdef0;
        unsigned fixed = family == 0 ? 0x0004 : family == 1 ? 0x0404 :
                         family == 2 ? 0x0c04 : 0x4c04;
        unsigned offset_field = family < 2 ? 2 : 1; /* offset 2 / ucst0=1 */
        uint32_t word = fixed | offset_field << 13 | 1u << 12 |
                        tc.sz << 9 | 1u << 7 | reg_field << 4 |
                        tc.na << 4 | load << 3;
        memory[0] = word | 0x0c6e0000;
        memory[7] = 0xe0200000 | tc.dsz << 16 | 1u << 19;
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(c.r[0][5] == (family < 2 ? base :
                             family == 2 ? base + 2 * scale : target));
        if (load) {
            assert(c.load_count == 1 && !c.store_count);
            assert(c.loads[0].address == address && c.loads[0].size == tc.size);
            assert(c.loads[0].bank == 1 && c.loads[0].dst == reg);
            assert(c.loads[0].sign_extend == tc.sign_extend);
        } else {
            assert(c.store_count == 1 && !c.load_count);
            assert(c.stores[0].address == address && c.stores[0].size == tc.size);
            assert(c.stores[0].value == (tc.pair ?
                UINT64_C(0x9abcdef012345678) : UINT64_C(0x12345678)));
        }
    }

    /* The genuine next word is STDW .D2 B5:B4,*B6[2]++.  It updates B6
     * in E1, captures both old source registers and commits the pair in E3. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][4] = 0x11223344; c.r[1][5] = 0xaabbccdd; c.r[1][6] = 0x10c0;
    memory[0] = 0x0c6e3d45; memory[7] = 0xe0240000;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][6] == 0x10d0 && c.store_count == 1 && memory[48] == 0);
    c.r[1][4] = c.r[1][5] = 0;
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(memory[48] == 0x11223344 && memory[49] == 0xaabbccdd &&
           !c.store_count);

    /* A later packet failure rolls compact base updates and queues back. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][6] = 0x10c0; c.r[1][4] = 0x12345678;
    memory[0] = 0xffff3d45; memory[7] = 0xe0240001;
    assert(!cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[1][6] == 0x10c0 && !c.store_count && !c.cycles);
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
    /* SPLOOPW records its predicate even if initially false, then checks
     * the value three cycles before an end-of-stage boundary. ILC is unused. */
    for (unsigned ii = 1; ii <= 14; ++ii)
        for (unsigned invert = 0; invert < 2; ++invert) {
            memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
            c.control[13] = 91; c.control[14] = 27; c.control_ready[13] = 999;
            c.r[1][1] = invert; /* false condition for both polarities */
            memory[0] = 0x4003e000 | (ii - 1) << 23 | invert << 28;
            memory[1] = 0x34000; /* empty body ending in SPKERNEL 0,0 */
            memory[2] = mvk(0, 10, 42);
            unsigned steps = 0;
            do {
                assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
                assert(++steps < 20);
            } while (c.loop_active);
            assert(c.cycles == 1 + ((4 + ii - 1) / ii) * ii);
            assert(c.control[13] == 91 && c.control[14] == 27 && c.r[0][10] == 0);
            assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
            assert(c.r[0][10] == 42);
        }
    /* A late predicate update is not visible at the imminent boundary. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][1] = 1;
    memory[0] = 0x4183e000; /* [B1] SPLOOPW 4 */
    memory[1] = 0; memory[2] = 0;
    memory[3] = mvk(1, 1, 0); memory[4] = 0x34000;
    for (unsigned j = 0; j < 5; ++j) assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.loop_active && c.cycles == 5);
    for (unsigned j = 0; j < 4; ++j) assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(!c.loop_active && c.cycles == 9);
    /* NOP cycles are empty loop-buffer cycles. Exercise the exact compact
     * NOP 8 in a synthetic [B1] SPLOOPW 4, then change B1 late:
     * the stage boundary still observes the condition from three cycles ago. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][1] = 1;
    memory[0] = 0x4183e000; /* [B1] SPLOOPW 4 */
    memory[1] = 0x0c6eec6e; /* compact NOP 8; compact NOP 1 */
    memory[2] = mvk(1, 1, 0);
    memory[3] = 0x34000; /* SPKERNEL 0,0 */
    memory[4] = mvk(0, 10, 42);
    memory[7] = 0xe0400000; /* slot 1 contains compact instructions */
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.loop_active && c.cycles == 1);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.loop_wait == 7 && c.loop.length == 1 && c.loop_tags == 0);
    for (unsigned j = 0; j < 7; ++j)
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.loop_wait == 0 && c.loop.length == 8 && c.loop.cycle == 8);
    unsigned compact_steps = 0;
    while (c.loop_active) {
        assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
        assert(++compact_steps < 12);
    }
    assert(c.cycles == 17 && c.r[0][10] == 0);
    assert(cdj_c674x_step(&c, read_word, write_memory, NULL));
    assert(c.r[0][10] == 42);
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
    /* Six consecutive taken branches fill the pipeline. Each redirects on
     * its own cycle, even after earlier branches have changed the fetch PC. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    for (unsigned j = 0; j < 6; ++j) {
        c.r[1][j + 1] = 0x1040 + j * 4;
        memory[j] = ((j + 1) << 18) | 0x362;
    }
    for (unsigned j = 0; j < 6; ++j) assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.branch_count == 4 && c.branch_due == 7);
    c.r[1][2] = 0x1080; /* in-flight target must already be captured */
    for (unsigned j = 1; j < 6; ++j) {
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.pc == 0x1040 + j * 4 && c.cycles == 6 + j);
    }
    assert(!c.branch_due && !c.branch_count);

    /* Two taken branches in one execute packet remain an explicit fault. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = (1u << 18) | 0x363; memory[1] = (2u << 18) | 0x362;
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.cycles && !c.branch_due && !c.branch_count);

    /* TI section 7.14: a branch started before SPLOOP cancels its buffer
     * when the fifth delay slot completes. It must not replay at the target. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][3] = 0x1040; c.control[13] = 10;
    memory[0] = (3u << 18) | 0x362;
    memory[1] = 2u << 13; /* NOP 3 */
    memory[2] = 0x38000;
    memory[3] = mvk(0, 4, 7);
    memory[16] = mvk(0, 5, 9);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.loop_active && c.cycles == 5);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!c.loop_active && c.pc == 0x1040 && c.cycles == 6 && c.r[0][4] == 7);
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[0][5] == 9);
    /* Compact MVK uses a split signed immediate and the selected subset. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = 0xfe27; memory[7] = 0xe0200000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][4] == 0xffffffff);
    cdj_c674x_reset(&c, 0x1000); memory[7] |= 1u << 19;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][20] == 0xffffffff && c.r[1][4] == 0);

    /* All L2c operations: high-subset operands, low predicate destination. */
    const uint32_t l2c_results[] = {0, 0x80000001, 0x80000001, 0, 1, 0, 0, 1};
    for (unsigned op = 0; op < 8; ++op) {
        memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
        c.r[0][17] = 0x80000000; c.r[1][18] = 1;
        memory[0] = (1u << 13) | (1u << 12) | ((op & 4) << 9) |
                    (2u << 7) | ((op & 3) << 5) | 0x418;
        memory[7] = 0xe0280000;
        assert(cdj_c674x_step(&c, read_word, NULL, NULL));
        assert(c.r[0][1] == l2c_results[op] && c.r[0][17] == 0x80000000);
    }
    /* Immediate comparison also writes a low predicate register with RS=1. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][18] = 7;
    memory[0] = (7u << 13) | (1u << 11) | (2u << 7) | 0x27;
    memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.r[1][1] == 1);

    /* Compact register BNOP ignores RS and captures its B0-B15 target. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][5] = 0x1040; c.r[1][21] = 0x1080;
    memory[0] = (5u << 13) | 0x2ef; memory[7] = 0xe0280000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.cycles == 6);
    /* CALLP writes the next execute-packet address and takes six cycles.
     * A parallel operation observes the old link register. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][3] = 0x2222;
    memory[0] = 0x10000813; /* CALLP .S2 1040,B3 || */
    memory[1] = (4u << 23) | (3u << 18) | 0x1058;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1040 && c.r[1][3] == 0x1008 && c.r[0][4] == 0x2222 && c.cycles == 6);

    /* Compact CALLP retains its word-scaled displacement while its return
     * address may be a halfword instruction boundary. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    memory[0] = (17u << 6) | 0x1a; memory[7] = 0xe0208000;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.pc == 0x1044 && c.r[0][3] == 0x1002 && c.cycles == 6);
    /* Captured firmware Scs10 at 0x118028e4 calls the aligned helper at
     * 0x118027ec. Halfword scaling would incorrectly target 0x11802866. */
    CdjC674xPacket captured_call = {.count = 1, .next_pc = 0x118028e6,
        .instructions = {{.compact = true, .pc = 0x118028e4,
            .header = 0xe8c08000, .word = 0xf0db}}};
    cdj_c674x_reset(&c, 0x118028e4);
    assert(cdj_c674x_execute(&c, &captured_call, read_word, NULL, NULL));
    assert(c.pc == 0x118027ec && c.r[1][3] == 0x118028e6 && c.cycles == 6);

    /* The 16-bit halfword pack family has parallel .L/.S encodings. Test
     * every operation on both sides with the cross path selected. */
    static const unsigned pack_l[] = {0x018, 0x3d8, 0x398, 0x378};
    static const unsigned pack_s[] = {0xff0, 0x260, 0x220, 0x420};
    static const uint32_t pack_result[] = {
        0x3344ccdd, 0x1122aabb, 0x1122ccdd, 0x3344aabb
    };
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned op = 0; op < 4; ++op)
            for (unsigned unit = 0; unit < 2; ++unit) {
                cdj_c674x_reset(&c, 0x1000);
                c.r[bank][1] = 0x11223344;
                c.r[bank ^ 1][2] = 0xaabbccdd;
                uint32_t encoding = unit ? pack_s[op] : pack_l[op];
                CdjC674xPacket p = {.count = 1, .next_pc = 0x1004,
                    .instructions = {{.pc = 0x1000, .word =
                        3u << 23 | 2u << 18 | 1u << 13 | 1u << 12 |
                        encoding | bank << 1}}};
                assert(cdj_c674x_execute(&c, &p, read_word, NULL, NULL));
                assert(c.r[bank][3] == pack_result[op]);
                assert(c.r[bank ^ 1][3] == 0 && c.cycles == 1);
            }
    static const unsigned pack4[] = {0xd18, 0xd38};
    static const uint32_t pack4_result[] = {0x2244bbdd, 0x1133aacc};
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned op = 0; op < 2; ++op) {
            cdj_c674x_reset(&c, 0x1000);
            c.r[bank][5] = 0x11223344; c.r[bank ^ 1][6] = 0xaabbccdd;
            CdjC674xPacket p = {.count = 1, .next_pc = 0x1004,
                .instructions = {{.pc = 0x1000, .word =
                    7u << 23 | 6u << 18 | 5u << 13 | 1u << 12 |
                    pack4[op] | bank << 1}}};
            assert(cdj_c674x_execute(&c, &p, read_word, NULL, NULL));
            assert(c.r[bank][7] == pack4_result[op]);
        }

    /* CALLP cannot be issued behind another pending branch. */
    memset(memory, 0, sizeof(memory)); cdj_c674x_reset(&c, 0x1000);
    c.r[1][1] = 0x1040;
    memory[0] = (1u << 18) | 0x362; memory[1] = 0x10000812;
    assert(cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(!cdj_c674x_step(&c, read_word, NULL, NULL));
    assert(c.cycles == 1 && c.r[1][3] == 0 && c.branch_target == 0x1040);
    puts("C674x sign extension, parallel reads, branch delay, NOP and atomic fault passed");
}
