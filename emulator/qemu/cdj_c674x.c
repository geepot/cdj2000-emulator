/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cdj_c674x.h"

#define CDJ_C674X_TSR_SPLX (1u << 14)
#define CDJ_C674X_LOOP_RETURNING (1u << 3)

/* SPRUFE8B 7.7.3.2 makes TSR.SPLX hardware-owned loop-buffer state.  Keep
 * it synchronized here instead of adding a second checkpointed state bit. */
static void loop_set_active(CdjC674x *cpu, bool active)
{
    cpu->loop_active = active;
    if (active) cpu->control[26] |= CDJ_C674X_TSR_SPLX;
    else cpu->control[26] &= ~CDJ_C674X_TSR_SPLX;
}

static int32_t sx(uint32_t value, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)((value ^ sign) - sign);
}

static bool queued_memory_load(const CdjC674xLoad *load)
{
    return load->size == 1 || load->size == 2 ||
           load->size == 4 || load->size == 8;
}

static unsigned queued_result_registers(const CdjC674xLoad *load)
{
    if (load->size == CDJ_C674X_DELAYED_IFR_SET ||
        load->size == CDJ_C674X_DELAYED_IFR_CLEAR) return 0;
    return load->size == 8 || load->size == 16 ? 2 : 1;
}

static uint32_t control_read(const CdjC674x *cpu, unsigned id)
{
    switch (id) {
    case 1:                         /* CSR */
        return cpu->control[id] & 0xffff03ffu;
    case 2:                         /* IFR */
        return cpu->control[id] & 0xfff2u;
    case 4:                         /* IER */
        return (cpu->control[id] & 0xfff2u) | 1u;
    case 5: {                       /* ISTP */
        uint32_t pending = cpu->control[2] & cpu->control[4] & 0xfff2u;
        unsigned highest = 0;
        while (pending && !(pending & 1)) {
            ++highest;
            pending >>= 1;
        }
        return (cpu->control[id] & 0xfffffc00u) | highest << 5;
    }
    case 6: case 7:                 /* IRP, NRP */
    case 13: case 14:               /* ILC, RILC */
        return cpu->control[id];
    case 27:                        /* ITSR */
        return (cpu->control[id] & 0x0000c6deu) |
               ((cpu->control[1] >> 1) & 1u);
    case 18: case 19: case 20:      /* FADCR, FAUCR, FMCR */
        return cpu->control[id];
    case 26:                        /* TSR */
        return (cpu->control[id] & 0x0000c6deu) |
               (cpu->control[1] & 1u);
    default:
        return 0;
    }
}

static bool control_read_supported(unsigned id)
{
    return id == 1 || id == 2 || id == 4 || id == 5 || id == 6 || id == 7 ||
           id == 13 || id == 14 || id == 26 || id == 27 ||
           (id >= 18 && id <= 20);
}

static bool control_write_supported(unsigned id)
{
    return (id >= 1 && id <= 7) || id == 13 || id == 14 ||
           id == 26 || id == 27 || (id >= 18 && id <= 20);
}

static uint64_t arithmetic_shift_right64(uint64_t value, unsigned count)
{
    uint64_t shifted = value >> count;
    if (value >> 63) shifted |= UINT64_MAX << (64 - count);
    return shifted;
}

static uint64_t register_long40(const CdjC674x *cpu, unsigned bank,
                                unsigned reg)
{
    return ((uint64_t)(cpu->r[bank][reg + 1] & 0xff) << 32) |
           cpu->r[bank][reg];
}

static uint32_t shift_32(uint32_t source, unsigned count, unsigned operation)
{
    if (operation == 0) return count >= 32 ? 0 : source << count;
    if (operation == 2) return count >= 32 ? 0 : source >> count;
    if (count >= 32) return (source & 0x80000000u) ? UINT32_MAX : 0;
    if (!count) return source;
    return (source >> count) |
           ((source & 0x80000000u) ? UINT32_MAX << (32 - count) : 0);
}

/* Convert a 32-bit integer to IEEE-754 binary32 without depending on the
 * host floating-point environment.  FADCR modes are nearest-even, toward
 * zero, toward +infinity and toward -infinity (SPRUFE8B Table 2-25). */
static uint32_t integer_to_sp(uint32_t source, bool signed_source,
                              unsigned rmode, bool *inexact)
{
    bool negative = signed_source && (source & 0x80000000u);
    uint32_t magnitude = negative ? 0u - source : source;
    *inexact = false;
    if (!magnitude) return 0;
    unsigned msb = 31;
    while (!(magnitude & (1u << msb))) --msb;
    uint32_t mantissa;
    unsigned exponent = msb + 127;
    if (msb <= 23) {
        mantissa = magnitude << (23 - msb);
    } else {
        unsigned shift = msb - 23;
        uint32_t remainder_mask = (1u << shift) - 1;
        uint32_t remainder = magnitude & remainder_mask;
        mantissa = magnitude >> shift;
        *inexact = remainder != 0;
        bool increment = false;
        if (rmode == 0 && remainder) {
            uint32_t halfway = 1u << (shift - 1);
            increment = remainder > halfway ||
                        (remainder == halfway && (mantissa & 1));
        } else if (rmode == 2) increment = !negative && remainder;
        else if (rmode == 3) increment = negative && remainder;
        if (increment && ++mantissa == (1u << 24)) {
            mantissa >>= 1;
            ++exponent;
        }
    }
    return (negative ? 0x80000000u : 0) | exponent << 23 |
           (mantissa & 0x7fffffu);
}

typedef struct { uint32_t value, status; } SpResult;

static SpResult compare_sp(uint32_t left, uint32_t right, unsigned relation)
{
    unsigned le = (left >> 23) & 255, re = (right >> 23) & 255;
    uint32_t lf = left & 0x7fffff, rf = right & 0x7fffff;
    bool lnan = le == 255 && lf, rnan = re == 255 && rf;
    bool lden = !le && lf, rden = !re && rf;
    uint32_t status = (lnan ? 1u : 0) | (rnan ? 2u : 0) |
                      (lden ? 4u : 0) | (rden ? 8u : 0);
    if (lnan || rnan) {
        status |= 1u << 9;             /* UNORD */
        if (relation) status |= 1u << 4; /* ordered compare is invalid */
        return (SpResult){0, status};
    }
    if (lden) left &= 0x80000000u;
    if (rden) right &= 0x80000000u;
    bool both_zero = !(left << 1) && !(right << 1);
    bool equal = both_zero || left == right;
    bool less;
    if (equal) less = false;
    else if ((left ^ right) & 0x80000000u) less = (left >> 31) != 0;
    else less = (left >> 31) ? left > right : left < right;
    uint32_t value = relation == 0 ? equal : relation == 1 ? (!equal && !less) : less;
    return (SpResult){value, status};
}

static uint32_t right_shift_jam32(uint32_t value, unsigned count)
{
    if (!count) return value;
    if (count < 32)
        return (value >> count) | ((value << (32 - count)) != 0);
    return value != 0;
}

/* operation is add, src1-src2, or src2-src1.  Warning bits always describe
 * the encoded src1/src2 fields, including the reversed .S SUBSP form. */
static SpResult add_sub_sp(uint32_t source1, uint32_t source2,
                           unsigned operation, unsigned rmode)
{
    unsigned e1 = (source1 >> 23) & 255, e2 = (source2 >> 23) & 255;
    uint32_t f1 = source1 & 0x7fffff, f2 = source2 & 0x7fffff;
    bool nan1 = e1 == 255 && f1, nan2 = e2 == 255 && f2;
    bool inf1 = e1 == 255 && !f1, inf2 = e2 == 255 && !f2;
    bool den1 = !e1 && f1, den2 = !e2 && f2;
    uint32_t status = (nan1 ? 1u : 0) | (nan2 ? 2u : 0) |
                      (den1 ? 4u : 0) | (den2 ? 8u : 0);
    if (nan1 || nan2) {
        if ((nan1 && !(f1 & 0x400000)) || (nan2 && !(f2 & 0x400000)))
            status |= 1u << 4;
        return (SpResult){0x7fffffffu, status};
    }
    if (den1 && !inf2) status |= 1u << 7;
    if (den2 && !inf1) status |= 1u << 7;

    uint32_t left = source1, right = source2;
    if (operation == 2) {
        left = source2;
        right = source1;
    }
    if (operation) right ^= 0x80000000u;
    unsigned le = (left >> 23) & 255, re = (right >> 23) & 255;
    uint32_t lf = left & 0x7fffff, rf = right & 0x7fffff;
    if (!le && lf) left &= 0x80000000u;
    if (!re && rf) right &= 0x80000000u;
    bool linf = le == 255 && !lf, rinf = re == 255 && !rf;
    if (linf || rinf) {
        if (linf && rinf && ((left ^ right) & 0x80000000u))
            return (SpResult){0x7fffffffu, status | (1u << 4)};
        uint32_t infinity = linf ? left : right;
        return (SpResult){infinity & 0xff800000u, status | (1u << 5)};
    }

    le = (left >> 23) & 255; re = (right >> 23) & 255;
    bool lzero = le == 0, rzero = re == 0;
    unsigned lsign = left >> 31, rsign = right >> 31;
    if (lzero && rzero) {
        unsigned sign = lsign == rsign ? lsign : (rmode == 3);
        return (SpResult){sign << 31, status};
    }
    if (lzero) return (SpResult){right, status};
    if (rzero) return (SpResult){left, status};

    uint32_t lsig = (0x800000u | (left & 0x7fffff)) << 3;
    uint32_t rsig = (0x800000u | (right & 0x7fffff)) << 3;
    int exponent;
    if (le > re) {
        rsig = right_shift_jam32(rsig, le - re);
        exponent = le;
    } else if (re > le) {
        lsig = right_shift_jam32(lsig, re - le);
        exponent = re;
    } else exponent = le;

    uint32_t significand;
    unsigned sign;
    if (lsign == rsign) {
        sign = lsign;
        significand = lsig + rsig;
        if (significand & (1u << 27)) {
            significand = right_shift_jam32(significand, 1);
            ++exponent;
        }
    } else {
        if (lsig == rsig)
            return (SpResult){rmode == 3 ? 0x80000000u : 0, status};
        if (lsig > rsig) {
            sign = lsign; significand = lsig - rsig;
        } else {
            sign = rsign; significand = rsig - lsig;
        }
        while (!(significand & (1u << 26))) {
            significand <<= 1;
            --exponent;
        }
    }

    if (exponent <= 0) {
        bool smallest = (rmode == 2 && !sign) || (rmode == 3 && sign);
        return (SpResult){sign << 31 | (smallest ? 0x00800000u : 0),
                          status | (1u << 8) | (1u << 7)};
    }
    uint32_t mantissa = significand >> 3;
    unsigned remainder = significand & 7;
    bool increment = (rmode == 0 &&
                      (remainder > 4 || (remainder == 4 && (mantissa & 1)))) ||
                     (rmode == 2 && !sign && remainder) ||
                     (rmode == 3 && sign && remainder);
    if (increment && ++mantissa == (1u << 24)) {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent >= 255) {
        bool infinity = rmode == 0 || (rmode == 2 && !sign) ||
                        (rmode == 3 && sign);
        uint32_t result = sign << 31 |
                          (infinity ? 0x7f800000u : 0x7f7fffffu);
        status |= (1u << 7) | (1u << 6) | (infinity ? 1u << 5 : 0);
        return (SpResult){result, status};
    }
    if (remainder) status |= 1u << 7;
    return (SpResult){sign << 31 | (uint32_t)exponent << 23 |
                      (mantissa & 0x7fffff), status};
}

/* C674x MPYSP treats denormal sources as signed zero and flushes an
 * underflow result to signed zero or the smallest normal according to FMCR.
 * Normal finite multiplication is evaluated exactly as a 48-bit integer and
 * rounded once, avoiding host FP and excess-precision differences. */
static SpResult multiply_sp(uint32_t left, uint32_t right, unsigned rmode)
{
    unsigned sign = (left ^ right) & 0x80000000u;
    unsigned le = (left >> 23) & 255, re = (right >> 23) & 255;
    uint32_t lf = left & 0x7fffff, rf = right & 0x7fffff;
    bool lnan = le == 255 && lf, rnan = re == 255 && rf;
    bool linf = le == 255 && !lf, rinf = re == 255 && !rf;
    bool lden = !le && lf, rden = !re && rf;
    bool lzero = !le && !lf, rzero = !re && !rf;
    uint32_t status = (lnan ? 1u : 0) | (rnan ? 2u : 0) |
                      (lden ? 4u : 0) | (rden ? 8u : 0);
    if (lnan || rnan) {
        if ((lnan && !(lf & 0x400000)) || (rnan && !(rf & 0x400000)))
            status |= 1u << 4;
        return (SpResult){sign | 0x7fffffffu, status};
    }
    if ((linf && (rzero || rden)) || (rinf && (lzero || lden)))
        return (SpResult){sign | 0x7fffffffu, status | (1u << 4)};
    if (linf || rinf)
        return (SpResult){sign | 0x7f800000u, status | (1u << 5)};
    if (lzero || rzero || lden || rden) {
        if ((lden && !rzero && !rden) || (rden && !lzero && !lden))
            status |= 1u << 7;
        return (SpResult){sign, status};
    }
    uint64_t product = (uint64_t)(0x800000u | lf) * (0x800000u | rf);
    unsigned msb = (product & (UINT64_C(1) << 47)) ? 47 : 46;
    int exponent = (int)le + (int)re - 127 + (int)msb - 46;
    unsigned shift = msb - 23;
    uint32_t mantissa = product >> shift;
    uint64_t remainder = product & ((UINT64_C(1) << shift) - 1);
    bool inexact = remainder != 0, increment = false;
    if (rmode == 0 && remainder) {
        uint64_t halfway = UINT64_C(1) << (shift - 1);
        increment = remainder > halfway ||
                    (remainder == halfway && (mantissa & 1));
    } else if (rmode == 2) increment = !sign && remainder;
    else if (rmode == 3) increment = sign && remainder;
    if (increment && ++mantissa == (1u << 24)) {
        mantissa >>= 1;
        ++exponent;
    }
    if (exponent >= 255) {
        bool infinity = rmode == 0 || (rmode == 2 && !sign) ||
                        (rmode == 3 && sign);
        uint32_t result = sign | (infinity ? 0x7f800000u : 0x7f7fffffu);
        status |= (1u << 7) | (1u << 6) | (infinity ? 1u << 5 : 0);
        return (SpResult){result, status};
    }
    if (exponent <= 0) {
        bool smallest = (rmode == 2 && !sign) || (rmode == 3 && sign);
        status |= (1u << 8) | (1u << 7);
        return (SpResult){sign | (smallest ? 0x00800000u : 0), status};
    }
    if (inexact) status |= 1u << 7;
    return (SpResult){sign | (uint32_t)exponent << 23 |
                      (mantissa & 0x7fffff), status};
}

/* SPINT/SPTRUNC convert the binary32 bit pattern directly.  Invalid and
 * overflow inputs saturate by sign; denormals become zero.  SPINT uses FADCR
 * rounding while SPTRUNC supplies mode 1 regardless of FADCR. */
static SpResult sp_to_integer(uint32_t source, unsigned rmode)
{
    bool negative = (source & 0x80000000u) != 0;
    unsigned exponent = (source >> 23) & 255;
    uint32_t fraction = source & 0x7fffff;
    uint32_t saturated = negative ? 0x80000000u : 0x7fffffffu;
    if (exponent == 255) {
        if (fraction)
            return (SpResult){saturated, (1u << 4) | (1u << 1)};
        return (SpResult){saturated, (1u << 7) | (1u << 6)};
    }
    if (!exponent) {
        if (fraction) return (SpResult){0, (1u << 7) | (1u << 3)};
        return (SpResult){0, 0};
    }
    int power = (int)exponent - 127;
    uint64_t significand = 0x800000u | fraction;
    uint64_t magnitude, remainder = 0, halfway = 0;
    if (power >= 23) {
        if (power > 31) return (SpResult){saturated, (1u << 7) | (1u << 6)};
        magnitude = significand << (power - 23);
    } else if (power >= 0) {
        unsigned shift = 23 - power;
        magnitude = significand >> shift;
        remainder = significand & ((UINT64_C(1) << shift) - 1);
        halfway = UINT64_C(1) << (shift - 1);
    } else {
        magnitude = 0;
        remainder = significand;
        if (power == -1) halfway = UINT64_C(1) << 23;
        else halfway = UINT64_MAX; /* magnitude is strictly below one half */
    }
    if (remainder) {
        bool increment = (rmode == 0 &&
                          (remainder > halfway ||
                           (remainder == halfway && (magnitude & 1)))) ||
                         (rmode == 2 && !negative) ||
                         (rmode == 3 && negative);
        if (increment) ++magnitude;
    }
    if ((!negative && magnitude > 0x7fffffffu) ||
        (negative && magnitude > 0x80000000u))
        return (SpResult){saturated, (1u << 7) | (1u << 6)};
    uint32_t result = negative ? 0u - (uint32_t)magnitude : magnitude;
    return (SpResult){result, remainder ? 1u << 7 : 0};
}

/* Return the number of cycles inserted by a NOP encoding, or zero when the
 * instruction is not a NOP. The compact H-9 N3 field encodes count - 1;
 * SPRUFE8B labels the operand N3, but TI dis6x and GNU binutils agree on the
 * +1 translation used here. */
static unsigned nop_cycles(const CdjC674xInstruction *insn)
{
    if (insn->compact)
        return (insn->word & 0x1fffu) == 0x0c6eu ? (insn->word >> 13) + 1 : 0;
    if ((insn->word & 0xfffe1ffeu) == 0)
        return ((insn->word >> 13) & 15) + 1;
    return 0;
}

typedef enum {
    CDJ_C674X_INTERRUPT_GATE_NONE,
    CDJ_C674X_INTERRUPT_GATE_DISABLE,
    CDJ_C674X_INTERRUPT_GATE_RESTORE,
} CdjC674xInterruptGate;

/* DINT/RINT are unconditional no-unit instructions. Their high nibble is
 * not a normal predicate field (SPRUFE8B DINT/RINT entries). GNU's no-unit
 * formats expose an s bit for table uniformity, but both require s=0; only
 * the parallel bit is variable. */
static CdjC674xInterruptGate interrupt_gate_decode(
    const CdjC674xInstruction *insn)
{
    if (insn->compact) return CDJ_C674X_INTERRUPT_GATE_NONE;
    switch (insn->word & ~1u) {
    case 0x10004000u: return CDJ_C674X_INTERRUPT_GATE_DISABLE;
    case 0x10006000u: return CDJ_C674X_INTERRUPT_GATE_RESTORE;
    default: return CDJ_C674X_INTERRUPT_GATE_NONE;
    }
}

/* SPRUFE8B 3.8.11.3/6 list the operations which may not share an execute
 * packet with DINT or RINT. Keep this format check separate from execution:
 * several members intentionally remain unsupported, but must still reject
 * the whole packet atomically rather than being hidden by decode order. */
static bool interrupt_gate_parallel_conflict(
    CdjC674xInterruptGate gate, const CdjC674xInstruction *insn)
{
    CdjC674xInterruptGate other = interrupt_gate_decode(insn);
    if (other != CDJ_C674X_INTERRUPT_GATE_NONE) return other != gate;

    uint32_t w = insn->word;
    if (nop_cycles(insn) > 1) return true; /* Includes full-width IDLE. */
    if (insn->compact) {
        return (w & 0xbc7eu) == 0x0c66u || /* SPLOOP(D), reload form. */
               (w & 0x3c7eu) == 0x1c66u || /* SPKERNEL. */
               (w & 0x3c7eu) == 0x2c66u || /* SPMASK. */
               (w & 0x3c7eu) == 0x3c66u;   /* SPMASKR. */
    }
    if ((w & ~1u) == 0x10000000u || /* SWE. */
        (w & ~1u) == 0x10002000u || /* SWENR. */
        (w & ~1u) == 0x00036000u)   /* SPKERNELR. */
        return true;
    if ((w & 0xf03ffffeu) == 0x00034000u) return true; /* SPKERNEL. */
    if ((w & 0x007ffffcu) == 0x00038000u || /* SPLOOP. */
        (w & 0x007ffffcu) == 0x0003a000u || /* SPLOOPD. */
        (w & 0x007ffffeu) == 0x0003e000u)   /* SPLOOPW. */
        return true;
    if ((w & 0xfc03fffeu) == 0x00030000u || /* SPMASK. */
        (w & 0xfc03fffeu) == 0x00032000u)   /* SPMASKR. */
        return true;
    if ((w & 0x0ffffffeu) == 0x001800e2u || /* B IRP. */
        (w & 0x0ffffffeu) == 0x001c00e2u)   /* B NRP. */
        return true;
    if ((w & 0xffeu) == 0x3a2u && ((w >> 13) & 31) == 0) {
        unsigned control = (w >> 23) & 31;
        if (control == 1 || control == 26) return true; /* MVC reg,CSR/TSR. */
    }
    return false;
}

/* SPMASK unit bits are L1,L2,S1,S2,D1,D2,M1,M2. GNU's opcode table
 * corrects the full-width opcode in SPRUFE8B; compact is Figure H-8. */
static bool spmask_decode(const CdjC674xInstruction *insn, unsigned *mask)
{
    uint32_t w = insn->word;
    if (insn->compact && (w & 0x3c7e) == 0x2c66) {
        *mask = (w & 1) | ((w >> 6) & 14) | ((w >> 10) & 48);
        return true;
    }
    if (!insn->compact && (w & 0xfc03fffe) == 0x30000) {
        *mask = (w >> 18) & 255;
        return true;
    }
    return false;
}

/* Format-level unit classification (SPRUFE8B appendices C-G). Zero means
 * unknown: never infer that an unknown operation is safe to mask or replay.
 * This classifies units, not opcode validity; execution still validates ISA. */
static unsigned instruction_unit(const CdjC674xInstruction *insn)
{
    uint32_t w = insn->word;
    unsigned side = insn->compact ? w & 1 : (w >> 1) & 1;
    if (!insn->compact) {
        if ((w & 0x0c) == 12) return 32u; /* Long offsets always use .D2. */
        if ((w & 0x1c) == 0x18) return 1u << side;
        if ((w & 0x0c) == 4 || (w & 0x0c) == 12 ||
            (w & 0x7c) == 0x40 || (w & 0xc3c) == 0x830) return 16u << side;
        if ((w & 0x3c) == 0x20 || (w & 0x3c) == 0x28 ||
            (w & 0x3c) == 8 || (w & 0x7c) == 0x10 ||
            (w & 0x7c) == 0x50 || (w & 0xc3c) == 0xc30) return 4u << side;
        if ((w & 0x7c) == 0 || (w & 0x83c) == 0x30) return 64u << side;
        return 0;
    }
    if (((w & 0x26) == 6 || (w & 0x1c66) == 0x0866 ||
         (w & 0x1c66) == 0x1866) && ((w >> 3) & 3) != 3)
        return 1u << (2 * ((w >> 3) & 3) + side);
    if ((w & 6) == 4 || (w & 0x087f) == 0x0077 ||
        (w & 0x047e) == 0x0036 || (w & 0x047e) == 0x0436 ||
        (w & 0x1c7e) == 0x0c76) return 16u << side;
    if ((w & 0x040e) == 0 || (w & 0x040e) == 0x400 ||
        (w & 0x040e) == 0x408 || (w & 0x047e) == 0x426 ||
        (w & 0x047e) == 0x26) return 1u << side;
    if ((w & 0x001e) == 0x001e) return 64u << side;
    if ((w & 0x040e) == 0xa || (w & 0x040e) == 0x40a ||
        (w & 0x001e) == 0x12 || (w & 0x001e) == 2 ||
        (w & 0x047e) == 0x62 || (w & 0x047e) == 0x462 ||
        (w & 0x047e) == 0x2e || (w & 0x047e) == 0x42e ||
        (w & 0x187e) == 0x6e || (w & 0x1c7e) == 0x186e) return 4u << side;
    return 0;
}

typedef struct { CdjC674x *cpu; unsigned mask; bool unknown; } LoopMask;
static bool compact_branch(const CdjC674xInstruction *i)
{
    unsigned w = i->word;
    return i->compact && ((w & 0x187f) == 0x006f ||
        ((i->header & 0x8000) && ((w & 0x3e) == 0x0a ||
         (w & 0x3e) == 0x1a || (w & 0x2e) == 0x2a)));
}
static bool protected_load(const CdjC674xInstruction *i)
{
    if (!(i->header & (1u << 20))) return false;
    uint32_t w = i->word;
    if (i->compact) {
        /* Figure C-21 Dpp uses bit 14 for load/store, unlike the other
         * compact .D formats which use bit 3.  PROT applies to every LD in
         * the fetch packet regardless of compact format (section 3.10.2.2). */
        if ((w & 0x087f) == 0x0077) return (w & 0x4000) != 0;
        return (w & 6) == 4 && (w & 8);
    }
    if ((w & 0x0c) == 12) {
        unsigned op = (w >> 4) & 7;
        return op != 3 && op != 5 && op != 7;
    }
    if ((w & 0x10c) != 4 && (w & 0x17c) != 0x134 &&
        (w & 0x17c) != 0x154 && (w & 0x17c) != 0x124 &&
        (w & 0x17c) != 0x174 && (w & 0x17c) != 0x164 &&
        (w & 0x17c) != 0x144) return false;
    unsigned op = (w >> 4) & 7;
    return op != 5 && op != 7 && op != ((w & 0x100) ? 4u : 3u);
}
static bool loop_allow(void *opaque, uint32_t tag)
{
    LoopMask *context = opaque;
    if (!context->mask) return true;
    unsigned unit = instruction_unit(&context->cpu->loop_instructions[tag]);
    if (!unit) context->unknown = true;
    return !(unit & context->mask);
}

/* Side-effect-free RAM reads; nonaligned words may span two bus words. */
static bool read_scalar(CdjC674xRead read, void *opaque, uint32_t address,
                        unsigned size, uint64_t *value)
{
    if ((uint64_t)address + size > UINT64_C(0x100000000)) return false;
    *value = 0;
    for (unsigned done = 0; done < size;) {
        uint32_t word, current = address + done;
        if (!read(opaque, current & ~3u, &word)) return false;
        unsigned lane = current & 3, n = 4 - lane;
        if (n > size - done) n = size - done;
        word >>= lane * 8;
        if (n < 4) word &= (1u << (n * 8)) - 1;
        *value |= (uint64_t)word << (done * 8);
        done += n;
    }
    return true;
}

void cdj_c674x_reset(CdjC674x *cpu, uint32_t entry)
{
    memset(cpu, 0, sizeof(*cpu));
    /* C674x CPU ID 0x14, little endian, and reset interrupt enabled.
     * C6747 HOST1CFG resets the interrupt table base to DSP ROM 0x00700000. */
    cpu->control[1] = 0x14000100;
    cpu->control[4] = 1;
    cpu->control[5] = 0x00700000;
    cpu->pc = entry;
}

static bool stop(CdjC674x *cpu, uint32_t pc, uint32_t word, const char *why)
{
    cpu->fault = why;
    cpu->fault_pc = pc;
    cpu->fault_word = word;
    return false;
}

bool cdj_c674x_interrupt(CdjC674x *cpu, uint32_t pending)
{
    const uint32_t maskable = 0x0000fff0u;
    if (cpu->fault) return false;
    if (pending & ~maskable)
        return stop(cpu, cpu->pc, 0, "invalid CPU interrupt request mask");

    /* This interface starts after system-event selection/INTMUX. The caller
     * presents CPU INT4..15 requests, not raw C6747 events. SPRUFE8B 5.4.1
     * makes IFR sticky until ICR or acceptance clears a bit. */
    cpu->control[2] = (cpu->control[2] | pending) & maskable;
    uint32_t eligible = cpu->control[2] & cpu->control[4] & maskable;
    if (!(cpu->control[1] & 1u) || !(cpu->control[4] & 2u) || !eligible)
        return true;

    /* Section 5.4.2 forbids recognition in a branch's five delay packets.
     * The interpreter represents taken branches explicitly, so defer while
     * one is live. False conditional branches do not yet have pipeline state.
     */
    if (cpu->branch_due || cpu->branch_count) return true;
    if (cpu->loop_active)
        return stop(cpu, cpu->pc, 0,
                    "maskable interrupt during SPLOOP not implemented");
    if (cpu->control[26] & (1u << 9))
        return stop(cpu, cpu->pc, 0,
                    "nested maskable interrupt not implemented");

    /* INT4 has highest maskable priority (Table 5-1). The interpreter has no
     * speculative fetch pipeline, so the current PC is exactly the first
     * execute packet annulled by the interrupt and therefore the IRP value.
     * Existing delayed E2..E5 effects belong to older, non-annulled packets
     * and remain queued to mature while the handler executes (5.4.4). */
    unsigned interrupt = 4;
    while (!(eligible & (1u << interrupt))) ++interrupt;
    uint32_t saved_tsr = (cpu->control[26] & 0x0000c6deu) |
                         (cpu->control[1] & 1u);
    saved_tsr &= ~((1u << 15) | (1u << 14));
    cpu->control[27] = saved_tsr;
    cpu->control[6] = cpu->pc;
    cpu->control[2] &= ~(1u << interrupt);

    /* Table 5-3: save TSR in ITSR, enter supervisor interrupt context, retain
     * GEE/DBGM, clear GIE/SGIE/XEN/CXM/EXC/SPLX, and assert INT/IB. PGIE and
     * ITSR.GIE are the same physical bit. */
    cpu->control[1] = (cpu->control[1] & ~3u) |
                      ((saved_tsr & 1u) << 1);
    cpu->control[26] = (saved_tsr & ((1u << 4) | (1u << 2))) |
                       (1u << 15) | (1u << 9);
    cpu->pc = (cpu->control[5] & 0xfffffc00u) + interrupt * 32u;
    return true;
}

/* One taken branch may enter E1 each cycle; all six pipeline positions can
 * be occupied. Preserve later branches when an older branch redirects fetch. */
static bool queue_branch(CdjC674x *out, uint64_t due, uint32_t target)
{
    if (!out->branch_due) {
        out->branch_due = due; out->branch_target = target;
        return true;
    }
    if (out->branch_due == due || out->branch_count == 5 ||
        (out->branch_count && out->branch_queue[out->branch_count - 1].due == due)) return false;
    out->branch_queue[out->branch_count].due = due;
    out->branch_queue[out->branch_count++].target = target;
    return true;
}

bool cdj_c674x_fetch(CdjC674x *cpu, CdjC674xRead read, void *opaque,
                     CdjC674xPacket *packet)
{
    CdjC674xPacket result = {0};
    uint32_t next = cpu->pc;
    unsigned count = 0;
    bool parallel = true;
    if (cpu->fault) return false;
    /* Validate the entire packet before committing any architectural state. */
    do {
        uint32_t header, word;
        if (next & 1) return stop(cpu, next, 0, "unaligned instruction fetch");
        if (!read(opaque, (next & ~31u) + 28, &header))
            return stop(cpu, next, 0, "unmapped instruction fetch");
        bool mixed = (header >> 28) == 14;
        /* The header occupies no execution slot. */
        if (mixed && (next & 31) == 28) {
            next += 4;
            continue;
        }
        if (mixed && (next & 31) == 30)
            return stop(cpu, next, header, "instruction points into compact header");
        unsigned slot = (next & 31) / 4;
        bool short_word = mixed && ((header >> (21 + slot)) & 1);
        if (!short_word && (next & 3))
            return stop(cpu, next, 0, "unaligned full instruction fetch");
        if (!read(opaque, next & ~3u, &word))
            return stop(cpu, next, 0, "unmapped instruction fetch");
        if (count == 8) return stop(cpu, next, word, "execute packet exceeds eight instructions");
        if (short_word) word = (word >> ((next & 2) * 8)) & 0xffff;
        parallel = short_word ? ((header >> ((next & 31) / 2)) & 1) : (word & 1);
        result.instructions[count++] = (CdjC674xInstruction){
            .pc = next, .word = word, .compact = short_word, .header = mixed ? header : 0
        };
        next += short_word ? 2 : 4;
        if (mixed && (next & 31) == 28) next += 4;
    } while (parallel);
    result.count = count;
    result.next_pc = next;
    *packet = result;
    return true;
}

bool cdj_c674x_execute(CdjC674x *cpu, const CdjC674xPacket *packet,
                       CdjC674xRead read, CdjC674xWrite write, void *opaque)
{
    unsigned elapsed = 1, memory_count = 0;
    bool nonaligned_memory = false;
    CdjC674x out = *cpu;
    bool written[2][32] = {{false}}, controls[32] = {false};
    if (cpu->fault) return false;
    if (packet->count > 8) return stop(cpu, cpu->pc, 0, "execute packet exceeds eight instructions");
    for (unsigned i = 0; i < packet->count; ++i) {
        const CdjC674xInstruction *insn = &packet->instructions[i];
        uint32_t w = insn->word, pc = insn->pc, value = 0;
        bool compact = insn->compact;
        unsigned ignored_mask;
        if (spmask_decode(insn, &ignored_mask)) {
            if (i) return stop(cpu, pc, w, "SPMASK must start packet");
            continue; /* Idle loop buffer: SPMASK is a NOP, section 7.15. */
        }
        unsigned nop = nop_cycles(insn);
        if (nop) {
            if (nop > 9) return stop(cpu, pc, insn->word, "reserved NOP count");
            if (nop > 1 && elapsed > 1)
                return stop(cpu, pc, insn->word, "multiple multicycle instructions");
            if (nop > elapsed) elapsed = nop;
            continue;
        }
        /* Compact-header PROT inserts four cycles after every load in the
         * fetch packet, including Dpp/Dstk forms handled by early exits
         * below and 32-bit loads in a mixed packet.  Establish the packet's
         * multicycle duration before format-specific lowering so all load
         * families receive identical timing. */
        if (protected_load(insn)) {
            if (elapsed > 1)
                return stop(cpu, pc, insn->word,
                            "multiple multicycle instructions");
            elapsed = 5;
        }
        /* Figures C-8 through C-15: lower the compact .D memory families to
         * the existing E1/E3/E5 scalar pipeline.  Pointer registers are
         * always A/B4-7 and ignore RS; data and register-offset operands use
         * RS.  Dinc is scaled post-increment, Ddec scaled pre-decrement.
         * The nonaligned doubleword offset is unscaled for Doff/Dind but
         * scaled for Dinc/Ddec, as called out by Figures C-9/11/13/15. */
        bool compact_doff = compact && (w & 0x0406) == 0x0004;
        bool compact_dind = compact && (w & 0x0c06) == 0x0404;
        bool compact_dinc = compact && (w & 0xcc06) == 0x0c04;
        bool compact_ddec = compact && (w & 0xcc06) == 0x4c04;
        if (compact_doff || compact_dind || compact_dinc || compact_ddec) {
            unsigned dsz = (insn->header >> 16) & 7;
            bool load = (w & 8) != 0, secondary = (w & 0x200) != 0;
            unsigned reg = ((w >> 4) & 7) + ((insn->header & 0x80000) ? 16 : 0);
            unsigned op, extended = 0;
            bool nonaligned_pair = false;
            if (!secondary && (dsz & 4)) {
                bool nonaligned = (w & 0x10) != 0;
                reg &= ~1u;
                op = load ? (nonaligned ? 2 : 6) : (nonaligned ? 7 : 4);
                extended = 0x100;
                nonaligned_pair = nonaligned;
            } else if (!secondary) op = load ? 6 : 7;
            else {
                static const unsigned loads[8] = {1, 2, 0, 4, 6, 2, 3, 4};
                static const unsigned stores[8] = {3, 3, 5, 5, 7, 3, 5, 5};
                op = load ? loads[dsz] : stores[dsz];
                if (dsz == 6) extended = 0x100;
            }
            unsigned offset, mode;
            if (compact_doff) {
                offset = ((w >> 13) & 7) | (((w >> 11) & 1) << 3);
                mode = 1;              /* positive constant offset */
            } else if (compact_dind) {
                offset = ((w >> 13) & 7) +
                         ((insn->header & 0x80000) ? 16 : 0);
                mode = 5;              /* positive register offset */
            } else {
                offset = ((w >> 13) & 1) + 1;
                mode = compact_dinc ? 11 : 8; /* postincrement / predecrement */
            }
            unsigned scaled_nonaligned = nonaligned_pair &&
                (compact_dinc || compact_ddec) ? 1u << 23 : 0;
            w = reg << 23 | (4 + ((w >> 7) & 3)) << 18 | offset << 13 |
                mode << 9 | scaled_nonaligned | extended | (w & 1) << 7 |
                op << 4 | 4 | ((w >> 12) & 1) << 1;
            compact = false;
        }
        unsigned side = (w >> 1) & 1, dst = (w >> 23) & 31;
        unsigned a = (w >> 13) & 31, b = (w >> 18) & 31;
        unsigned cross = side ^ ((w >> 12) & 1);
        bool callp = (!compact && (w & 0xf000007c) == 0x10000010) ||
                     (compact && (insn->header & 0x8000) && (w & 0x3e) == 0x1a);
        if (callp) {
            side = compact ? w & 1 : (w >> 1) & 1;
            /* CALLP retains a word-scaled PC-relative displacement in its
             * compact Scs10 form (SPRUFE8B CALLP description / Figure F-19).
             * Compact BNOP is the branch family that uses halfword scaling. */
            int32_t offset = compact ? sx(w >> 6, 10) * 4
                                          : sx((w >> 7) & 0x1fffff, 21) * 4;
            if (out.branch_due || elapsed > 1)
                return stop(cpu, pc, insn->word, "CALLP with pending branch or multicycle instruction");
            for (unsigned j = 0; j < packet->count; ++j) {
                if (j == i) continue;
                CdjC674xInstruction other = packet->instructions[j];
                if ((!other.compact && ((other.word & 0x7c) == 0x10 ||
                     (other.word & 0x1ffe) == 0x162 || (other.word & 0xffe) == 0x362 ||
                     (other.word & 0x1ffc) == 0x120)) ||
                    compact_branch(&other))
                    return stop(cpu, pc, insn->word, "CALLP with parallel control instruction");
            }
            if (written[side][3]) return stop(cpu, pc, insn->word, "parallel register write conflict");
            out.r[side][3] = packet->next_pc; written[side][3] = true;
            if (!queue_branch(&out, cpu->cycles + 6, (pc & ~31u) + (uint32_t)offset))
                return stop(cpu, pc, insn->word, "CALLP branch queue conflict");
            elapsed = 6;
            continue;
        }
        if (compact) {
            unsigned rs = (insn->header & 0x80000) ? 16 : 0;
            bool simple = true;
            side = w & 1;
            cross = side ^ ((w >> 12) & 1);
            if ((w & 0x1c66) == 0x0866 && ((w >> 3) & 3) != 3) {
                /* Figure G-3: [A0/!A0/B0/!B0] MVK 0/1 on L/S/D. */
                unsigned cc = w >> 14;
                if (!((cpu->r[cc >> 1][0] != 0) ^ (cc & 1))) continue;
                dst = ((w >> 7) & 7) + rs;
                value = (w >> 13) & 1;
            } else if (!(insn->header & (1u << 15)) &&
                       (w & 0x040e) == 0x000a) {
                /* Figure F-22: compact .S ADD/SUB.  Header SAT changes ADD
                 * into SADD, whose delayed CSR.SAT behavior remains
                 * deliberately unsupported; SAT does not alter SUB. */
                bool subtract = (w & 0x0800) != 0;
                if (!subtract && (insn->header & (1u << 14)))
                    return stop(cpu, pc, insn->word,
                                "compact SADD saturation not implemented");
                dst = ((w >> 4) & 7) + rs;
                uint32_t left = cpu->r[side][((w >> 13) & 7) + rs];
                uint32_t right = cpu->r[cross][((w >> 7) & 7) + rs];
                value = subtract ? left - right : left + right;
            } else if ((w & 0x047e) == 0x042e) {
                /* SPRUFE8B Figure F-30, Sx5: compact ADDK adds its
                 * unsigned five-bit constant to the destination in place.
                 * The three-bit destination observes header RS. */
                unsigned constant = ((w >> 13) & 7) |
                                    (((w >> 11) & 3) << 3);
                dst = ((w >> 7) & 7) + rs;
                value = cpu->r[side][dst] + constant;
            } else if ((w & 0x047e) == 0x0436) {
                /* SPRUFE8B Figure C-18, Dx5: B15 plus a word-scaled
                 * unsigned five-bit constant.  B15 is fixed and ignores
                 * RS; the three-bit destination observes RS. */
                unsigned constant = ((w >> 13) & 7) |
                                    (((w >> 11) & 3) << 3);
                dst = ((w >> 7) & 7) + rs;
                value = cpu->r[1][15] + constant * 4;
            } else if ((w & 0x1c7f) == 0x0c77) {
                /* SPRUFE8B Figure C-19, Dx5p: the only architectural
                 * side is D2 (s=1), and both operands are the fixed B15.
                 * Rejecting s=0 below keeps the reserved encoding closed. */
                unsigned constant = ((w >> 13) & 7) |
                                    (((w >> 8) & 3) << 3);
                side = 1;
                dst = 15;
                value = (w & 0x0080) ? cpu->r[1][15] - constant * 4
                                     : cpu->r[1][15] + constant * 4;
            } else if ((w & 0x047e) == 0x0036) {
                /* Figure C-17: compact .D in-place ADD/SUB. */
                dst = ((w >> 13) & 7) + rs;
                uint32_t left = cpu->r[side][dst];
                uint32_t right = cpu->r[cross][((w >> 7) & 7) + rs];
                value = (w & 0x0800) ? left - right : left + right;
            } else if (!(insn->header & (1u << 15)) &&
                       (w & 0x040e) == 0x040a) {
                /* Figure F-23: compact .S SHL/SHR with the special
                 * 0->16 and 7->8 constant translation. */
                unsigned encoded = (w >> 13) & 7;
                unsigned count = encoded == 0 ? 16 : encoded == 7 ? 8 : encoded;
                dst = ((w >> 4) & 7) + rs;
                value = shift_32(cpu->r[cross][((w >> 7) & 7) + rs],
                                 count, (w >> 11) & 1);
            } else if ((w & 0x041e) == 0x0402 &&
                       (w & 0x047e) != 0x0462) {
                /* Figure F-25: compact in-place constant shifts.  SAT
                 * changes op 2 from SHRU into delayed-status SSHL. */
                unsigned op = (w >> 5) & 3;
                if (op == 3)
                    return stop(cpu, pc, insn->word,
                                "reserved compact Ssh5 instruction");
                if (op == 2 && (insn->header & (1u << 14)))
                    return stop(cpu, pc, insn->word,
                                "compact SSHL saturation not implemented");
                unsigned count = ((w >> 13) & 7) | (((w >> 11) & 3) << 3);
                dst = ((w >> 7) & 7) + rs;
                value = shift_32(cpu->r[side][dst], count, op);
            } else if ((w & 0x047e) == 0x0462) {
                /* Figure F-26: compact in-place register-count shifts.
                 * As for full-width scalar shifts, only the low six count
                 * bits participate. */
                unsigned op = (w >> 11) & 3;
                if (op == 3)
                    return stop(cpu, pc, insn->word,
                                "compact SSHL saturation not implemented");
                dst = ((w >> 7) & 7) + rs;
                unsigned count = cpu->r[side][((w >> 13) & 7) + rs] & 63;
                value = shift_32(cpu->r[side][dst], count, op);
            } else if ((w & 0x040e) == 0x0400) { /* Figure D-5, ADD.L immediate */
                dst = ((w >> 4) & 7) + rs;
                unsigned imm = (w >> 13) & 7;
                int32_t offset = (w & 0x800) ? (int32_t)imm - 8 : (imm ? (int32_t)imm : 8);
                value = cpu->r[cross][((w >> 7) & 7) + rs] + (uint32_t)offset;
            } else if ((w & 0x041e) == 2) { /* Figures F-27/F-28 */
                unsigned op = (w >> 5) & 3;
                unsigned src = ((w >> 7) & 7) + rs;
                uint32_t source = cpu->r[side][src];
                if (op == 3) {
                    unsigned kind = (w >> 11) & 3;
                    unsigned bits = (kind & 1) ? 8 : 16;
                    dst = ((w >> 13) & 7) + rs;
                    value = source & ((1u << bits) - 1);
                    if (!(kind & 2)) value = sx(value, bits);
                } else {
                    unsigned n = ((w >> 13) & 7) | (((w >> 11) & 3) << 3);
                    dst = op ? src : 0; /* EXTU always writes A0/B0, even RS=1. */
                    value = op == 0 ? (source >> (31 - n)) & 1 :
                            op == 1 ? source | (1u << n) : source & ~(1u << n);
                }
            } else if ((w & 0x001e) == 0x0012) { /* Figure F-24, unsigned MVK.S */
                dst = ((w >> 7) & 7) + rs;
                value = ((w >> 13) & 7) | (((w >> 11) & 3) << 3) |
                        (((w >> 5) & 3) << 5) | (((w >> 10) & 1) << 7);
            } else if ((w & 0x1c66) == 0x1866 &&
                       ((w >> 13) & 7) != 6) {
                /* Figure G-4 joins the compact .L/.S/.D zero, one,
                 * increment and XOR-one forms.  Unit-specific op 2 is
                 * integer negation on .L/.S and reserved on .D; op 3 is
                 * decrement on every unit.  Op 6 is the separate .S2 MVC
                 * to ILC handled below. */
                unsigned unit = (w >> 3) & 3;
                unsigned op = (w >> 13) & 7;
                if (unit == 3 || op == 4 || (op == 2 && unit == 2))
                    return stop(cpu, pc, insn->word,
                                "reserved compact LSDx1 instruction");
                dst = ((w >> 7) & 7) + rs;
                uint32_t source = cpu->r[side][dst];
                switch (op) {
                case 0: value = 0; break;
                case 1: value = 1; break;
                case 2: value = 0u - source; break;
                case 3: value = source - 1; break;
                case 5: value = source + 1; break;
                default: value = source ^ 1; break;
                }
            } else if ((w & 0x047e) == 0x0426) { /* Figure D-8, MVK.L */
                dst = ((w >> 7) & 7) + rs;
                value = sx(((w >> 13) & 7) | (((w >> 11) & 3) << 3), 5);
            } else if ((w & 0x147e) == 0x0026) { /* Figure D-9, CMPEQ immediate */
                dst = (w >> 11) & 1;
                value = ((w >> 13) & 7) == cpu->r[side][((w >> 7) & 7) + rs];
            } else if ((w & 0x147e) == 0x1026) { /* Figure D-10, ordered compare */
                dst = ((w >> 11) & 1) + rs;
                uint32_t constant = (w >> 13) & 1;
                uint32_t source = cpu->r[side][((w >> 7) & 7) + rs];
                switch (w >> 14) {
                case 0: value = (int32_t)constant < (int32_t)source; break;
                case 1: value = (int32_t)constant > (int32_t)source; break;
                case 2: value = constant < source; break;
                default: value = constant > source; break;
                }
            } else if ((w & 0x040e) == 0x0408) { /* Figure D-7, L2c */
                dst = (w >> 4) & 1;
                uint32_t left = cpu->r[side][((w >> 13) & 7) + rs];
                uint32_t right = cpu->r[cross][((w >> 7) & 7) + rs];
                unsigned op = ((w >> 9) & 4) | ((w >> 5) & 3);
                switch (op) {
                case 0: value = left & right; break;
                case 1: value = left | right; break;
                case 2: value = left ^ right; break;
                case 3: value = left == right; break;
                case 4: value = (int32_t)left < (int32_t)right; break;
                case 5: value = (int32_t)left > (int32_t)right; break;
                case 6: value = left < right; break;
                default: value = left > right; break;
                }
            } else simple = false;
            if (simple) {
                if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                out.r[side][dst] = value; written[side][dst] = true;
                continue;
            }
            if ((w & 0x187f) == 0x006f) { /* Figure F-32, register BNOP */
                unsigned n = w >> 13;
                if (n && elapsed > 1) return stop(cpu, pc, insn->word, "multiple multicycle instructions");
                if (n + 1 > elapsed) elapsed = n + 1;
                if (!queue_branch(&out, cpu->cycles + 6, cpu->r[1][(w >> 7) & 15]))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
                continue;
            }
            /* Figure F-31: compact MVC to ILC. SPLOOP observes a four-cycle
             * availability latency (section 7.4.3), tracked separately. */
            if ((w & 0xfc7f) == 0xd86f) {
                unsigned src = ((w >> 7) & 7) + ((insn->header & 0x80000) ? 16 : 0);
                if (controls[13]) return stop(cpu, pc, insn->word, "parallel control write conflict");
                out.control[13] = cpu->r[1][src];
                out.control_ready[13] = cpu->cycles + 4;
                controls[13] = true;
                continue;
            }
            /* Figures F-17/18/20/21: compact BNOP uses halfword offsets.
             * Predicate controls the branch, never the inserted NOPs. */
            if ((insn->header & 0x8000) &&
                ((w & 0x3e) == 0x0a || (w & 0x2e) == 0x2a)) {
                bool unsigned_offset = (w & 0xc000) == 0xc000;
                unsigned n = unsigned_offset ? 5 : w >> 13;
                int32_t displacement = unsigned_offset ? (int32_t)((w >> 6) & 255)
                                                       : sx((w >> 6) & 127, 7);
                bool enabled = !(w & 0x20) ||
                    ((cpu->r[w & 1][0] != 0) ^ ((w >> 4) & 1));
                if (n > 0 && elapsed > 1)
                    return stop(cpu, pc, insn->word, "multiple multicycle instructions");
                if (n + 1 > elapsed) elapsed = n + 1;
                if (enabled) {
                    if (!queue_branch(&out, cpu->cycles + 6, (pc & ~31u) + (uint32_t)(displacement * 2)))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
                }
                continue;
            }
            /* SPRUFE8B Figure C-16: compact word transfer at a positive
             * constant offset from B15. Unlike Dpp, this form does not
             * update B15 and its three-bit register observes header RS. */
            if ((w & 0x8c07) == 0x8c05) {
                ++memory_count;
                bool load = (w & 8) != 0;
                unsigned bank = (w >> 12) & 1;
                unsigned reg = ((w >> 4) & 7) + rs;
                unsigned offset = ((w >> 13) & 3) | (((w >> 7) & 7) << 2);
                uint32_t address = cpu->r[1][15] + offset * 4;
                uint64_t dummy;
                if ((address & 3) ||
                    (load ? !read_scalar(read, opaque, address, 4, &dummy) :
                            (!write || !write(opaque, address,
                                              cpu->r[bank][reg], 4, false))))
                    return stop(cpu, pc, insn->word, "unmapped B15 stack transfer");
                if (load) {
                    if (out.load_count == 40)
                        return stop(cpu, pc, insn->word, "load queue full");
                    for (unsigned j = 0; j < out.load_count; ++j)
                        if (out.loads[j].due == cpu->cycles + 5 &&
                            out.loads[j].bank == bank && out.loads[j].dst == reg)
                            return stop(cpu, pc, insn->word,
                                        "parallel load write conflict");
                    out.loads[out.load_count++] = (CdjC674xLoad){
                        .due = cpu->cycles + 5, .address = address,
                        .bank = bank, .dst = reg, .size = 4
                    };
                } else {
                    if (out.store_count == 24)
                        return stop(cpu, pc, insn->word, "store queue full");
                    out.stores[out.store_count++] = (CdjC674xStore){
                        .due = cpu->cycles + 3, .value = cpu->r[bank][reg],
                        .address = address, .size = 4
                    };
                }
                continue;
            }
            /* SPRUFE8B Figure C-21: compact B15 stack forms. Stores use
             * post-decrement and commit in E3; loads use pre-increment and
             * write their destination in E5. Dpp ignores header RS. */
            if ((w & 0x087f) == 0x0077) {
                ++memory_count;
                unsigned bank = (w >> 12) & 1, reg = (w >> 7) & 15;
                unsigned size = (w & 0x8000) ? 8 : 4;
                bool load = (w & 0x4000) != 0;
                uint32_t old_sp = cpu->r[1][15];
                uint32_t delta = size * (((w >> 13) & 1) + 1);
                uint32_t address = load ? old_sp + delta : old_sp;
                uint64_t data = cpu->r[bank][reg], dummy;
                if ((address & (size - 1)) || (size == 8 && (reg & 1)))
                    return stop(cpu, pc, insn->word,
                                "unaligned stack access or invalid register pair");
                if (size == 8) data |= (uint64_t)cpu->r[bank][reg + 1] << 32;
                if (load ? !read_scalar(read, opaque, address, size, &dummy) :
                           (!write || !write(opaque, address, data, size, false)))
                    return stop(cpu, pc, insn->word, "unmapped stack access");
                if (written[1][15]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                if (load) {
                    if (out.load_count == 40)
                        return stop(cpu, pc, insn->word, "load queue full");
                    for (unsigned j = 0; j < out.load_count; ++j)
                        if (out.loads[j].due == cpu->cycles + 5 &&
                            out.loads[j].bank == bank &&
                            out.loads[j].dst < reg + (size == 8 ? 2 : 1) &&
                            reg < out.loads[j].dst + (out.loads[j].size == 8 ? 2 : 1))
                            return stop(cpu, pc, insn->word, "parallel load write conflict");
                    out.loads[out.load_count++] = (CdjC674xLoad){
                        .due = cpu->cycles + 5, .address = address, .bank = bank,
                        .dst = reg, .size = size
                    };
                } else {
                    if (out.store_count == 24)
                        return stop(cpu, pc, insn->word, "store queue full");
                    out.stores[out.store_count++] = (CdjC674xStore){
                        .due = cpu->cycles + 3, .value = data,
                        .address = address, .size = size
                    };
                }
                out.r[1][15] = load ? address : old_sp - delta;
                written[1][15] = true;
                continue;
            }
            /* Figures G-1/G-2: one operand is a full 5-bit register,
             * the other uses the header-selected register subset. */
            if ((w & 0x0026) == 0x0006 && ((w >> 3) & 3) != 3) {
                unsigned rs = (insn->header & (1u << 19)) ? 16 : 0;
                unsigned ms = ((w >> 10) & 3) << 3;
                side = w & 1;
                cross = side ^ ((w >> 12) & 1);
                dst = (w >> 13) & 7;
                b = (w >> 7) & 7;
                if (w & 0x40) { dst += ms; b += rs; }
                else { dst += rs; b += ms; }
                if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                out.r[side][dst] = cpu->r[cross][b]; written[side][dst] = true;
                continue;
            }
            /* SPRUFE8B Figure D-4: compact .L ADD/SUB. */
            if ((w & 0x040e) != 0 || (insn->header & (1u << 14)))
                return stop(cpu, pc, insn->word, "compact instruction not implemented");
            rs = (insn->header & (1u << 19)) ? 16 : 0;
            side = w & 1;
            dst = ((w >> 4) & 7) + rs;
            a = ((w >> 13) & 7) + rs;
            b = ((w >> 7) & 7) + rs;
            cross = side ^ ((w >> 12) & 1);
            value = (w & 0x0800) ? cpu->r[side][a] - cpu->r[cross][b]
                                  : cpu->r[side][a] + cpu->r[cross][b];
            if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
            out.r[side][dst] = value; written[side][dst] = true;
            continue;
        }
        CdjC674xInterruptGate interrupt_gate = interrupt_gate_decode(insn);
        if (interrupt_gate != CDJ_C674X_INTERRUPT_GATE_NONE) {
            for (unsigned j = 0; j < packet->count; ++j) {
                if (j != i && interrupt_gate_parallel_conflict(
                                  interrupt_gate, &packet->instructions[j]))
                    return stop(cpu, pc, w,
                                "DINT/RINT parallel instruction conflict");
            }
            /* DINT and RINT change interruptibility in their E1 cycle. The
             * execute packet commits as one architectural operation here;
             * no delayed-result entry is appropriate. CSR.PGIE is unchanged.
             * TSR.GIE and CSR.GIE are one physical bit, while TSR.SGIE holds
             * the saved state used by RINT. */
            if (interrupt_gate == CDJ_C674X_INTERRUPT_GATE_DISABLE) {
                out.control[26] = (out.control[26] & ~3u) |
                                  ((cpu->control[1] & 1u) << 1);
                out.control[1] &= ~1u;
            } else {
                uint32_t gie = (cpu->control[26] >> 1) & 1u;
                out.control[26] = (out.control[26] & ~3u) | gie;
                out.control[1] = (out.control[1] & ~1u) | gie;
            }
            continue;
        }
        unsigned creg = w >> 29, z = (w >> 28) & 1;
        bool enabled = true, reg_write = true, control_write = false;
        if (creg == 7 || (!creg && z)) return stop(cpu, pc, insn->word, "reserved predicate");
        if (creg) {
            static const unsigned bank[] = {0,1,1,1,0,0,0};
            static const unsigned index[] = {0,0,1,2,1,2,0};
            enabled = (cpu->r[bank[creg]][index[creg]] != 0) ^ z;
        }
        bool long_offset = (w & 0x0c) == 12;
        if (long_offset || (w & 0x10c) == 0x04 || (w & 0x17c) == 0x134 || (w & 0x17c) == 0x154 ||
                   (w & 0x17c) == 0x124 || (w & 0x17c) == 0x174 ||
                   (w & 0x17c) == 0x164 || (w & 0x17c) == 0x144) {
            /* Scalar memory: address E1, RAM access E3, load destination E5. */
            unsigned op = (w >> 4) & 7;
            bool extended = !long_offset && (w & 0x100) != 0;
            bool pair = extended && (op == 2 || op == 4 || op == 6 || op == 7);
            bool nonaligned = extended && op != 4 && op != 6;
            unsigned size = pair ? 8 : nonaligned ? 4 : op >= 6 ? 4 : (op == 0 || op == 4 || op == 5) ? 2 : 1;
            bool is_store = extended ? (op == 4 || op == 5 || op == 7) : (op == 3 || op == 5 || op == 7);
            unsigned scale = size;
            if (pair && nonaligned) {
                scale = (w & (1u << 23)) ? 8 : 1;
                dst &= ~1u;
            } else if (pair && (dst & 1)) {
                return stop(cpu, pc, insn->word, "invalid doubleword register pair");
            }
            unsigned bank = (w >> 7) & 1, mode = (w >> 9) & 15;
            if (long_offset) {
                /* Figure C-5 / 3.9.3: unsigned scaled 15-bit displacement,
                 * fixed B14/B15 base, no base update, data bank from s. */
                b = 14 + bank; bank = 1; mode = 1;
            }
            reg_write = false;
            if (!(mode & 8) && (mode & 2))
                return stop(cpu, pc, insn->word, "reserved memory addressing mode");
            if (enabled) {
                ++memory_count;
                nonaligned_memory |= nonaligned;
                if (b >= 4 && b <= 7 && cpu->control[0])
                    return stop(cpu, pc, insn->word, "circular memory addressing not implemented");
                uint32_t offset = (long_offset ? (w >> 8) & 32767 :
                                   (mode & 4) ? cpu->r[bank][a] : a) * scale;
                uint32_t base = cpu->r[bank][b];
                uint32_t updated = (mode & 1) ? base + offset : base - offset;
                uint32_t address = ((mode & 10) == 10) ? base : updated;
                uint64_t dummy, store_value = cpu->r[side][dst];
                if (pair) store_value |= (uint64_t)cpu->r[side][dst + 1] << 32;
                if ((!nonaligned && (address & (size - 1))) ||
                    (is_store ? (!write || !write(opaque, address, store_value, size, false))
                              : !read_scalar(read, opaque, address, size, &dummy)))
                    return stop(cpu, pc, insn->word, "unaligned or unmapped scalar memory access");
                if (is_store) {
                    if (out.store_count == 24) return stop(cpu, pc, insn->word, "store queue full");
                    out.stores[out.store_count++] = (CdjC674xStore){
                        .due = cpu->cycles + 3, .address = address,
                        .value = store_value, .size = size
                    };
                } else {
                    if (out.load_count == 40) return stop(cpu, pc, insn->word, "load queue full");
                    for (unsigned j = 0; j < out.load_count; ++j)
                        if (out.loads[j].due == cpu->cycles + 5 &&
                            out.loads[j].bank == side &&
                            out.loads[j].dst < dst + (pair ? 2 : 1) &&
                            dst < out.loads[j].dst + (out.loads[j].size == 8 ? 2 : 1))
                            return stop(cpu, pc, insn->word, "parallel load write conflict");
                    out.loads[out.load_count++] = (CdjC674xLoad){
                        .due = cpu->cycles + 5, .address = address, .bank = side, .dst = dst,
                        .size = size, .sign_extend = !extended && (op == 2 || op == 4)
                    };
                }
                if (mode & 8) {
                    if (written[bank][b]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                    out.r[bank][b] = updated; written[bank][b] = true;
                }
            }
        } else if ((w & 0x7c) == 0x50) {
            /* ADDK .S1/.S2 is an in-place modular add of a signed
             * sixteen-bit constant, with an E1 read and E1 write. */
            value = cpu->r[side][dst] +
                    (uint32_t)sx((w >> 7) & 0xffff, 16);
        } else if ((w & 0x7c) == 0x28) {
            value = sx((w >> 7) & 0xffff, 16);
        } else if ((w & 0x7c) == 0x68) {
            value = (cpu->r[side][dst] & 0xffff) | (((w >> 7) & 0xffff) << 16);
        } else if ((w & 0x7c) == 0x40 && ((w >> 7) & 63) >= 0x30 &&
                   ((w >> 7) & 63) <= 0x3d) {
            /* ADDAB/H/W and SUBAB/H/W: same-bank operands, unsigned
             * five-bit immediate or register offset, scaled by 1/2/4. */
            unsigned op = (w >> 7) & 63;
            if (enabled && b >= 4 && b <= 7 && cpu->control[0])
                return stop(cpu, pc, insn->word, "circular address arithmetic not implemented");
            /* ADDAD uses op 3c/3d; there is no SUBAD (TI page 117). */
            uint32_t offset = (op >= 0x3c ? op & 1 : op & 2) ? a : cpu->r[side][a];
            offset <<= (op - 0x30) / 4;
            value = (op < 0x3c && (op & 1)) ? cpu->r[side][b] - offset : cpu->r[side][b] + offset;
        } else if ((w & 0x7c) == 0x40 && ((w >> 7) & 63) >= 0x10 &&
                   ((w >> 7) & 63) <= 0x13) {
            /* ADD/SUB .D without a cross path, SPRUFE8B pp110,529.
             * The assembler operand order is src2,src1 because the D-unit
             * hardware subtracts the src1 field from the src2 field. */
            unsigned op = (w >> 7) & 63;
            uint32_t left = cpu->r[side][b];
            uint32_t right = (op & 2) ? a : cpu->r[side][a];
            value = (op & 1) ? left - right : left + right;
        } else if ((w & 0xffc) == 0xab0 || (w & 0xffc) == 0xaf0 ||
                   (w & 0xffc) == 0xb30) {
            /* Cross-path ADD/SUB .D, including ADD's signed constant form.
             * Cross SUB uses conventional src1-src2 ordering (SPRUFE8B
             * pp110-111,529-530), unlike non-cross D-unit SUB above. */
            uint32_t left = (w & 0xffc) == 0xaf0 ? (uint32_t)sx(a, 5)
                                                 : cpu->r[side][a];
            uint32_t right = cpu->r[cross][b];
            value = (w & 0xffc) == 0xb30 ? left - right : left + right;
        } else if ((w & 0x7c1ffc) == 0x40) {
            value = sx(a, 5); /* MVK .D */
        } else if ((w & 0x3effc) == 0xa358) {
            value = sx(b, 5); /* MVK .L */
        } else if ((w & 0x1c) == 0x18 &&
                   (((w >> 5) & 0x7f) == 0x20 ||
                    ((w >> 5) & 0x7f) == 0x21 ||
                    ((w >> 5) & 0x7f) == 0x23 ||
                    ((w >> 5) & 0x7f) == 0x24 ||
                    ((w >> 5) & 0x7f) == 0x27 ||
                    ((w >> 5) & 0x7f) == 0x29 ||
                    ((w >> 5) & 0x7f) == 0x2b ||
                    ((w >> 5) & 0x7f) == 0x2f ||
                    ((w >> 5) & 0x7f) == 0x37 ||
                    ((w >> 5) & 0x7f) == 0x3f)) {
            /* ADD/ADDU/SUB/SUBU extended .L forms write a 40-bit long in
             * an even/odd register pair.  The low register holds bits 31:0;
             * only bits 7:0 of the high register are architecturally part
             * of the value (SPRUFE8B ADD/ADDU/SUB/SUBU). */
            unsigned op = (w >> 5) & 0x7f;
            bool pair_source = op == 0x20 || op == 0x21 ||
                               op == 0x24 || op == 0x29;
            reg_write = false;
            /* The immediate-long forms have no cross-path variant: their
             * 40-bit src2 consumes the local .L long-data input.  Cross
             * paths carry only one 32-bit operand (SPRUFE8B 2.3, ADD/SUB
             * opcode maps).  Keep the otherwise format-shaped x=1 words
             * fail-closed instead of silently reading the local pair. */
            bool immediate_long = op == 0x20 || op == 0x24;
            if (immediate_long && (w & (1u << 12)))
                return stop(cpu, pc, insn->word,
                            "cross-path long operand not supported");
            if ((dst & 1) || (pair_source && (b & 1)))
                return stop(cpu, pc, insn->word,
                            "invalid long register pair");
            if (enabled) {
                uint64_t left, right, result;
                switch (op) {
                case 0x20: /* ADD signed 5-bit, signed long. */
                    left = (uint64_t)(int64_t)sx(a, 5);
                    right = register_long40(cpu, side, b);
                    result = left + right;
                    break;
                case 0x21: /* ADD cross signed 32-bit, signed long. */
                    left = (uint64_t)(int64_t)(int32_t)cpu->r[cross][a];
                    right = register_long40(cpu, side, b);
                    result = left + right;
                    break;
                case 0x23: /* ADD signed 32-bit, cross signed 32-bit. */
                    left = (uint64_t)(int64_t)(int32_t)cpu->r[side][a];
                    right = (uint64_t)(int64_t)(int32_t)cpu->r[cross][b];
                    result = left + right;
                    break;
                case 0x24: /* SUB signed 5-bit, signed long. */
                    left = (uint64_t)(int64_t)sx(a, 5);
                    right = register_long40(cpu, side, b);
                    result = left - right;
                    break;
                case 0x27: /* SUB signed 32-bit, cross signed 32-bit. */
                    left = (uint64_t)(int64_t)(int32_t)cpu->r[side][a];
                    right = (uint64_t)(int64_t)(int32_t)cpu->r[cross][b];
                    result = left - right;
                    break;
                case 0x29: /* ADDU cross unsigned 32-bit, unsigned long. */
                    left = cpu->r[cross][a];
                    right = register_long40(cpu, side, b);
                    result = left + right;
                    break;
                case 0x2b: /* ADDU unsigned 32-bit, cross unsigned 32-bit. */
                    left = cpu->r[side][a];
                    right = cpu->r[cross][b];
                    result = left + right;
                    break;
                case 0x2f: /* SUBU unsigned 32-bit, cross unsigned 32-bit. */
                    left = cpu->r[side][a];
                    right = cpu->r[cross][b];
                    result = left - right;
                    break;
                case 0x37: /* SUB cross signed 32-bit, signed 32-bit. */
                    left = (uint64_t)(int64_t)(int32_t)cpu->r[cross][a];
                    right = (uint64_t)(int64_t)(int32_t)cpu->r[side][b];
                    result = left - right;
                    break;
                default:   /* SUBU cross unsigned 32-bit, unsigned 32-bit. */
                    left = cpu->r[cross][a];
                    right = cpu->r[side][b];
                    result = left - right;
                    break;
                }
                result &= UINT64_C(0xffffffffff);
                if (written[side][dst] || written[side][dst + 1])
                    return stop(cpu, pc, insn->word,
                                "parallel register write conflict");
                out.r[side][dst] = (uint32_t)result;
                out.r[side][dst + 1] = (uint32_t)(result >> 32);
                written[side][dst] = written[side][dst + 1] = true;
            }
        } else if (((w & 0x7c) == 0 &&
                    (((w >> 7) & 31) == 0x10 ||
                     ((w >> 7) & 31) == 0x14 ||
                     ((w >> 7) & 31) == 0x16)) ||
                   ((w & 0x83c) == 0x30 &&
                    (((w >> 6) & 31) == 0x18 ||
                     ((w >> 6) & 31) == 0x19))) {
            /* The C674x 32x32 .M family samples both operands in E1 and
             * writes in E4.  MPY32 has scalar (low 32 bits) and signed
             * full-product forms; the SU/U/US variants always write the
             * complete 64-bit product to an even/odd register pair. */
            bool mpy_encoding = (w & 0x7c) == 0;
            unsigned op = mpy_encoding ? (w >> 7) & 31 : (w >> 6) & 31;
            bool pair = !mpy_encoding || op != 0x10;
            reg_write = false;
            if (pair && (dst & 1))
                return stop(cpu, pc, insn->word,
                            "invalid multiply result register pair");
            if (enabled) {
                uint32_t left = cpu->r[side][a];
                uint32_t right = cpu->r[cross][b];
                uint64_t result;
                if (mpy_encoding && (op == 0x10 || op == 0x14)) {
                    result = (uint64_t)((int64_t)(int32_t)left *
                                        (int64_t)(int32_t)right);
                } else if (mpy_encoding) {       /* MPY32SU */
                    result = (uint64_t)((int64_t)(int32_t)left *
                                        (int64_t)(uint64_t)right);
                } else if (op == 0x18) {         /* MPY32U */
                    result = (uint64_t)left * (uint64_t)right;
                } else {                         /* MPY32US */
                    result = (uint64_t)((int64_t)(uint64_t)left *
                                        (int64_t)(int32_t)right);
                }
                uint64_t due = cpu->cycles + 4;
                if (out.load_count == 40)
                    return stop(cpu, pc, insn->word, "delayed-result queue full");
                unsigned count = pair ? 2 : 1;
                for (unsigned j = 0; j < out.load_count; ++j) {
                    unsigned old_count = queued_result_registers(&out.loads[j]);
                    if (out.loads[j].due == due && out.loads[j].bank == side &&
                        out.loads[j].dst < dst + count &&
                        dst < out.loads[j].dst + old_count)
                        return stop(cpu, pc, insn->word,
                                    "parallel delayed-result write conflict");
                }
                out.loads[out.load_count++] = (CdjC674xLoad){
                    .due = due,
                    .value = pair ? result : (uint32_t)result,
                    .bank = side, .dst = dst, .size = pair ? 16 : 0
                };
            }
        } else if ((w & 0x7c) == 0 &&
                   (((w >> 7) & 31) == 0x19 || ((w >> 7) & 31) == 0x18 ||
                    ((w >> 7) & 31) == 0x01 || ((w >> 7) & 31) == 0x09 ||
                    ((w >> 7) & 31) == 0x0f || ((w >> 7) & 31) == 0x0b ||
                    ((w >> 7) & 31) == 0x03 || ((w >> 7) & 31) == 0x07 ||
                    ((w >> 7) & 31) == 0x0d || ((w >> 7) & 31) == 0x05 ||
                    ((w >> 7) & 31) == 0x11 || ((w >> 7) & 31) == 0x17 ||
                    ((w >> 7) & 31) == 0x13 || ((w >> 7) & 31) == 0x15 ||
                    ((w >> 7) & 31) == 0x1b || ((w >> 7) & 31) == 0x1e ||
                    ((w >> 7) & 31) == 0x1f || ((w >> 7) & 31) == 0x1d)) {
            /* Complete non-saturating 16x16 .M scalar family: MPY/H/HL/LH,
             * signed/unsigned permutations, and the two signed-constant
             * forms. Operands are sampled E1 and the scalar result is E2. */
            unsigned op = (w >> 7) & 31;
            uint32_t left_word = cpu->r[side][a];
            uint32_t right_word = cpu->r[cross][b];
            uint32_t result;
            switch (op) {
            case 0x18:
                result = (uint32_t)((int32_t)sx(a, 5) *
                                    (int32_t)(int16_t)right_word); break;
            case 0x1e:
                result = (uint32_t)((int64_t)sx(a, 5) *
                                    (uint16_t)right_word); break;
            case 0x01:
                result = (uint32_t)((int32_t)(int16_t)(left_word >> 16) *
                                    (int32_t)(int16_t)(right_word >> 16)); break;
            case 0x09:
                result = (uint32_t)((int32_t)(int16_t)(left_word >> 16) *
                                    (int32_t)(int16_t)right_word); break;
            case 0x0f:
                result = (uint32_t)((uint32_t)(uint16_t)(left_word >> 16) *
                                    (uint16_t)right_word); break;
            case 0x0b:
                result = (uint32_t)((int64_t)(int16_t)(left_word >> 16) *
                                    (uint16_t)right_word); break;
            case 0x03:
                result = (uint32_t)((int64_t)(int16_t)(left_word >> 16) *
                                    (uint16_t)(right_word >> 16)); break;
            case 0x07:
                result = (uint32_t)((uint32_t)(uint16_t)(left_word >> 16) *
                                    (uint16_t)(right_word >> 16)); break;
            case 0x0d:
                result = (uint32_t)((int64_t)(uint16_t)(left_word >> 16) *
                                    (int16_t)right_word); break;
            case 0x05:
                result = (uint32_t)((int64_t)(uint16_t)(left_word >> 16) *
                                    (int16_t)(right_word >> 16)); break;
            case 0x11:
                result = (uint32_t)((int32_t)(int16_t)left_word *
                                    (int32_t)(int16_t)(right_word >> 16)); break;
            case 0x17:
                result = (uint32_t)((uint32_t)(uint16_t)left_word *
                                    (uint16_t)(right_word >> 16)); break;
            case 0x13:
                result = (uint32_t)((int64_t)(int16_t)left_word *
                                    (uint16_t)(right_word >> 16)); break;
            case 0x15:
                result = (uint32_t)((int64_t)(uint16_t)left_word *
                                    (int16_t)(right_word >> 16)); break;
            case 0x1b:
                result = (uint32_t)((int64_t)(int16_t)left_word *
                                    (uint16_t)right_word); break;
            case 0x1f:
                result = (uint32_t)((uint32_t)(uint16_t)left_word *
                                    (uint16_t)right_word); break;
            case 0x1d:
                result = (uint32_t)((int64_t)(uint16_t)left_word *
                                    (int16_t)right_word); break;
            default:
                result = (uint32_t)((int32_t)(int16_t)left_word *
                                    (int32_t)(int16_t)right_word); break;
            }
            reg_write = false;
            if (enabled) {
                uint64_t due = cpu->cycles + 2;
                if (out.load_count == 40)
                    return stop(cpu, pc, insn->word, "delayed-result queue full");
                for (unsigned j = 0; j < out.load_count; ++j) {
                    unsigned count = queued_result_registers(&out.loads[j]);
                    if (out.loads[j].due == due && out.loads[j].bank == side &&
                        out.loads[j].dst < dst + 1 && dst < out.loads[j].dst + count)
                        return stop(cpu, pc, insn->word,
                                    "parallel delayed-result write conflict");
                }
                out.loads[out.load_count++] = (CdjC674xLoad){
                    .due = due, .value = result, .bank = side, .dst = dst,
                    .size = 0
                };
            }
        } else if ((w & 0x83c) == 0x30 &&
                   (((w >> 6) & 31) == 0x0e ||
                    ((w >> 6) & 31) == 0x10 ||
                    ((w >> 6) & 31) == 0x14 ||
                    ((w >> 6) & 31) == 0x15)) {
            /* MPYIH/MPYHI and MPYIL/MPYLI multiply a signed high/low
             * halfword by a signed 32-bit operand.  Their R variants add
             * 0x4000 and arithmetically shift by 15.  All sample in E1 and
             * write either one register or a full pair in E4. */
            unsigned op = (w >> 6) & 31;
            bool high = op == 0x10 || op == 0x14;
            bool pair = op == 0x14 || op == 0x15;
            reg_write = false;
            if (pair && (dst & 1))
                return stop(cpu, pc, insn->word,
                            "invalid multiply result register pair");
            if (enabled) {
                int16_t half = high ? (int16_t)(cpu->r[side][a] >> 16)
                                    : (int16_t)cpu->r[side][a];
                int64_t product = (int64_t)half * (int32_t)cpu->r[cross][b];
                uint64_t result = pair ? (uint64_t)product :
                    arithmetic_shift_right64((uint64_t)(product + 0x4000), 15);
                uint64_t due = cpu->cycles + 4;
                if (out.load_count == 40)
                    return stop(cpu, pc, insn->word, "delayed-result queue full");
                unsigned count = pair ? 2 : 1;
                for (unsigned j = 0; j < out.load_count; ++j) {
                    unsigned old_count = queued_result_registers(&out.loads[j]);
                    if (out.loads[j].due == due && out.loads[j].bank == side &&
                        out.loads[j].dst < dst + count &&
                        dst < out.loads[j].dst + old_count)
                        return stop(cpu, pc, insn->word,
                                    "parallel delayed-result write conflict");
                }
                out.loads[out.load_count++] = (CdjC674xLoad){
                    .due = due, .value = result, .bank = side, .dst = dst,
                    .size = pair ? 16 : 0
                };
            }
        } else if ((w & 0x3cffc) == 0x958 ||
                   (w & 0x3cffc) == 0x938) {
            /* INTSP/INTSPU read the integer in E1 and write binary32 in E4.
             * The FADCR rounding mode is sampled at issue; INEX becomes
             * sticky with the delayed result only when precision is lost. */
            reg_write = false;
            if (enabled) {
                bool inexact;
                bool signed_source = (w & 0x3cffc) == 0x958;
                unsigned rmode = (cpu->control[18] >> (side ? 25 : 9)) & 3;
                uint32_t result = integer_to_sp(cpu->r[cross][b],
                                                signed_source, rmode, &inexact);
                uint64_t due = cpu->cycles + 4;
                if (out.load_count == 40)
                    return stop(cpu, pc, insn->word, "delayed-result queue full");
                for (unsigned j = 0; j < out.load_count; ++j) {
                    unsigned count = queued_result_registers(&out.loads[j]);
                    if (out.loads[j].due == due && out.loads[j].bank == side &&
                        out.loads[j].dst < dst + 1 && dst < out.loads[j].dst + count)
                        return stop(cpu, pc, insn->word,
                                    "parallel delayed-result write conflict");
                }
                out.loads[out.load_count++] = (CdjC674xLoad){
                    .due = due, .value = result,
                    .address = inexact ? 1u << (side ? 23 : 7) : 0,
                    .bank = side, .dst = dst, .size = 0
                };
            }
        } else if ((w & 0x3cffc) == 0x158 ||
                   (w & 0x3cffc) == 0x178) {
            /* SPINT obeys FADCR; SPTRUNC always rounds toward zero.  Both
             * read in E1 and publish the integer and status in E4. */
            reg_write = false;
            if (enabled) {
                unsigned shift = side ? 16 : 0;
                unsigned rmode = (w & 0x20) ? 1 :
                    (cpu->control[18] >> (shift + 9)) & 3;
                SpResult result = sp_to_integer(cpu->r[cross][b], rmode);
                uint64_t due = cpu->cycles + 4;
                if (out.load_count == 40)
                    return stop(cpu, pc, insn->word, "delayed-result queue full");
                for (unsigned j = 0; j < out.load_count; ++j) {
                    unsigned count = queued_result_registers(&out.loads[j]);
                    if (out.loads[j].due == due && out.loads[j].bank == side &&
                        out.loads[j].dst < dst + 1 && dst < out.loads[j].dst + count)
                        return stop(cpu, pc, insn->word,
                                    "parallel delayed-result write conflict");
                }
                out.loads[out.load_count++] = (CdjC674xLoad){
                    .due = due, .value = result.value,
                    .address = result.status << shift,
                    .bank = side, .dst = dst, .size = 0
                };
            }
        } else if ((w & 0x3effc) == 0xf20) {
            /* ABSSP is the single-cycle .S auxiliary absolute operation.
             * It treats denormals as zero and records its warnings in FAUCR,
             * not FADCR.  No host floating-point operation is involved. */
            uint32_t source = cpu->r[cross][b];
            unsigned exponent = (source >> 23) & 255;
            uint32_t fraction = source & 0x7fffff;
            uint32_t status = 0;
            if (exponent == 255 && fraction) {
                value = 0x7fffffffu;
                status = 1u << 1;
                if (!(fraction & 0x400000)) status |= 1u << 4;
            } else if (!exponent && fraction) {
                value = 0;
                status = (1u << 7) | (1u << 3);
            } else {
                value = source & 0x7fffffffu;
                if (exponent == 255) status = 1u << 5;
            }
            if (enabled && status) {
                if (controls[19])
                    return stop(cpu, pc, insn->word,
                                "parallel FAUCR status write conflict");
                out.control[19] |= status << (side ? 16 : 0);
                controls[19] = true;
            }
        } else if ((w & 0x3c) == 0x20 &&
                   ((w >> 6) & 63) >= 0x38 &&
                   ((w >> 6) & 63) <= 0x3a) {
            /* CMPEQSP/CMPGTSP/CMPLTSP are bit-exact single-cycle .S
             * comparisons.  Signed denormals compare as signed zero; NaNs
             * are unordered.  Warning bits are sticky in FAUCR. */
            unsigned relation = ((w >> 6) & 63) - 0x38;
            SpResult result = compare_sp(cpu->r[side][a],
                                         cpu->r[cross][b], relation);
            value = result.value;
            if (enabled && result.status) {
                if (controls[19])
                    return stop(cpu, pc, insn->word,
                                "parallel FAUCR status write conflict");
                out.control[19] |= result.status << (side ? 16 : 0);
                controls[19] = true;
            }
        } else if ((w & 0xffc) == 0x218 || (w & 0xffc) == 0xe18 ||
                   (w & 0xffc) == 0x238 || (w & 0xffc) == 0x2b8 ||
                   (w & 0xffc) == 0xe38 || (w & 0xffc) == 0xeb8) {
            /* ADDSP/SUBSP use FADCR on both .L and .S.  The .L reverse
             * subtract places the cross source in src1; the .S reverse form
             * computes encoded src2-src1.  Results and warnings appear E4. */
            unsigned encoding = w & 0xffc;
            unsigned operation = encoding == 0x218 || encoding == 0xe18 ? 0 :
                                 encoding == 0xeb8 ? 2 : 1;
            uint32_t source1 = cpu->r[side][a];
            uint32_t source2 = cpu->r[cross][b];
            if (encoding == 0x2b8) {
                source1 = cpu->r[cross][a];
                source2 = cpu->r[side][b];
            }
            unsigned shift = side ? 16 : 0;
            unsigned rmode = (cpu->control[18] >> (shift + 9)) & 3;
            SpResult result = add_sub_sp(source1, source2, operation, rmode);
            reg_write = false;
            if (enabled) {
                uint64_t due = cpu->cycles + 4;
                if (out.load_count == 40)
                    return stop(cpu, pc, insn->word, "delayed-result queue full");
                for (unsigned j = 0; j < out.load_count; ++j) {
                    unsigned count = queued_result_registers(&out.loads[j]);
                    if (out.loads[j].due == due && out.loads[j].bank == side &&
                        out.loads[j].dst < dst + 1 && dst < out.loads[j].dst + count)
                        return stop(cpu, pc, insn->word,
                                    "parallel delayed-result write conflict");
                }
                out.loads[out.load_count++] = (CdjC674xLoad){
                    .due = due, .value = result.value,
                    .address = result.status << shift,
                    .bank = side, .dst = dst, .size = 0
                };
            }
        } else if ((w & 0xffc) == 0xe00) {
            /* MPYSP uses FMCR and the same E1-read/E4-write timing as the
             * other four-cycle scalar operations. */
            reg_write = false;
            if (enabled) {
                unsigned shift = side ? 16 : 0;
                unsigned rmode = (cpu->control[20] >> (shift + 9)) & 3;
                SpResult result = multiply_sp(cpu->r[side][a],
                                              cpu->r[cross][b], rmode);
                uint64_t due = cpu->cycles + 4;
                if (out.load_count == 40)
                    return stop(cpu, pc, insn->word, "delayed-result queue full");
                for (unsigned j = 0; j < out.load_count; ++j) {
                    unsigned count = queued_result_registers(&out.loads[j]);
                    if (out.loads[j].due == due && out.loads[j].bank == side &&
                        out.loads[j].dst < dst + 1 && dst < out.loads[j].dst + count)
                        return stop(cpu, pc, insn->word,
                                    "parallel delayed-result write conflict");
                }
                out.loads[out.load_count++] = (CdjC674xLoad){
                    .due = due, .value = result.value,
                    .address = result.status << shift,
                    .bank = side, .dst = dst, .size = 0,
                    .sign_extend = true
                };
            }
        } else if ((w & 0xffc) == 0x018 || (w & 0xffc) == 0xff0 ||
                   (w & 0xffc) == 0x3d8 || (w & 0xffc) == 0x260 ||
                   (w & 0xffc) == 0x398 || (w & 0xffc) == 0x220 ||
                   (w & 0xffc) == 0x378 || (w & 0xffc) == 0x420 ||
                   (w & 0xffc) == 0xd18 || (w & 0xffc) == 0xd38) {
            /* PACK2/PACKH2/PACKHL2/PACKLH2 (.L/.S) and PACKL4/PACKH4
             * (.L), SPRUFE8B.
             * src1 supplies the upper result half and src2 the lower. */
            uint32_t left = cpu->r[side][a], right = cpu->r[cross][b];
            switch (w & 0xffc) {
            case 0x018: case 0xff0: /* PACK2 */
                value = left << 16 | (right & 0xffff); break;
            case 0x3d8: case 0x260: /* PACKH2 */
                value = (left & 0xffff0000) | (right >> 16); break;
            case 0x398: case 0x220: /* PACKHL2 */
                value = (left & 0xffff0000) | (right & 0xffff); break;
            case 0x378: case 0x420: /* PACKLH2 */
                value = left << 16 | (right >> 16); break;
            case 0xd18:             /* PACKL4 */
                value = (left & 0x00ff0000) << 8 | (left & 0xff) << 16 |
                        (right & 0x00ff0000) >> 8 | (right & 0xff); break;
            default:                /* PACKH4 */
                value = (left & 0xff000000) | (left & 0x0000ff00) << 8 |
                        (right & 0xff000000) >> 16 |
                        (right & 0x0000ff00) >> 8; break;
            }
        } else if ((w & 0xffc) == 0xf98 || (w & 0xffc) == 0xdb0 ||
                   (w & 0xffc) == 0x830) {
            /* ANDN is available on .L/.S/.D with identical single-cycle
             * semantics: src1 AND the bitwise inverse of src2. */
            value = cpu->r[side][a] & ~cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x7a0 || (w & 0xffc) == 0xf58 || (w & 0xffc) == 0x9f0) {
            value = (uint32_t)sx(a, 5) & cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x7e0 || (w & 0xffc) == 0xf78 || (w & 0xffc) == 0x9b0) {
            value = cpu->r[side][a] & cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x58 || (w & 0xffc) == 0x1a0) {
            value = (uint32_t)sx(a, 5) + cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x78 || (w & 0xffc) == 0x1e0) {
            value = cpu->r[side][a] + cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xd8 || (w & 0xffc) == 0x5a0) {
            value = (uint32_t)sx(a, 5) - cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xf8 || (w & 0xffc) == 0x5e0) {
            value = cpu->r[side][a] - cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x2f8) {
            /* Reverse-cross .L SUB encodes its cross source in src1 and
             * its local source in src2. */
            value = cpu->r[cross][a] - cpu->r[side][b];
        } else if ((w & 0xffc) == 0xa58 || (w & 0xffc) == 0xa78 ||
                   (w & 0xffc) == 0x8d8 || (w & 0xffc) == 0x8f8 ||
                   (w & 0xffc) == 0x9d8 || (w & 0xffc) == 0x9f8 ||
                   (w & 0xffc) == 0xad8 || (w & 0xffc) == 0xaf8 ||
                   (w & 0xffc) == 0xbd8 || (w & 0xffc) == 0xbf8) {
            /* CMPEQ/CMPGT/CMPGTU/CMPLT/CMPLTU scalar .L forms.  Even
             * opfields use a signed or unsigned five-bit constant; odd
             * opfields read src1 from the local register file. */
            unsigned encoding = w & 0xffc;
            bool immediate = !(encoding & 0x20);
            uint32_t left = immediate ? a : cpu->r[side][a];
            uint32_t right = cpu->r[cross][b];
            switch (encoding & ~0x20u) {
            case 0xa58: value = (immediate ? (uint32_t)sx(a, 5) : left) == right; break;
            case 0x8d8: value = (immediate ? sx(a, 5) : (int32_t)left) >
                                (int32_t)right; break;
            case 0x9d8: value = left > right; break;
            case 0xad8: value = (immediate ? sx(a, 5) : (int32_t)left) <
                                (int32_t)right; break;
            default:    value = left < right; break;
            }
        } else if ((w & 0xffc) == 0xfd8 || (w & 0xffc) == 0x6a0 || (w & 0xffc) == 0x8f0) {
            value = (uint32_t)sx(a, 5) | cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xff8 || (w & 0xffc) == 0x6e0 || (w & 0xffc) == 0x8b0) {
            value = cpu->r[side][a] | cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xdd8 || (w & 0xffc) == 0x2a0 || (w & 0xffc) == 0xbf0) {
            value = (uint32_t)sx(a, 5) ^ cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xdf8 || (w & 0xffc) == 0x2e0 || (w & 0xffc) == 0xbb0) {
            value = cpu->r[side][a] ^ cpu->r[cross][b];
        } else if ((w & 0x3c) == 8 || (w & 0xafc) == 0xae0) {
            /* SPRUFE8B CLR/SET/EXT/EXTU: constant .S field format or
             * register counts packed as src1[9:5],src1[4:0]. EXT counts
             * describe left/right shifts, NOT low/high bit indices. */
            bool immediate = (w & 0x3c) == 8;
            unsigned op = immediate ? (w >> 6) & 3 : ((w >> 9) & 2) | ((w >> 8) & 1);
            uint32_t fields = cpu->r[side][a];
            if (!immediate && enabled && (fields & ~1023u))
                return stop(cpu, pc, insn->word, "invalid bit-field register counts");
            unsigned left = immediate ? a : (fields >> 5) & 31;
            unsigned right = immediate ? (w >> 8) & 31 : fields & 31;
            uint32_t source = cpu->r[immediate ? side : cross][b];
            if (op < 2) {
                uint32_t shifted = source << left;
                value = shifted >> right;
                if (op == 1 && right && (shifted & 0x80000000u))
                    value |= UINT32_MAX << (32 - right);
            } else {
                uint32_t mask = right < left ? 0 :
                    (UINT32_MAX << left) & (UINT32_MAX >> (31 - right));
                value = op == 2 ? source | mask : source & ~mask;
            }
        } else if ((w & 0xfbc) == 0x9a0 || (w & 0xfbc) == 0xda0 ||
                   (w & 0xfbc) == 0xca0) {
            /* Scalar .S shifts; register counts use only six low bits. */
            unsigned n = (w & 0x40) ? (cpu->r[side][a] & 63) : a;
            uint32_t source = cpu->r[cross][b];
            unsigned op = w & 0xfbc;
            value = shift_32(source, n, op == 0xca0 ? 0 : op == 0x9a0 ? 2 : 1);
        } else if ((w & 0xffe) == 0x3e2 && a == 0) {
            /* MVC control-register to B-register.  The crhi field is zero
             * for every implemented C674x control register ID. */
            if (!control_read_supported(b))
                return stop(cpu, pc, insn->word,
                            "control register read not implemented");
            value = control_read(cpu, b);
        } else if ((w & 0xffe) == 0x3a2 && a == 0) {
            /* MVC register to control-register.  Interrupt-control writes
             * below apply architectural masks rather than acting as storage. */
            if (!control_write_supported(dst))
                return stop(cpu, pc, insn->word,
                            "control register write not implemented");
            control_write = true; reg_write = false; value = cpu->r[cross][b];
        } else if ((w & 0x0ffffffeu) == 0x001800e2u) {
            /* B IRP, SPRUFE8B pp.155-156 and 5.3.4.3. The return branch has
             * five delay slots. ITSR is restored to TSR in E1; ITSR.GIE is
             * the physical CSR.PGIE bit, and PGIE itself remains unchanged. */
            reg_write = false;
            if (enabled) {
                if (controls[1] || controls[26] || controls[27])
                    return stop(cpu, pc, insn->word,
                                "B IRP parallel task-state write conflict");
                if (!queue_branch(&out, cpu->cycles + 6, cpu->control[6]))
                    return stop(cpu, pc, insn->word,
                                "parallel taken branches or branch queue overflow");
                uint32_t restored = (cpu->control[27] & 0x0000c6deu) |
                                    ((cpu->control[1] >> 1) & 1u);
                out.control[26] = restored;
                out.control[1] = (out.control[1] & ~1u) | (restored & 1u);
                controls[1] = controls[26] = controls[27] = true;
            }
        } else if ((w & 0x7c) == 0x10) {
            reg_write = false;
            if (enabled) {
                if (!queue_branch(&out, cpu->cycles + 6, (pc & ~31u) + (uint32_t)(sx((w >> 7) & 0x1fffff, 21) * 4)))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
            }
        } else if ((w & 0x1ffc) == 0x120) {
            /* SPRUFE8B BNOP displacement, pp165-167: NOPs are unconditional.
             * A 32-bit BNOP in a header-based fetch packet uses halfword
             * displacement units; without a header it uses words. */
            unsigned n = (w >> 13) & 7;
            reg_write = false;
            if (n && elapsed > 1) return stop(cpu, pc, insn->word, "multiple multicycle instructions");
            if (n + 1 > elapsed) elapsed = n + 1;
            if (enabled && !queue_branch(&out, cpu->cycles + 6,
                    (pc & ~31u) + (uint32_t)(sx((w >> 16) & 4095, 12) *
                                           (insn->header ? 2 : 4))))
                return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
        } else if ((w & 0x0f830ffe) == 0x00800362) {
            unsigned n = (w >> 13) & 7;
            reg_write = false;
            if (n && elapsed > 1) return stop(cpu, pc, insn->word, "multiple multicycle instructions");
            if (n + 1 > elapsed) elapsed = n + 1;
            if (enabled) {
                if (!queue_branch(&out, cpu->cycles + 6, cpu->r[cross][b]))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
            }
        } else if ((w & 0x0f83effe) == 0x362) {
            reg_write = false;
            if (enabled) {
                if (!queue_branch(&out, cpu->cycles + 6, cpu->r[((w >> 12) & 1) ^ 1][b]))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
            }
        } else if ((w & 0x1ffe) == 0x162) {
            if (!side) return stop(cpu, pc, insn->word, "ADDKPC requires S2");
            value = (pc & ~31u) + (uint32_t)(sx((w >> 16) & 127, 7) * 4);
            unsigned n = 1 + ((w >> 13) & 7);
            if (enabled && n > 1 && elapsed > 1) return stop(cpu, pc, insn->word, "multiple multicycle instructions");
            if (enabled && n > elapsed) elapsed = n;
        } else {
            return stop(cpu, pc, insn->word, "instruction not implemented");
        }
        if (enabled && reg_write) {
            if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
            out.r[side][dst] = value; written[side][dst] = true;
        }
        if (enabled && control_write) {
            if (controls[dst]) return stop(cpu, pc, insn->word, "parallel control write conflict");
            if (dst == 2 || dst == 3) {
                /* ISR/ICR update IFR after one intervening execute packet.
                 * Reuse the ABI-stable delayed-result queue so old checkpoint
                 * layouts retain the in-flight effect. */
                if (out.load_count == 40)
                    return stop(cpu, pc, insn->word, "load queue full");
                out.loads[out.load_count++] = (CdjC674xLoad){
                    .due = cpu->cycles + 2, .address = value & 0xfff0u,
                    .size = dst == 2 ? CDJ_C674X_DELAYED_IFR_SET
                                     : CDJ_C674X_DELAYED_IFR_CLEAR
                };
            } else if (dst == 1) {
                /* PCC/DCC are documented as ignored on C674x. PWRD support is
                 * device-specific; actual C6747 CPU power-down is not yet
                 * modeled, so ignoring that field is an explicit approximation.
                 * SAT is clear-only through MVC; CPU ID, revision and endian
                 * mode are read-only. */
                out.control[1] = (out.control[1] & 0xffff0100u) |
                                 (out.control[1] & value & 0x200u) |
                                 (value & 3u);
                /* CSR.GIE and TSR.GIE are the same physical bit. */
                out.control[26] = (out.control[26] & ~1u) | (value & 1u);
                /* CSR.PGIE and ITSR.GIE are also one physical bit. */
                out.control[27] = (out.control[27] & ~1u) |
                                  ((value >> 1) & 1u);
            } else if (dst == 4) {
                /* Reset remains enabled. NMIE can be set by MVC but not
                 * manually cleared; maskable enables are ordinary RW bits. */
                out.control[4] = (value & 0xfff0u) |
                                 ((out.control[4] | value) & 2u) | 1u;
            } else if (dst == 5) {
                /* HPEINT is derived on read; only the aligned IST base writes. */
                out.control[5] = value & 0xfffffc00u;
            } else if (dst == 27) {
                out.control[27] = value & 0x0000c6dfu;
                out.control[1] = (out.control[1] & ~2u) |
                                 ((value & 1u) << 1);
            } else if (dst == 26) {
                /* Privilege and hardware-owned TSR fields need a later
                 * execution-mode model. Preserve them while allowing the
                 * GIE/SGIE pair used by interrupt-critical firmware. */
                out.control[26] = (out.control[26] & ~3u) | (value & 3u);
                out.control[1] = (out.control[1] & ~1u) | (value & 1u);
            } else {
                out.control[dst] = value;
            }
            controls[dst] = true;
            if (dst == 13 || dst == 14) out.control_ready[dst] = cpu->cycles + 4;
        }
    }
    if (nonaligned_memory && memory_count > 1)
        return stop(cpu, cpu->pc, 0, "parallel access with nonaligned memory instruction");
    /* Same-cycle overlapping RAM reads/writes need bus arbitration that
     * this core does not yet model. Do not choose an invented ordering. */
    for (unsigned j = 0; j < out.load_count; ++j) {
        if (!queued_memory_load(&out.loads[j])) continue;
        for (unsigned k = 0; k < out.store_count; ++k)
            if (out.loads[j].due - 2 == out.stores[k].due &&
                (uint64_t)out.loads[j].address < (uint64_t)out.stores[k].address + out.stores[k].size &&
                (uint64_t)out.stores[k].address < (uint64_t)out.loads[j].address + out.loads[j].size)
                return stop(cpu, cpu->pc, 0, "simultaneous overlapping RAM accesses not implemented");
    }
    /* Reject E5/E1 register collisions before any RAM transaction commits. */
    for (unsigned j = 0; j < out.load_count; ++j) {
        unsigned count = queued_result_registers(&out.loads[j]);
        if (count && out.loads[j].due == cpu->cycles + 1 &&
            (written[out.loads[j].bank][out.loads[j].dst] ||
             (count == 2 && written[out.loads[j].bank][out.loads[j].dst + 1])))
            return stop(cpu, cpu->pc, 0, "delayed-result write conflict");
        if (!out.loads[j].size && out.loads[j].address &&
            out.loads[j].due == cpu->cycles + 1 &&
            controls[out.loads[j].sign_extend ? 20 : 18])
            return stop(cpu, cpu->pc, 0, "delayed FP-status write conflict");
    }
    if (packet->single_cycle && elapsed > 1) {
        out.idle_cycles = elapsed - 1;
        elapsed = 1;
    }
    out.pc = packet->next_pc;
    for (unsigned i = 0; i < elapsed; ++i) {
        ++out.cycles;
        if (out.cycle_tick) out.cycle_tick(out.cycle_opaque);
        for (unsigned j = 0; j < out.store_count;) {
            CdjC674xStore *store = &out.stores[j];
            if (store->due > out.cycles) { ++j; continue; }
            if (!write || !write(opaque, store->address, store->value, store->size, true))
                return stop(cpu, cpu->pc, 0, "RAM store callback broke commit guarantee");
            memmove(store, store + 1, (--out.store_count - j) * sizeof(*store));
        }
        for (unsigned j = 0; j < out.load_count;) {
            CdjC674xLoad *load = &out.loads[j];
            if (queued_memory_load(load) && load->due == out.cycles + 2) {
                uint64_t data;
                if (!read_scalar(read, opaque, load->address, load->size, &data))
                    return stop(cpu, cpu->pc, 0, "RAM load mapping changed during execution");
                if (load->sign_extend) data = sx(data, load->size * 8);
                load->value = data;
            }
            if (load->due > out.cycles) { ++j; continue; }
            if (load->size == CDJ_C674X_DELAYED_IFR_SET ||
                load->size == CDJ_C674X_DELAYED_IFR_CLEAR) {
                uint32_t sets = 0, clears = 0;
                for (unsigned k = j; k < out.load_count; ++k) {
                    CdjC674xLoad *effect = &out.loads[k];
                    if (effect->due > out.cycles) continue;
                    if (effect->size == CDJ_C674X_DELAYED_IFR_SET)
                        sets |= effect->address;
                    else if (effect->size == CDJ_C674X_DELAYED_IFR_CLEAR)
                        clears |= effect->address;
                }
                /* Incoming/set requests win over a simultaneous clear. */
                out.control[2] = ((out.control[2] & ~clears) | sets) & 0xfff2u;
                for (unsigned k = 0; k < out.load_count;) {
                    CdjC674xLoad *effect = &out.loads[k];
                    if (effect->due <= out.cycles &&
                        (effect->size == CDJ_C674X_DELAYED_IFR_SET ||
                         effect->size == CDJ_C674X_DELAYED_IFR_CLEAR)) {
                        memmove(effect, effect + 1,
                                (--out.load_count - k) * sizeof(*effect));
                    } else ++k;
                }
                j = 0;
                continue;
            }
            out.r[load->bank][load->dst] = load->value;
            if (queued_result_registers(load) == 2)
                out.r[load->bank][load->dst + 1] = load->value >> 32;
            if (!load->size)
                out.control[load->sign_extend ? 20 : 18] |= load->address;
            memmove(load, load + 1, (--out.load_count - j) * sizeof(*load));
        }
        if (out.branch_due && out.cycles == out.branch_due) {
            out.pc = out.branch_target;
            /* SPRUFE8B 7.14: a taken branch idles an active loop buffer.
             * Do not clear the special idle SPLX state restored by B IRP;
             * section 7.7.3.2 requires it until the return SPLOOP starts. */
            if (out.loop_active) loop_set_active(&out, false);
            if (out.branch_count) {
                out.branch_due = out.branch_queue[0].due;
                out.branch_target = out.branch_queue[0].target;
                memmove(out.branch_queue, out.branch_queue + 1,
                        --out.branch_count * sizeof(out.branch_queue[0]));
            } else out.branch_due = 0;
            out.idle_cycles = 0;
            break;
        }
    }
    ++out.packets;
    *cpu = out;
    return true;
}

static bool loop_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque)
{
    CdjC674x out = *cpu;
    /* Also reconciles legacy checkpoints written before SPLX tracking. */
    out.control[26] |= CDJ_C674X_TSR_SPLX;
    CdjC674xPacket combined = {.next_pc = cpu->pc, .single_cycle = true};
    CdjC674xPacket direct = {0};
    LoopMask masking = {.cpu = &out};
    bool loading = !out.loop.sealed;
    bool post = out.loop.sealed && out.loop.cycle >= out.loop.post_cycle;
    if (loading && !out.loop_wait) {
        CdjC674xPacket source;
        if (!cdj_c674x_fetch(&out, read, opaque, &source))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        /* Section 7.7.3.3 indexes storage by LBC (cycle modulo II), not
         * the number of source fetches. NOP/setup packets can exceed 14.
         * The 48-cycle, 112-tag and simultaneous-issue limits still apply. */
        ++out.loop_packets;
        bool finish = false;
        unsigned delay = 0, count = 0;
        uint32_t tags[8];
        bool has_mask = spmask_decode(&source.instructions[0], &masking.mask);
        if ((out.loop_pred_history & CDJ_C674X_LOOP_RETURNING) && has_mask)
            return stop(cpu, source.instructions[0].pc,
                        source.instructions[0].word,
                        "SPLOOP interrupt-return SPMASK not implemented");
        for (unsigned i = 0; i < source.count; ++i) {
            CdjC674xInstruction insn = source.instructions[i];
            uint32_t w = insn.word;
            unsigned mask;
            if (spmask_decode(&insn, &mask)) {
                if (i) return stop(cpu, insn.pc, w, "SPMASK must start packet");
                continue;
            }
            bool full_kernel = !insn.compact &&
                (w & 0xf03ffffc) == 0x34000;
            bool compact_kernel = insn.compact &&
                (w & 0x3c7e) == 0x1c66;
            if (full_kernel || compact_kernel) {
                if (i != 0) return stop(cpu, insn.pc, w, "SPKERNEL must start packet");
                /* Figure H-7 scatters the same six-bit combined
                 * stage/cycle field across bits 0, 9:7 and 15:14. */
                unsigned field = compact_kernel ?
                    (w & 1) | (((w >> 7) & 7) << 1) |
                    (((w >> 14) & 3) << 4) : (w >> 22) & 63;
                unsigned cbits = 0, stage = 0;
                while ((1u << cbits) < out.loop.ii) ++cbits;
                for (unsigned j = 5; j >= cbits && j < 6; --j)
                    stage |= ((field >> j) & 1) << (5 - j);
                unsigned cycle = field & ((1u << cbits) - 1);
                if (cycle >= out.loop.ii) return stop(cpu, insn.pc, w, "invalid SPKERNEL cycle");
                delay = stage * out.loop.ii + cycle;
                if (out.loop.predicate_loop && delay)
                    return stop(cpu, insn.pc, w, "SPLOOPW requires zero SPKERNEL fetch delay");
                finish = true;
                continue;
            }
            if (finish && interrupt_gate_decode(&insn) !=
                              CDJ_C674X_INTERRUPT_GATE_NONE)
                return stop(cpu, insn.pc, w,
                            "DINT/RINT cannot share SPKERNEL packet");
            unsigned n = nop_cycles(&insn);
            if (n) {
                if (n > 9 || (n > 1 && (finish || out.loop_wait)))
                    return stop(cpu, insn.pc, w, "invalid loop NOP packet");
                if (n > 1) out.loop_wait = n - 1;
                continue;
            }
            /* SPRUFE8B 3.10 and 7.7.3.3: PROT expands the program stream
             * with four empty loading cycles. Buffered instructions continue
             * issuing during those cycles, just as for explicit NOP 4.
             * Do not reinsert fetch delays when the load is replayed. This
             * expansion applies even when predicated false or SPMASKed. */
            if (protected_load(&insn)) {
                if (finish || out.loop_wait)
                    return stop(cpu, insn.pc, w, "invalid protected loop load packet");
                out.loop_wait = 4;
                insn.header &= ~(1u << 20);
            }
            if ((!insn.compact && ((w & 0x1ffe) == 0x162 || (w & 0x7c) == 0x10 ||
                                  (w & 0xffe) == 0x362 || (w & 0x1ffc) == 0x120)) ||
                compact_branch(&insn))
                return stop(cpu, insn.pc, w, "loop body control instruction not implemented");
            if (has_mask && masking.mask) {
                unsigned unit = instruction_unit(&insn);
                if (!unit) return stop(cpu, insn.pc, w, "SPMASK unit not implemented");
                if (unit & masking.mask) {
                    direct.instructions[direct.count++] = insn;
                    continue;
                }
            }
            /* Section 7.18: MVC may execute from memory when masked but
             * cannot enter the loop buffer. */
            if ((!insn.compact && (w & 0xffe) == 0x3a2) ||
                (insn.compact && (w & 0xfc7f) == 0xd86f))
                return stop(cpu, insn.pc, w, "unmasked loop MVC not permitted");
            if (out.loop_tags == 112) return stop(cpu, insn.pc, w, "loop instruction capacity exceeded");
            tags[count++] = out.loop_tags;
            out.loop_instructions[out.loop_tags++] = insn;
        }
        if (!cdj_c674x_loop_load(&out.loop, tags, count, finish, delay))
            return stop(cpu, cpu->pc, 0, "invalid loop buffer load");
        combined.next_pc = source.next_pc;
    } else if (loading) {
        --out.loop_wait;
        if (!cdj_c674x_loop_load(&out.loop, NULL, 0, false, 0))
            return stop(cpu, cpu->pc, 0, "loop dynamic length exceeded");
    }
    if (post && !out.idle_cycles) {
        CdjC674xPacket source;
        if (!cdj_c674x_fetch(&out, read, opaque, &source))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        bool has_mask = spmask_decode(&source.instructions[0], &masking.mask);
        for (unsigned i = has_mask ? 1 : 0; i < source.count; ++i) {
            unsigned mask;
            if (spmask_decode(&source.instructions[i], &mask))
                return stop(cpu, source.instructions[i].pc, source.instructions[i].word,
                            "SPMASK must start packet");
            direct.instructions[direct.count++] = source.instructions[i];
        }
        combined.next_pc = source.next_pc;
    } else if (out.idle_cycles) {
        --out.idle_cycles;
    }
    uint32_t tags[8]; unsigned count; bool scheduler_post, drained;
    if (!cdj_c674x_loop_issue_filtered(&out.loop, tags, &count, &scheduler_post,
                                     &drained, loop_allow, &masking))
        return stop(cpu, cpu->pc, 0, "loop issue capacity exceeded");
    if (masking.unknown) return stop(cpu, cpu->pc, 0, "buffered SPMASK unit not implemented");
    if (count + direct.count > 8) return stop(cpu, cpu->pc, 0, "loop/direct packet capacity exceeded");
    for (unsigned i = 0; i < count; ++i)
        combined.instructions[combined.count++] = out.loop_instructions[tags[i]];
    for (unsigned i = 0; i < direct.count; ++i)
        combined.instructions[combined.count++] = direct.instructions[i];
    /* SPRUFE8B 7.10: sample the selected condition each cycle. At the end
     * of a stage, use its value three cycles earlier; the first three loop
     * cycles cannot terminate. No ILC/RILC access and no epilog. */
    bool end_while = out.loop.predicate_loop && out.loop.cycle >= 4 &&
        out.loop.cycle % out.loop.ii == 0 && !(out.loop_pred_history & 4);
    if (end_while && !out.loop.sealed)
        return stop(cpu, cpu->pc, 0, "SPLOOPW termination during loading not implemented");
    bool condition = (cpu->r[out.loop_pred_bank][out.loop_pred_reg] != 0) ^ out.loop_pred_invert;
    out.loop_pred_history =
        (out.loop_pred_history & CDJ_C674X_LOOP_RETURNING) |
        ((((out.loop_pred_history & 7) << 1) | condition) & 7);
    if (!cdj_c674x_execute(&out, &combined, read, write, opaque))
        return stop(cpu, out.fault_pc, out.fault_word, out.fault);
    uint64_t launched = 1 + out.loop.cycle / out.loop.ii;
    if (out.loop.delayed_count) {
        /* SPLOOPD forces termination false and suppresses ILC decrement
         * during the first three loop cycles.  At later stage boundaries,
         * test ILC before conditionally decrementing it (7.9.2/7.9.3). */
        if (out.loop.cycle >= 4 && out.loop.cycle % out.loop.ii == 0 &&
            out.control[13])
            --out.control[13];
    } else if (!out.loop.predicate_loop)
        out.control[13] = launched < out.loop.iterations ? out.loop.iterations - launched : 0;
    if (end_while) loop_set_active(&out, false);
    if (drained && scheduler_post) loop_set_active(&out, false);
    *cpu = out;
    return true;
}

bool cdj_c674x_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque)
{
    if (cpu->fault) return false;
    if (cpu->loop_active) return loop_step(cpu, read, write, opaque);
    if (cpu->idle_cycles) {
        CdjC674x out = *cpu;
        CdjC674xPacket idle = {.next_pc = cpu->pc, .single_cycle = true};
        --out.idle_cycles;
        if (!cdj_c674x_execute(&out, &idle, read, write, opaque))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        *cpu = out;
        return true;
    }
    CdjC674xPacket packet;
    if (!cdj_c674x_fetch(cpu, read, opaque, &packet)) return false;
    CdjC674xInstruction first = packet.instructions[0];
    bool compact_sploop = first.compact && (first.word & 0xbc7f) == 0x0c66;
    bool compact_sploopd = first.compact &&
        (first.word & 0xbc7f) == 0x0c67;
    bool compact_sploopd_reload = first.compact &&
        (first.word & 0xbc7e) == 0x8c66;
    bool while_loop = !first.compact && (first.word & 0x007ffffe) == 0x3e000;
    bool full_sploop = !first.compact &&
        (first.word & 0x007ffffc) == 0x38000;
    bool full_sploopd = !first.compact &&
        (first.word & 0x007ffffc) == 0x3a000;
    if (compact_sploopd_reload)
        return stop(cpu, cpu->pc, first.word,
                    "SPLOOPD reload not implemented");
    if (full_sploop || compact_sploop || full_sploopd ||
        compact_sploopd || while_loop) {
        uint32_t w = first.word;
        /* There is one currently implemented architectural idle/SPLX=1
         * state: B IRP has
         * restored a task interrupted in SPLOOP.  This partial interpreter
         * uses that hardware-only bit as its return-window proxy because it
         * has no separate pipeline provenance in the checkpoint ABI.  MVC
         * cannot write TSR.SPLX, and all normal loop-idle paths clear it.
         * Full retained-buffer reload is intentionally not claimed here. */
        bool returning = (cpu->control[26] & CDJ_C674X_TSR_SPLX) != 0;
        bool delayed_loop = (full_sploopd || compact_sploopd) && !returning;
        unsigned pred = first.compact ? 0 : w >> 29;
        if (returning && while_loop)
            return stop(cpu, cpu->pc, w,
                        "SPLOOPW interrupt return not implemented");
        if (while_loop ? (!pred || pred == 7) : (!first.compact && (w >> 28) != 0))
            return stop(cpu, cpu->pc, w, "unsupported loop predicate");
        if (!while_loop && !delayed_loop &&
            cpu->cycles < cpu->control_ready[13])
            return stop(cpu, cpu->pc, w, "ILC not yet available");
        CdjC674x out = *cpu;
        /* SPRUFE8B Figure H-5 scatters compact ii-1 across bits 9:7 and
         * bit 14. GNU binutils format nfu_uspl independently agrees. */
        unsigned ii = first.compact ? (((w >> 7) & 7) | ((w >> 11) & 8)) + 1
                                    : ((w >> 23) & 31) + 1;
        if (!cdj_c674x_loop_init(&out.loop, ii, cpu->control[13]))
            return stop(cpu, cpu->pc, w, "invalid SPLOOP interval");
        out.loop.predicate_loop = while_loop;
        out.loop.delayed_count = delayed_loop;
        out.loop_pred_history = returning ? CDJ_C674X_LOOP_RETURNING : 0;
        if (while_loop) {
            static const unsigned banks[] = {0,1,1,1,0,0,0};
            static const unsigned regs[] = {0,0,1,2,1,2,0};
            out.loop_pred_bank = banks[pred]; out.loop_pred_reg = regs[pred];
            out.loop_pred_invert = (w >> 28) & 1;
            out.loop_pred_history = 7;
        }
        /* Loop setup cannot share a packet with multicycle operations. */
        for (unsigned j = 1; j < packet.count; ++j) {
            CdjC674xInstruction other = packet.instructions[j];
            uint32_t v = other.word;
            unsigned mask;
            if (spmask_decode(&other, &mask))
                return stop(cpu, other.pc, v, "SPMASK cannot share loop setup packet");
            if (protected_load(&other) ||
                (nop_cycles(&other) > 1) ||
                interrupt_gate_decode(&other) !=
                    CDJ_C674X_INTERRUPT_GATE_NONE ||
                (!other.compact && ((v & 0xffe) == 0x362 || (v & 0x7c) == 0x10 ||
                                   (v & 0x1ffc) == 0x120)) ||
                compact_branch(&other))
                return stop(cpu, other.pc, v, "multicycle loop setup packet not implemented");
        }
        memmove(packet.instructions, packet.instructions + 1, (--packet.count) * sizeof(packet.instructions[0]));
        /* Section 7.13.2: operations parallel with the return SPLOOP(D/W)
         * are NOPs.  Executing an empty packet still advances its one cycle
         * and keeps all older delayed effects architectural. */
        if (returning) packet.count = 0;
        if (!cdj_c674x_execute(&out, &packet, read, write, opaque))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        if (delayed_loop) {
            uint32_t minimum = (4 + ii - 1) / ii;
            if (out.control[13] > UINT32_MAX - minimum)
                return stop(cpu, cpu->pc, w, "SPLOOPD iteration count overflow");
            out.loop.iterations = out.control[13] + minimum;
        }
        bool active = !(cpu->branch_due && out.cycles >= cpu->branch_due);
        if (active) loop_set_active(&out, true);
        else {
            out.loop_active = false;
            if (!returning) out.control[26] &= ~CDJ_C674X_TSR_SPLX;
        }
        out.loop_wait = out.loop_tags = out.loop_packets = 0;
        if (!while_loop && !delayed_loop && out.control[13]) --out.control[13];
        *cpu = out;
        return true;
    }
    return cdj_c674x_execute(cpu, &packet, read, write, opaque);
}
