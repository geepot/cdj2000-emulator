/* SPDX-License-Identifier: GPL-2.0-or-later */
/* TI SPRUFE8B: SADD pp422-424, SSHL pp493-494, SSUB pp499-500;
 * CSR Table 2-9, SSR 2.9.13, compact Figures D-4/F-22/F-25/F-26.
 * Covers scalar32 and signed40 SADD/SSUB, not packed saturation/multiply. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "cdj_c674x.h"

static void issue(CdjC674x *c, uint32_t word, bool compact, uint32_t header)
{
    CdjC674xPacket p = {
        .instructions = {{.word = word, .pc = c->pc,
                          .compact = compact, .header = header}},
        .count = 1, .next_pc = c->pc + (compact ? 2 : 4),
    };
    if (!cdj_c674x_execute(c, &p, NULL, NULL, NULL)) {
        fprintf(stderr, "saturation test fault word=%08x: %s\n", word, c->fault);
        assert(false);
    }
}

static uint32_t clamp(int64_t n, bool *saturated)
{
    *saturated = n < INT32_MIN || n > INT32_MAX;
    return (uint32_t)(n < INT32_MIN ? INT32_MIN : n > INT32_MAX ? INT32_MAX : n);
}

static void delayed_flags(CdjC674x *c, bool saturated, unsigned unit_bit)
{
    assert(!(c->control[1] & 0x200));
    assert(!c->control[21]);
    issue(c, 0, false, 0);
    assert((c->control[1] & 0x200) == (saturated ? 0x200u : 0));
    assert(c->control[21] == (saturated ? unit_bit : 0));
    issue(c, 0, false, 0);
    assert(c->control[21] == (saturated ? unit_bit : 0));
}

static const uint32_t values[] = {
    0, 1, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x40000000u,
    0xc0000000u, 15, 0xfffffff0u,
};

static void full_arithmetic(void)
{
    /* op, immediate src1, subtract, src1 cross (SSUB reverse route), S unit */
    static const struct { unsigned opcode; bool imm, sub, reverse, sunit; } forms[] = {
        {0x278, false, false, false, false}, /* SADD .L reg */
        {0x258, true, false, false, false},  /* SADD .L scst5 */
        {0x820, false, false, false, true},  /* SADD .S reg */
        {0x1f8, false, true, false, false},  /* SSUB .L local-cross */
        {0x3f8, false, true, true, false},   /* SSUB .L cross-local */
        {0x1d8, true, true, false, false},   /* SSUB .L scst5 */
    };
    for (unsigned f = 0; f < sizeof(forms) / sizeof(forms[0]); ++f)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned enabled = 0; enabled < 2; ++enabled)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        unsigned field = forms[f].imm ? (values[i] & 31) : 1;
        int64_t a = forms[f].imm ? (int)(field ^ 16) - 16 : (int32_t)values[i];
        int64_t b = (int32_t)values[j];
        c.r[side ^ (forms[f].reverse ? cross : 0)][1] = values[i];
        c.r[side ^ (forms[f].reverse ? 0 : cross)][2] = values[j];
        c.r[side][3] = 0x11223344;
        c.r[1][0] = enabled;
        uint32_t word = (1u << 29) | 3u << 23 | 2u << 18 | field << 13 |
                        cross << 12 | forms[f].opcode | side << 1;
        bool saturated;
        uint32_t expected = clamp(forms[f].sub ? a - b : a + b, &saturated);
        issue(&c, word, false, 0);
        assert(c.r[side][3] == (enabled ? expected : 0x11223344u));
        delayed_flags(&c, enabled && saturated,
                      1u << ((forms[f].sunit ? 2 : 0) + side));
    }
}

static uint32_t shift_expected(uint32_t source, unsigned count, bool *sat)
{
    if (count >= 32) {
        *sat = source != 0;
        return !source ? 0 : source >> 31 ? 0x80000000u : 0x7fffffffu;
    }
    return clamp((int64_t)(int32_t)source * (INT64_C(1) << count), sat);
}

static void full_shifts(void)
{
    static const unsigned counts[] = {0, 1, 15, 30, 31, 32, 33, 63, 64, 95, 96, UINT_MAX};
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned imm = 0; imm < 2; ++imm)
    for (unsigned enabled = 0; enabled < 2; ++enabled)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned j = 0; j < sizeof(counts) / sizeof(counts[0]); ++j) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1] = counts[j]; c.r[side ^ cross][2] = values[i];
        c.r[side][3] = 0x11223344; c.r[1][0] = enabled;
        unsigned count = counts[j] & (imm ? 31 : 63);
        uint32_t word = 1u << 29 | 3u << 23 | 2u << 18 |
                        (imm ? count : 1u) << 13 | cross << 12 |
                        (imm ? 0x8a0 : 0x8e0) | side << 1;
        bool saturated;
        uint32_t expected = shift_expected(values[i], count, &saturated);
        issue(&c, word, false, 0);
        assert(c.r[side][3] == (enabled ? expected : 0x11223344u));
        delayed_flags(&c, enabled && saturated, 1u << (2 + side));
    }
}

static void long_arithmetic(void)
{
    const int64_t limit = INT64_C(1) << 39;
    const int64_t longs[] = {0, 1, -1, limit - 1, limit - 16, -limit, -limit + 16,
                            INT64_C(0x7fffffff), -INT64_C(0x80000000)};
    const unsigned ops[] = {0x618, 0x638, 0x598};
    for (unsigned f = 0; f < 3; ++f)
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross <= (f == 1 ? 1u : 0u); ++cross)
    for (unsigned enabled = 0; enabled < 2; ++enabled)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned j = 0; j < sizeof(longs) / sizeof(longs[0]); ++j) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        unsigned field = f == 1 ? 1u : values[i] & 31;
        int64_t left = f == 1 ? (int32_t)values[i] : (int)(field ^ 16) - 16;
        c.r[side ^ cross][1] = values[i];
        c.r[side][2] = (uint32_t)longs[j];
        c.r[side][3] = 0xa5a5a500u | (((uint64_t)longs[j] >> 32) & 255);
        c.r[side][4] = 0x11223344; c.r[side][5] = 0x55667788;
        c.r[1][0] = enabled;
        int64_t result = f == 2 ? left - longs[j] : left + longs[j];
        bool sat = result >= limit || result < -limit;
        if (result >= limit) result = limit - 1;
        if (result < -limit) result = -limit;
        uint32_t word = 1u << 29 | 4u << 23 | 2u << 18 | field << 13 |
                        cross << 12 | ops[f] | side << 1;
        issue(&c, word, false, 0);
        assert(c.r[side][4] == (enabled ? (uint32_t)result : 0x11223344u));
        assert(c.r[side][5] == (enabled ? ((uint64_t)result >> 32) & 255 : 0x55667788u));
        delayed_flags(&c, enabled && sat, 1u << side);
    }
    for (unsigned f = 0; f < 3; ++f)
    for (unsigned bad = 0; bad < 3; ++bad) {
        if (f == 1 && bad == 2) continue; /* cross sint src1 is valid */
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1004,
            .instructions = {{.word = (bad == 0 ? 5u : 4u) << 23 |
                (bad == 1 ? 3u : 2u) << 18 | 1u << 13 |
                (bad == 2 ? 1u << 12 : 0) | ops[f], .pc = 0x1000}}};
        assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
        assert(c.fault && !c.cycles && !c.packets && !c.load_count);
        assert(!c.control[21] && !(c.control[1] & 0x200));
    }
}

static void compact_arithmetic(void)
{
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned cross = 0; cross < 2; ++cross)
    for (unsigned rs = 0; rs <= 16; rs += 16)
    for (unsigned sunit = 0; sunit < 2; ++sunit)
    for (unsigned sub = 0; sub < 2; ++sub)
    for (unsigned sat = 0; sat < 2; ++sat)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); ++j) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1 + rs] = values[i]; c.r[side ^ cross][2 + rs] = values[j];
        uint32_t word = 1u << 13 | cross << 12 | sub << 11 | 2u << 7 |
                        3u << 4 | (sunit ? 0xa : 0) | side;
        uint32_t header = 0xe0000000u | (rs ? 1u << 19 : 0) | sat << 14;
        bool saturated = false;
        uint32_t expected = sub ? values[i] - values[j] : values[i] + values[j];
        if (sat && !(sunit && sub))
            expected = clamp(sub ? (int64_t)(int32_t)values[i] - (int32_t)values[j] :
                                  (int64_t)(int32_t)values[i] + (int32_t)values[j], &saturated);
        issue(&c, word, true, header);
        assert(c.r[side][3 + rs] == expected);
        delayed_flags(&c, saturated, 1u << (2 * sunit + side));
    }
}

static void compact_shifts(void)
{
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned rs = 0; rs <= 16; rs += 16)
    for (unsigned imm = 0; imm < 2; ++imm)
    for (unsigned sat = 0; sat < 2; ++sat)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned count = 0; count < (imm ? 32u : 64u); ++count) {
        CdjC674x c; cdj_c674x_reset(&c, 0x1000);
        c.r[side][1 + rs] = count; c.r[side][2 + rs] = values[i];
        uint32_t word = imm ? (count & 7) << 13 | (count >> 3) << 11 | 0x442 :
                              1u << 13 | 3u << 11 | 0x462;
        word |= 2u << 7 | side;
        bool saturated = false;
        uint32_t expected = count < 32 ? values[i] >> count : 0;
        if (!imm || sat) expected = shift_expected(values[i], count, &saturated);
        issue(&c, word, true, 0xe0000000u | (rs ? 1u << 19 : 0) | sat << 14);
        assert(c.r[side][2 + rs] == expected);
        delayed_flags(&c, saturated, 1u << (2 + side));
    }
}

static uint32_t mvc_write(unsigned control, unsigned source)
{ return control << 23 | source << 18 | 0x3a2; }
static uint32_t mvc_read(unsigned control, unsigned destination)
{ return destination << 23 | control << 18 | 0x3e2; }

static void status_interactions(void)
{
    CdjC674x c; cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = c.r[1][1] = 0x7fffffff;
    c.r[0][2] = c.r[1][2] = 1;
    CdjC674xPacket p = {.count = 2, .next_pc = 0x1008,
        .instructions = {{.word = 3u << 23 | 2u << 18 | 1u << 13 | 0x278, .pc = 0x1000},
                         {.word = 3u << 23 | 2u << 18 | 1u << 13 | 0x822, .pc = 0x1004}}};
    assert(cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
    assert(!c.control[21] && !(c.control[1] & 0x200));
    issue(&c, mvc_read(21, 5), false, 0);
    assert(c.r[1][5] == 0 && c.control[21] == 9 && (c.control[1] & 0x200));
    issue(&c, mvc_read(21, 5), false, 0); assert(c.r[1][5] == 9);
    issue(&c, mvc_read(1, 5), false, 0); assert(c.r[1][5] & 0x200);
    c.r[0][1] = c.r[0][2] = 0;
    issue(&c, p.instructions[0].word, false, 0);
    issue(&c, 0, false, 0);
    assert(c.control[21] == 9 && (c.control[1] & 0x200));
    c.r[1][6] = 0; issue(&c, mvc_write(21, 6), false, 0);
    assert(!c.control[21] && (c.control[1] & 0x200));
    c.r[1][6] = UINT_MAX; issue(&c, mvc_write(21, 6), false, 0);
    assert(c.control[21] == 0x3f);
    c.r[1][6] = 0; issue(&c, mvc_write(1, 6), false, 0);
    assert(!(c.control[1] & 0x200) && c.control[21] == 0x3f);
    c.r[1][6] = 0x200; issue(&c, mvc_write(1, 6), false, 0);
    assert(!(c.control[1] & 0x200)); /* MVC cannot set SAT. */
    for (unsigned control = 1; control <= 21; control += 20) {
        cdj_c674x_reset(&c, 0x1000);
        c.r[0][1] = 0x7fffffff; c.r[0][2] = 1;
        issue(&c, p.instructions[0].word, false, 0);
        c.r[1][6] = 0; issue(&c, mvc_write(control, 6), false, 0);
        assert(c.control[21] == 1 && (c.control[1] & 0x200));
    }
    /* A rejected packet must not leak its saturating register/flag effects. */
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x7fffffff; c.r[0][2] = 1;
    p.instructions[1] = p.instructions[0];
    assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
    assert(c.fault && !c.load_count && !c.cycles && !c.r[0][3]);
    assert(!c.control[21] && !(c.control[1] & 0x200));
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][1] = 0x7fffffff; c.r[0][2] = 1;
    c.load_count = 40;
    for (unsigned i = 0; i < 40; ++i)
        c.loads[i] = (CdjC674xLoad){.due = 100, .size = CDJ_C674X_DELAYED_SAT, .address = 1};
    p.count = 1;
    assert(!cdj_c674x_execute(&c, &p, NULL, NULL, NULL));
    assert(c.fault && c.load_count == 40 && !c.cycles && !c.r[0][3]);
    assert(!c.control[21] && !(c.control[1] & 0x200));
}

int main(void)
{
    full_arithmetic(); full_shifts(); long_arithmetic();
    compact_arithmetic(); compact_shifts();
    status_interactions();
    puts("C674x scalar saturation and delayed CSR/SSR tests passed");
    return 0;
}
