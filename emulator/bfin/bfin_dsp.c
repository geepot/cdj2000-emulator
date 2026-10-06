/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * From hw/cdj/bfin/bfin_dsp.c of Stijn Jacobs' cdj-nxs2-qemu,
 * https://github.com/Stijn-Jacobs/cdj-nxs2-qemu, commit 08d5cb1.
 * Changed 2026-10-05 (bfin-link): A1 = A0, An = An (S), A0 += A1 / A0 -= A1 and their register
 * forms, BYTEOP2P, BYTEOP3P, BYTEPACK and BYTEUNPACK (dsp32alu 11, 22-24, as
 * GNU sim's decode_dsp32alu_0 does them; DEPOSIT's 16-bit field and (X) fill; A = -A and ABS A operands as GNU sim decodes them; the GUI's browse and image code
 * uses them), and a MAC pair from an odd register is not undefined: MAC1
 * writes R((dst + 1) & 7), as objdump decodes it (GNU sim writes past R7).
 */
/*
 * Blackfin DSP32 groups: the ALU (16-bit, vector and 32-bit arithmetic, the
 * accumulator moves), the shifter and the two multiply/accumulate units.
 *
 * Accumulators are 40 bits, kept sign-extended in an int64_t. Flags follow
 * the Programming Reference per instruction where the firmware can observe
 * them (AZ, AN, V, AC0, CC); the accumulator overflow flags are set on
 * saturation only.
 */
#include "bfin_dsp.h"
#include <stdlib.h>

enum {
    MM_DEFAULT = 0, MM_S2RND = 1, MM_T = 2, MM_W32 = 3, MM_FU = 4,
    MM_TFU = 6, MM_IS = 8, MM_ISS2 = 9, MM_IH = 11, MM_IU = 12,
};

#define ACC_MAX  ((int64_t)0x7FFFFFFFFF)
#define ACC_MIN  (-(int64_t)0x8000000000)

static inline uint16_t half(uint32_t v, int hi)
{
    return hi ? v >> 16 : v;
}

static inline void set_half(uint32_t *d, int hi, uint16_t v)
{
    *d = hi ? (*d & 0xFFFF) | (uint32_t)v << 16 : (*d & 0xFFFF0000) | v;
}

static int64_t clamp(int64_t v, int64_t lo, int64_t hi, int *ov)
{
    if (v < lo || v > hi) {
        *ov = 1;
        return v < lo ? lo : hi;
    }
    return v;
}

static inline int64_t sext40(int64_t v)
{
    return (int64_t)((uint64_t)v << 24) >> 24;
}

/* ---- divide primitives -------------------------------------------------- */

/* DIVS/DIVQ, one quotient bit per step: the dividend register holds the
 * partial remainder in its high half and collects quotient bits from the
 * bottom; AQ carries the sign relation between remainder and divisor. */
void bfin_divs(bfin_core *c, unsigned dst, unsigned src)
{
    uint32_t v = c->r[dst];
    int aq = ((v >> 31) ^ (c->r[src] >> 15)) & 1;

    c->astat = (c->astat & ~AS_AQ) | (aq ? AS_AQ : 0);
    c->r[dst] = v << 1 | aq;
}

void bfin_divq(bfin_core *c, unsigned dst, unsigned src)
{
    uint32_t v = c->r[dst];
    uint16_t div = c->r[src], rem = v >> 16;
    int aq;

    rem = c->astat & AS_AQ ? rem + div : rem - div;
    aq = ((rem ^ div) >> 15) & 1;
    c->astat = (c->astat & ~AS_AQ) | (aq ? AS_AQ : 0);
    c->r[dst] = ((uint32_t)rem << 16 | (v & 0xFFFF)) << 1 | !aq;
}

/* ---- ALU ---------------------------------------------------------------- */

static uint16_t add16(uint16_t a, uint16_t b, int sub, int sat, int *ov)
{
    int32_t r = sub ? (int16_t)a - (int16_t)b : (int16_t)a + (int16_t)b;

    if (r > 0x7FFF || r < -0x8000) {
        *ov = 1;
        if (sat) {
            r = r < 0 ? -0x8000 : 0x7FFF;
        }
    }
    return r;
}

static void flags16(bfin_core *c, uint16_t hi, uint16_t lo, int both, int ov)
{
    int z = !lo || (both && !hi), n = (lo >> 15) || (both && (hi >> 15));

    c->astat &= ~(AS_AZ | AS_AN);
    c->astat |= (z ? AS_AZ : 0) | (n ? AS_AN : 0);
    bfin_flags_v(c, ov);
}

/* Rd = Rs +|+ Rt and its siblings: op bit 1 is the high half's operation,
 * bit 0 the low half's; cross swaps the result halves. */
static uint32_t vadd(uint32_t a, uint32_t b, unsigned op, int sat, int cross,
                     int *ov)
{
    uint16_t hi = add16(a >> 16, b >> 16, op >> 1, sat, ov);
    uint16_t lo = add16(a, b, op & 1, sat, ov);

    return cross ? (uint32_t)lo << 16 | hi : (uint32_t)hi << 16 | lo;
}

static int32_t minmax(int32_t a, int32_t b, int max)
{
    return max ? (a > b ? a : b) : (a < b ? a : b);
}

static int64_t round16(bfin_core *c, int64_t v);

/* The byte-aligned word starting aln bytes into the pair l:h (BYTEOP*). */
static uint32_t algn(uint32_t l, uint32_t h, unsigned aln)
{
    return aln ? l >> (8 * aln) | h << (32 - 8 * aln) : l;
}

static void set_flag(bfin_core *c, uint32_t bit, int on)
{
    c->astat = on ? c->astat | bit : c->astat & ~bit;
}

/* A0 += A1 and A0 -= A1 (aopcde 11): aop 0 also writes Rd, aop 1 a rounded
 * half, aop 2 only A0, aop 3 subtracts; s is (W32). */
static int acc_add(bfin_core *c, unsigned aop, unsigned s, unsigned hl, unsigned dst)
{
    int64_t a0 = c->a[0], a1 = c->a[1], r;
    uint64_t m = 0xFFFFFFFFFFull;
    int carry = aop == 3 ? ((uint64_t)a1 & m) < ((uint64_t)a0 & m)
                         : (~(uint64_t)a1 & m) < ((uint64_t)a0 & m);
    int ov = 0;

    if ((aop == 0 && (s || hl)) || (aop == 1 && s) || (aop >= 2 && hl)) {
        return 0;
    }
    r = clamp(aop == 3 ? a0 - a1 : a0 + a1, ACC_MIN, ACC_MAX, &ov);
    if (aop >= 2 && s) {                        /* (W32) */
        r = r < 0 ? sext40(r & 0x80FFFFFFFFll) : r & 0xFFFFFFFFll;
        ov |= aop == 3 && r < 0;
    }
    c->a[0] = r;
    set_flag(c, AS_AV0, aop == 3 ? ov : ov && a1);
    if (ov) {
        c->astat |= AS_AV0S;
    }
    set_flag(c, AS_AC0 | AS_AC0_COPY, carry);
    if (aop >= 2) {
        set_flag(c, AS_AZ, r == 0);
        set_flag(c, AS_AN, r < 0);
        return 1;
    }
    int sat = 0;
    uint32_t d;

    if (aop) {
        uint16_t h = clamp(round16(c, r), -0x8000, 0x7FFF, &sat);

        set_half(&c->r[dst], hl, h);
        d = (uint32_t)h << 16;
    } else {
        d = c->r[dst] = clamp(r, INT32_MIN, INT32_MAX, &sat);
    }
    set_flag(c, AS_AZ, d == 0);
    set_flag(c, AS_AN, d >> 31);
    bfin_flags_v(c, sat);
    return 1;
}

static uint8_t clamp_u8(int32_t v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* BYTEOP2P, BYTEOP3P, BYTEPACK, BYTEUNPACK (aopcde 22-24). The source pairs
 * are R1:0 or R3:2, byte-aligned by I0 (and I1 for BYTEOP3P's second). */
static int byteop(bfin_core *c, unsigned aopcde, unsigned aop, unsigned s,
                  unsigned hl, unsigned dst0, unsigned dst1, unsigned src0,
                  unsigned src1)
{
    uint32_t *r = c->r;
    int pairs = (src0 == 0 || src0 == 2) && (src1 == 0 || src1 == 2);
    uint32_t s0 = 0, s1 = 0;

    if (pairs) {
        s0 = s ? algn(r[src0 + 1], r[src0], c->i[0] & 3) : algn(r[src0], r[src0 + 1], c->i[0] & 3);
        s1 = s ? algn(r[src1 + 1], r[src1], c->i[aopcde == 23] & 3)
               : algn(r[src1], r[src1 + 1], c->i[aopcde == 23] & 3);
    }
    if (aopcde == 22 && aop < 2 && pairs) {     /* BYTEOP2P */
        unsigned rnd = aop ? 0 : 2;
        uint32_t t0 = ((s1 >> 8 & 0xFF) + (s1 & 0xFF) + (s0 >> 8 & 0xFF) + (s0 & 0xFF) + rnd) >> 2 & 0xFF;
        uint32_t t1 = ((s1 >> 24) + (s1 >> 16 & 0xFF) + (s0 >> 24) + (s0 >> 16 & 0xFF) + rnd) >> 2 & 0xFF;

        r[dst0] = t1 << (16 + hl * 8) | t0 << (hl * 8);
        return 1;
    }
    if (aopcde == 23 && aop == 0 && pairs) {    /* BYTEOP3P */
        int32_t t0 = (int16_t)s0 + (int32_t)(s1 >> (8 * !hl) & 0xFF);
        int32_t t1 = (int16_t)(s0 >> 16) + (int32_t)(s1 >> (16 + 8 * !hl) & 0xFF);

        r[dst0] = (uint32_t)clamp_u8(t0) << (8 * hl) | (uint32_t)clamp_u8(t1) << (16 + 8 * hl);
        return 1;
    }
    if (aopcde == 24 && aop == 0 && !s && !hl) {    /* BYTEPACK */
        r[dst0] = (r[src0] & 0xFF) | (r[src0] >> 16 & 0xFF) << 8 |
                  (r[src1] & 0xFF) << 16 | (r[src1] >> 16 & 0xFF) << 24;
        return 1;
    }
    if (aopcde == 24 && aop == 1 && !hl && pairs && dst0 != dst1) {   /* BYTEUNPACK */
        uint64_t v = s ? (uint64_t)r[src0] << 32 | r[src0 + 1]
                       : (uint64_t)r[src0 + 1] << 32 | r[src0];
        unsigned o = 8 * (c->i[0] & 3);

        r[dst0] = (uint8_t)(v >> o) | (uint32_t)(uint8_t)(v >> (o + 8)) << 16;
        r[dst1] = (uint8_t)(v >> (o + 16)) | (uint32_t)(uint8_t)(v >> (o + 24)) << 16;
        return 1;
    }
    return 0;
}

void bfin_dsp32alu(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned aopcde = iw0 & 0x1F, hl = (iw0 >> 5) & 1;
    unsigned src1 = iw1 & 7, src0 = (iw1 >> 3) & 7;
    unsigned dst1 = (iw1 >> 6) & 7, dst0 = (iw1 >> 9) & 7;
    unsigned x = (iw1 >> 12) & 1, s = (iw1 >> 13) & 1, aop = iw1 >> 14;
    uint32_t a = c->r[src0], b = c->r[src1];
    int ov = 0;

    switch (aopcde) {
    case 0: {                                   /* Rd = Rs +|+ Rt ... */
        uint32_t r = vadd(a, b, aop, s, x, &ov);

        c->r[dst0] = r;
        flags16(c, r >> 16, r, 1, ov);
        return;
    }
    case 1: {                                   /* Rd = +|+, Re = -|- */
        if (aop) {
            break;
        }
        uint32_t r1 = vadd(a, b, hl ? 1 : 0, s, x, &ov);
        uint32_t r0 = vadd(a, b, hl ? 2 : 3, s, x, &ov);

        c->r[dst1] = r1;
        c->r[dst0] = r0;
        flags16(c, r0 >> 16, r0, 1, ov);
        return;
    }
    case 2:
    case 3: {                                   /* Rd.h = Rs.h +/- Rt.h */
        uint16_t r = add16(half(a, aop >> 1), half(b, aop & 1), aopcde == 3, s, &ov);

        set_half(&c->r[dst0], hl, r);
        flags16(c, 0, r, 0, ov);
        return;
    }
    case 4:
        if (aop == 2) {
            uint32_t sum = bfin_add32(c, a, b, 0, s);

            c->r[dst0] = bfin_add32(c, a, b, 1, s);
            c->r[dst1] = sum;
            return;
        }
        if (aop < 2) {
            c->r[dst0] = bfin_add32(c, a, b, aop, s);
            return;
        }
        break;
    case 6: {                                   /* MAX/MIN/ABS (V) */
        uint16_t hi, lo;

        if (aop == 2) {
            hi = (int16_t)(a >> 16) == -0x8000 ? 0x7FFF : abs((int16_t)(a >> 16));
            lo = (int16_t)a == -0x8000 ? 0x7FFF : abs((int16_t)a);
        } else if (aop < 2) {
            hi = minmax((int16_t)(a >> 16), (int16_t)(b >> 16), !aop);
            lo = minmax((int16_t)a, (int16_t)b, !aop);
        } else {
            break;
        }
        c->r[dst0] = (uint32_t)hi << 16 | lo;
        flags16(c, hi, lo, 1, 0);
        return;
    }
    case 7: {
        uint32_t r;

        switch (aop) {
        case 0: r = minmax(a, b, 1); break;
        case 1: r = minmax(a, b, 0); break;
        case 2:
            ov = a == 0x80000000u;
            r = ov ? 0x7FFFFFFFu : (int32_t)a < 0 ? -a : a;
            break;
        default:
            ov = a == 0x80000000u;
            r = ov && s ? 0x7FFFFFFFu : -a;
            break;
        }
        c->r[dst0] = r;
        bfin_flags_nz(c, r);
        bfin_flags_v(c, ov);
        return;
    }
    case 8:                                     /* A0 = 0 ... */
        if (hl || x) {
            break;
        }
        if (aop == 3) {                         /* A0 = A1, A1 = A0 */
            c->a[s] = c->a[!s];
            return;
        }
        if (s) {                                /* A0 = A0 (S) ... */
            for (int n = 0; n < 2; n++) {
                if (aop == 2 || aop == (unsigned)n) {
                    int sat = 0;

                    c->a[n] = clamp(c->a[n], INT32_MIN, INT32_MAX, &sat);
                    set_flag(c, n ? AS_AV1 : AS_AV0, sat);
                    if (sat) {
                        c->astat |= n ? AS_AV1S : AS_AV0S;
                    }
                }
            }
            set_flag(c, AS_AZ, (aop != 1 && !c->a[0]) || (aop != 0 && !c->a[1]));
            set_flag(c, AS_AN, (aop != 1 && c->a[0] < 0) || (aop != 0 && c->a[1] < 0));
            return;
        }
        switch (aop) {
        case 0: c->a[0] = 0; return;
        case 1: c->a[1] = 0; return;
        case 2: c->a[0] = c->a[1] = 0; return;
        case 3: c->a[0] = c->a[1]; return;
        }
        break;
    case 9:                                     /* A0 = Rs, A0.L = Rs.L ... */
        if (s) {
            c->a[aop >> 1] = (int32_t)a;
        } else if (aop & 1) {
            c->a[aop >> 1] = sext40(((int64_t)(int8_t)a << 32) |
                                    (uint32_t)c->a[aop >> 1]);
        } else {
            uint32_t w = c->a[aop >> 1];

            set_half(&w, hl, half(a, hl));
            c->a[aop >> 1] = (c->a[aop >> 1] & ~0xFFFFFFFFll) | w;
        }
        return;
    case 10:                                    /* Rd.L = A0.X */
        if (aop > 1) {
            break;
        }
        set_half(&c->r[dst0], 0, (int8_t)(c->a[aop] >> 32));
        return;
    case 14:                                    /* A1 = -A0 ... */
    case 16:                                    /* A1 = ABS A0 ... */
        /* A<HL> = op A<aop>; aop 3 both in place (GNU sim's operands). */
        if (s || x || aop == 2 || (aop == 3 && hl)) {
            break;
        }
        for (int n = 0; n < 2; n++) {
            if (aop == 3 || n == (int)hl) {
                int64_t v = c->a[aop == 3 ? n : aop];

                v = aopcde == 14 || v < 0 ? -v : v;
                c->a[n] = clamp(v, ACC_MIN, ACC_MAX, &ov);
            }
        }
        return;
    case 11:
        if (!x && acc_add(c, aop, s, hl, dst0)) {
            return;
        }
        break;
    case 22:
    case 23:
    case 24:
        if (!x && byteop(c, aopcde, aop, s, hl, dst0, dst1, src0, src1)) {
            return;
        }
        break;
    }
    c->undef = 1;
}

/* ---- shifter ------------------------------------------------------------ */

/* An arithmetic or logical shift by a signed count: positive is left. */
static uint32_t shift32(bfin_core *c, uint32_t v, int n, int logical, int sat)
{
    uint32_t r;
    int ov = 0;

    if (n >= 0) {
        r = n > 31 ? 0 : v << n;
        if (!logical && n && ((int32_t)r >> n != (int32_t)v || n > 31)) {
            ov = 1;
            if (sat) {
                r = (int32_t)v < 0 ? 0x80000000u : 0x7FFFFFFFu;
            }
        }
    } else if (logical) {
        r = -n > 31 ? 0 : v >> -n;
    } else {
        r = (int32_t)v >> (-n > 31 ? 31 : -n);
    }
    bfin_flags_nz(c, r);
    bfin_flags_v(c, ov);
    return r;
}

static uint16_t shift16(bfin_core *c, uint16_t v, int n, int logical, int sat)
{
    int32_t r;
    int ov = 0;

    if (n >= 0) {
        r = n > 15 ? 0 : (uint16_t)(v << n);
        if (!logical && n && ((int16_t)r >> n != (int16_t)v || n > 15)) {
            ov = 1;
            if (sat) {
                r = (int16_t)v < 0 ? 0x8000 : 0x7FFF;
            }
        }
    } else if (logical) {
        r = -n > 15 ? 0 : v >> -n;
    } else {
        r = (int16_t)v >> (-n > 15 ? 15 : -n);
    }
    flags16(c, 0, r, 0, ov);
    return r;
}

/* ROT: a 33-bit rotate through CC, positive counts to the left. Every form
 * gives the count as six signed bits, -32 to 31. */
static uint32_t rot32(bfin_core *c, uint32_t v, int n)
{
    uint64_t x = (uint64_t)c->cc << 32 | v;
    unsigned k = n < 0 ? n + 33 : n;

    if (k) {
        x = ((x << k) | (x >> (33 - k))) & 0x1FFFFFFFFull;
    }
    c->cc = x >> 32;
    return x;
}

static int64_t shift_acc(bfin_core *c, int64_t a, int n, int logical)
{
    uint64_t u = (uint64_t)a & 0xFFFFFFFFFFull;

    if (n >= 0) {
        return sext40(n > 39 ? 0 : (int64_t)(u << n));
    }
    if (logical) {
        return sext40(-n > 39 ? 0 : (int64_t)(u >> -n));
    }
    return a >> (-n > 39 ? 39 : -n);
}

static int count_signbits32(uint32_t v)
{
    int n = 0;

    while (n < 31 && ((v >> (30 - n)) & 1) == (v >> 31)) {
        n++;
    }
    return n;
}

static int count_signbits16(uint16_t v)
{
    int n = 0;

    while (n < 15 && ((v >> (14 - n)) & 1) == (v >> 15)) {
        n++;
    }
    return n;
}

static uint32_t extract(bfin_core *c, uint32_t v, uint16_t ctl, int x)
{
    unsigned pos = (ctl >> 8) & 0x1F, len = ctl & 0x1F;
    uint32_t r = pos ? v >> pos : v;

    if (len < 32) {
        r &= (1u << len) - 1;
        if (x && len && (r >> (len - 1)) & 1) {
            r |= ~0u << len;
        }
    }
    bfin_flags_nz(c, r);
    return r;
}

static uint32_t deposit(bfin_core *c, uint32_t bg, uint32_t fg, int x)
{
    unsigned pos = (fg >> 8) & 0x1F, len = fg & 0x1F;
    /* The field is 16 bits: a longer length deposits 16 (GNU sim). (X)
     * fills everything above the field with its sign. */
    uint32_t mask = (1u << (len < 16 ? len : 16)) - 1;
    uint32_t field = (fg >> 16) & mask;
    uint32_t r;

    if (x) {
        if (len && len <= 16 && ((field >> (len - 1)) & 1)) {
            field |= ~0u << len;
        }
        mask = ~0u;
    }
    r = (bg & ~(mask << pos)) | (field << pos);
    bfin_flags_nz(c, r);
    return r;
}

void bfin_dsp32shift(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned sopcde = iw0 & 0x1F;
    unsigned src1 = iw1 & 7, src0 = (iw1 >> 3) & 7, dst0 = (iw1 >> 9) & 7;
    unsigned hls = (iw1 >> 12) & 3, sop = iw1 >> 14;
    uint32_t v = c->r[src1], ctl = c->r[src0];

    switch (sopcde) {
    case 0:
        if (sop == 3) {
            break;
        }
        set_half(&c->r[dst0], hls >> 1,
                 shift16(c, half(v, hls & 1), (int8_t)(ctl << 3) >> 3, sop == 2, sop == 1));
        return;
    case 2:
        if (sop == 3) {
            c->r[dst0] = rot32(c, v, (int8_t)(ctl << 2) >> 2);
        } else {
            c->r[dst0] = shift32(c, v, (int8_t)(ctl << 2) >> 2, sop == 2, sop == 1);
        }
        return;
    case 3:
        if (sop < 2) {
            c->a[hls & 1] = shift_acc(c, c->a[hls & 1], (int8_t)(ctl << 2) >> 2, sop);
            return;
        }
        break;
    case 4: {                                   /* PACK */
        c->r[dst0] = (uint32_t)half(v, sop >> 1) << 16 | half(ctl, sop & 1);
        return;
    }
    case 5: {                                   /* SIGNBITS */
        int n = sop == 0 ? count_signbits32(v) : sop == 1 ? count_signbits16(v)
              : sop == 2 ? count_signbits16(v >> 16) : -1;

        if (n < 0) {
            break;
        }
        set_half(&c->r[dst0], 0, n);
        return;
    }
    case 6:
        if (sop == 3) {                         /* ONES */
            set_half(&c->r[dst0], 0, __builtin_popcount(v));
            return;
        }
        break;
    case 10:                                    /* EXTRACT, DEPOSIT */
        if (sop < 2) {
            c->r[dst0] = extract(c, v, ctl, sop);
        } else {
            c->r[dst0] = deposit(c, v, ctl, sop == 3);
        }
        return;
    }
    c->undef = 1;
}

void bfin_dsp32shiftimm(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned sopcde = iw0 & 0x1F;
    unsigned src1 = iw1 & 7, dst0 = (iw1 >> 9) & 7;
    unsigned hls = (iw1 >> 12) & 3, sop = iw1 >> 14;
    int n = (int8_t)(((iw1 >> 3) & 0x3F) << 2) >> 2;
    uint32_t v = c->r[src1];

    switch (sopcde) {
    case 0:
        if (sop == 3) {
            break;
        }
        set_half(&c->r[dst0], hls >> 1,
                 shift16(c, half(v, hls & 1), n, sop == 2, sop == 1));
        return;
    case 1: {                                   /* (V) */
        if (sop == 3) {
            break;
        }
        uint16_t hi = shift16(c, v >> 16, n, sop == 2, sop == 1);
        uint16_t lo = shift16(c, v, n, sop == 2, sop == 1);

        c->r[dst0] = (uint32_t)hi << 16 | lo;
        return;
    }
    case 2:
        if (sop == 3) {
            c->r[dst0] = rot32(c, v, n);
        } else {
            c->r[dst0] = shift32(c, v, n, sop == 2, sop == 1);
        }
        return;
    case 3:
        if (sop < 2) {
            c->a[hls & 1] = shift_acc(c, c->a[hls & 1], n, sop);
            return;
        }
        break;
    }
    c->undef = 1;
}

/* The 32-bit form (sopcde 2), which bundles use most. */
void bfin_dsp32shiftimm32(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned src1 = iw1 & 7, dst0 = (iw1 >> 9) & 7, sop = iw1 >> 14;
    int n = (int8_t)(((iw1 >> 3) & 0x3F) << 2) >> 2;
    uint32_t v = c->r[src1];

    if (sop == 3) {
        c->r[dst0] = rot32(c, v, n);
    } else {
        c->r[dst0] = shift32(c, v, n, sop == 2, sop == 1);
    }
}

/* ---- multiply/accumulate ------------------------------------------------ */

/* Ported from GNU sim's decode_multfunc, decode_macfunc and extract_mult
 * (bfin-sim.c) by bfin-link: the upstream versions disagreed with it on the
 * unsigned and integer modes (FU, TFU, IU, IH) and mixed (M) products, which
 * the GUI's image decoding uses. Accumulators stay 40-bit sign-extended
 * int64; an unsigned mode reads them zero-extended, as GNU sim does. */

static int mm_signed(unsigned mmod)
{
    return mmod == MM_DEFAULT || mmod == MM_IS || mmod == MM_T || mmod == MM_S2RND ||
           mmod == MM_ISS2 || mmod == MM_IH || mmod == MM_W32;
}

/* Round a value at bit 16: unbiased (to even) unless ASTAT.RND_MOD. */
static int64_t round16(bfin_core *c, int64_t v)
{
    if (c->astat & AS_RND_MOD) {
        return (v + 0x8000) >> 16;
    }
    if ((v & 0x1FFFF) == 0x8000) {
        return v >> 16;
    }
    return (v + 0x8000) >> 16;
}

static uint32_t sat_s16(int64_t v, int *ov)
{
    return (uint16_t)clamp(v, -0x8000, 0x7FFF, ov);
}

static uint32_t sat_u16(uint64_t v, int *ov)
{
    return v > 0xFFFF ? (*ov = 1, 0xFFFF) : (uint32_t)v;
}

static uint32_t sat_s32(int64_t v, int *ov)
{
    return (uint32_t)clamp(v, INT32_MIN, INT32_MAX, ov);
}

static uint32_t sat_u32(uint64_t v, int *ov)
{
    return v > 0xFFFFFFFFull ? (*ov = 1, 0xFFFFFFFFu) : (uint32_t)v;
}

/* One 16 x 16 product, sign- or zero-extended to 64 bits; *sat on the
 * fractional -1 * -1. */
static uint64_t multfunc(uint16_t a, uint16_t b, unsigned mmod, int mm, int *sat)
{
    uint32_t s0 = a, s1 = b, val;
    uint64_t v;

    if (mm) {
        s0 = (uint32_t)(int16_t)a;
    } else if (mm_signed(mmod)) {
        s0 = (uint32_t)(int16_t)a;
        s1 = (uint32_t)(int16_t)b;
    }
    val = s0 * s1;
    *sat = 0;
    if (!mm && (mmod == MM_DEFAULT || mmod == MM_T || mmod == MM_S2RND || mmod == MM_W32)) {
        if (val == 0x40000000) {
            val = mmod == MM_W32 ? 0x7FFFFFFF : 0x80000000;
            *sat = 1;
        } else {
            val <<= 1;
        }
    }
    v = val;
    if (mm_signed(mmod) || mm) {
        v = (uint64_t)(int64_t)(int32_t)val;
    }
    if (*sat) {
        v &= 0xFFFFFFFFull;
    }
    return v;
}

/* 16 or 32 bits of a product or an accumulator, by mode. */
static uint32_t extract_mult(bfin_core *c, uint64_t res, unsigned mmod, int mm, int full,
                        int *ov)
{
    int64_t r = (int64_t)res;

    if (full) {
        switch (mmod) {
        case MM_IU:
        case MM_FU:
            return mm ? sat_s32(r, ov) : sat_u32(res, ov);
        case MM_S2RND:
        case MM_ISS2:
            return sat_s32((int64_t)(res << 1), ov);
        default:
            return sat_s32(r, ov);
        }
    }
    switch (mmod) {
    case MM_IS:
        return sat_s16(r, ov);
    case MM_FU:
        return mm ? sat_s16(round16(c, r), ov) : sat_u16((uint64_t)round16(c, r), ov);
    case MM_IU:
        return mm ? sat_s16(r, ov) : sat_u16(res, ov);
    case MM_T:
        return sat_s16(r >> 16, ov);
    case MM_TFU:
        return mm ? sat_s16(r >> 16, ov) : sat_u16((uint64_t)(r >> 16), ov);
    case MM_S2RND:
        return sat_s16(round16(c, (int64_t)(res << 1)), ov);
    case MM_ISS2:
        return sat_s16((int64_t)(res << 1), ov);
    default:                                    /* default, W32, IH */
        return sat_s16(round16(c, r), ov);
    }
}

/* One MAC unit: accumulate (op 0 =, 1 +=, 2 -=, 3 none) and extract. */
static uint32_t macfunc(bfin_core *c, int n, unsigned op, uint16_t a, uint16_t b,
                        unsigned mmod, int mm, int full, int *ov, int *neg)
{
    uint64_t acc = mm_signed(mmod) || mm ? (uint64_t)c->a[n]
                                         : (uint64_t)c->a[n] & 0xFFFFFFFFFFull;
    int sat = 0;

    if (op != 3) {
        int sgn40 = (acc >> 39) & 1, tsat;
        uint64_t res = multfunc(a, b, mmod, mm, &tsat), nosat;
        int64_t s;

        acc = op == 0 ? res : op == 1 ? acc + res : acc - res;
        nosat = acc;
        s = (int64_t)acc;
        switch (mmod) {
        case MM_TFU:
        case MM_FU:
            if (mm) {
                if (s < ACC_MIN) {
                    acc = (uint64_t)ACC_MIN, sat = 1;
                } else if (s > ACC_MAX) {
                    acc = ACC_MAX, sat = 1;
                } else if (mmod == MM_FU && (acc & 0x8000000000ull)) {
                    acc |= 0xFFFFFF0000000000ull;
                }
            } else if (s < 0) {
                acc = 0, sat = 1;
            } else if (s > 0xFFFFFFFFFFll) {
                acc = 0xFFFFFFFFFFull, sat = 1;
            }
            break;
        case MM_IU:
            if (!mm && (acc >> 63)) {
                acc = 0, sat = 1;
            }
            if (!mm && acc > 0xFFFFFFFFFFull) {
                acc = 0xFFFFFFFFFFull, sat = 1;
            }
            if (mm && acc > 0xFFFFFFFFFFull) {
                acc &= 0xFFFFFFFFFFull;
            }
            if (acc & 0x8000000000ull) {
                acc |= 0xFFFFFF0000000000ull;
            }
            break;
        case MM_IH:
            if (s < INT32_MIN) {
                acc = (uint64_t)(int64_t)INT32_MIN, sat = 1;
            } else if (s > INT32_MAX) {
                acc = INT32_MAX, sat = 1;
            }
            break;
        case MM_W32:
            if (sgn40 && (acc >> 31) != 0x1FFFFFFFFull && (acc >> 31) != 0) {
                acc = 0x80000000, sat = 1;
            }
            if (!sat && !sgn40 && (acc >> 31) != 0 && (acc >> 31) != 0x1FFFFFFFFull) {
                acc = 0x7FFFFFFF, sat = 1;
            }
            acc = (uint64_t)(int64_t)(int32_t)(uint32_t)acc;
            sat |= tsat;
            break;
        default:                                /* default, T, IS, ISS2, S2RND */
            if (s < ACC_MIN) {
                acc = (uint64_t)ACC_MIN, sat = 1;
            } else if (s > ACC_MAX) {
                acc = ACC_MAX, sat = 1;
            }
            break;
        }
        *neg |= (acc >> 39) & 1;
        c->a[n] = sext40((int64_t)(acc & 0xFFFFFFFFFFull));
        c->astat &= ~(n ? AS_AV1 : AS_AV0);
        if (sat) {
            c->astat |= n ? AS_AV1 | AS_AV1S : AS_AV0 | AS_AV0S;
            if (full) {
                *ov = 1;
            } else {
                extract_mult(c, nosat, mmod, mm, full, ov);
            }
        }
    }
    uint32_t ret = extract_mult(c, acc, mmod, mm, full, ov);

    *neg |= full ? ret >> 31 : (ret >> 15) & 1;
    return ret;
}

/* dsp32mac (mult = 0) and dsp32mult (mult = 1) share one layout: MAC1 feeds
 * A1 and the high half (or the odd register of a pair), MAC0 A0 and the low
 * half (or the even register). MM (mixed) applies to MAC1 only. */
void bfin_dsp32mac(bfin_core *c, uint16_t iw0, uint16_t iw1, int mult)
{
    unsigned op1 = iw0 & 3, w1 = (iw0 >> 2) & 1, pair = (iw0 >> 3) & 1;
    unsigned mm = (iw0 >> 4) & 1, mmod = (iw0 >> 5) & 0xF;
    unsigned src1 = iw1 & 7, src0 = (iw1 >> 3) & 7, dst = (iw1 >> 6) & 7;
    unsigned h10 = (iw1 >> 9) & 1, h00 = (iw1 >> 10) & 1;
    unsigned op0 = (iw1 >> 11) & 3, w0 = (iw1 >> 13) & 1;
    unsigned h11 = (iw1 >> 14) & 1, h01 = (iw1 >> 15) & 1;
    uint32_t a = c->r[src0], b = c->r[src1], res = c->r[dst];
    unsigned dst1 = pair ? (dst + 1) & 7 : dst;
    int ov[2] = { 0, 0 }, neg[2] = { 0, 0 }, zero = 0;

    if (mult) {
        if (!w0 && !w1) {
            c->undef = 1;
            return;
        }
        for (int n = 1; n >= 0; n--) {
            if (!(n ? w1 : w0)) {
                continue;
            }
            int sat;
            uint64_t r = multfunc(half(a, n ? h01 : h00), half(b, n ? h11 : h10), mmod,
                                  n && mm, &sat);
            uint32_t v = extract_mult(c, r, mmod, n && mm, pair, &ov[n]);

            ov[n] |= sat;
            if (pair) {
                c->r[n ? dst1 : dst] = v;
            } else {
                set_half(&res, n, v);
            }
        }
        if (!pair) {
            c->r[dst] = res;
        }
        bfin_flags_v(c, ov[0] | ov[1]);
        return;
    }
    if (!w0 && !w1 && op0 == 3 && op1 == 3) {
        c->undef = 1;
        return;
    }
    for (int n = 1; n >= 0; n--) {
        unsigned op = n ? op1 : op0, w = n ? w1 : w0;

        if (!w && op == 3) {
            continue;
        }
        uint32_t v = macfunc(c, n, op, half(a, n ? h01 : h00), half(b, n ? h11 : h10),
                             mmod, n && mm, pair, &ov[n], &neg[n]);

        if (op == 3) {
            zero |= v == 0;
        }
        if (!w) {
            ov[n] = 0;
            continue;
        }
        if (pair) {
            c->r[n ? dst1 : dst] = v;
        } else {
            set_half(&res, n, v);
        }
    }
    if (!pair && (w0 || w1)) {
        c->r[dst] = res;
    }
    if (pair || w0 || w1) {
        c->astat &= ~AS_V;
        if (ov[0] | ov[1]) {
            c->astat |= AS_V | AS_VS;
        }
    }
    if ((w0 && op0 == 3) || (w1 && op1 == 3)) {
        c->astat = zero ? c->astat | AS_AZ : c->astat & ~AS_AZ;
        int an = (w0 && op0 == 3 && neg[0]) || (w1 && op1 == 3 && neg[1]);

        c->astat = an ? c->astat | AS_AN : c->astat & ~AS_AN;
    }
}
