/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Packed .M-unit dot products, SPRUFE8B July 2010.  Semantics only; the
 * decoder, the register-pair rule and the E4 delayed-result queueing stay in
 * cdj_c674x.c.  Each rule below quotes the printed page it comes from. */
#include "cdj_c674x_dotp.h"

static int32_t s16(uint32_t word, unsigned half)
{
    return (int16_t)(word >> (16u * half));
}

static int32_t u16(uint32_t word, unsigned half)
{
    return (int32_t)(uint32_t)(uint16_t)(word >> (16u * half));
}

static int32_t s8(uint32_t word, unsigned byte)
{
    return (int8_t)(word >> (8u * byte));
}

static int32_t u8(uint32_t word, unsigned byte)
{
    return (int32_t)(uint32_t)(uint8_t)(word >> (8u * byte));
}

/* ">> 16" on a signed intermediate, rounding toward negative infinity.  C
 * leaves the right shift of a negative signed value implementation-defined, so
 * the sign fill is written out.  The caller has already established that the
 * intermediate fits in 32 bits. */
static int32_t shift_right_16(int32_t value)
{
    uint32_t shifted = (uint32_t)value >> 16;
    if (value < 0) shifted |= 0xffff0000u;
    return (int32_t)shifted;
}

/* The rounded forms.  Printed page 240 (DOTPNRSU2) and printed page 244
 * (DOTPRSU2) give the same three steps: multiply src1's signed halfwords by
 * src2's unsigned halfwords, combine the two products, add 8000h, then signed
 * shift right by 16 and sign extend into dst.
 *
 * Printed page 244 also states the limit: "Overflow can be avoided if the sum
 * of the two products plus the rounding term is less than or equal to 2^31 - 1
 * for a positive sum and greater than or equal to -2^31 for a negative sum."
 * Outside that range the manual declares the result undefined and prints
 * "xxxx xxxxh" in its own worked examples (DOTPNRSU2 example 3, printed page
 * 241; DOTPRSU2 example 3, printed page 246) - and it contradicts itself in
 * the same breath, because both entries also claim "The intermediate results
 * ... are maintained to a 33-bit precision, ensuring that no overflow may
 * occur".  Nothing in SPRUFE8B says what those words actually produce, so the
 * caller is told to halt rather than invent one. */
static CdjC674xDotpResult rounded(int64_t products)
{
    CdjC674xDotpResult result = {0, false, true};
    int64_t intermediate = products + 0x8000;

    if (intermediate > INT32_MAX || intermediate < INT32_MIN) {
        result.undefined = true;
        return result;
    }
    result.value = (uint64_t)(int64_t)shift_right_16((int32_t)intermediate);
    return result;
}

CdjC674xDotpResult cdj_c674x_dotp(unsigned opfield, uint32_t src1,
                                  uint32_t src2)
{
    CdjC674xDotpResult result = {0, false, true};

    switch (opfield) {
    case CDJ_C674X_DOTP2:
    case CDJ_C674X_DOTP2L:
        /* Printed page 236: "(lsb16(src1) x lsb16(src2)) + (msb16(src1) x
         * msb16(src2)) -> dst", and printed page 235 for the pair form: "the
         * upper word of the register pair always contains either all 0s or all
         * 1s, depending on whether the result is positive or negative".
         *
         * The one documented overflow, printed page 236: "In the overflow
         * case, where all four halfwords in src1 and src2 are 8000h, the value
         * 8000 0000h is written into the 32-bit dst and 0000 0000 8000 0000h
         * is written into the 64-bit dst."  That is 2^30 + 2^30 = 2^31 held as
         * a positive 64-bit sum, which is what this expression produces; no
         * special case is needed and none is written. */
        result.value = (uint64_t)((int64_t)s16(src1, 0) * s16(src2, 0) +
                                  (int64_t)s16(src1, 1) * s16(src2, 1));
        break;
    case CDJ_C674X_DOTPN2:
        /* Printed page 238: "(msb16(src1) x msb16(src2)) - (lsb16(src1) x
         * lsb16(src2)) -> dst", and "unlike DOTP2, no overflow case exists for
         * this instruction". */
        result.value = (uint64_t)((int64_t)s16(src1, 1) * s16(src2, 1) -
                                  (int64_t)s16(src1, 0) * s16(src2, 0));
        break;
    case CDJ_C674X_DOTPNRSU2:
        /* Printed page 240: "int33 = (smsb16(src1) x umsb16(src2)) -
         * (slsb16(src1) x ulsb16(src2)) + 8000h; int33 >> 16 -> dst". */
        result = rounded((int64_t)s16(src1, 1) * u16(src2, 1) -
                         (int64_t)s16(src1, 0) * u16(src2, 0));
        break;
    case CDJ_C674X_DOTPRSU2:
        /* Printed page 245: "int33 = (smsb16(src1) x umsb16(src2)) +
         * (slsb16(src1) x ulsb16(src2)) + 8000h; int33 >> 16 -> dst". */
        result = rounded((int64_t)s16(src1, 1) * u16(src2, 1) +
                         (int64_t)s16(src1, 0) * u16(src2, 0));
        break;
    case CDJ_C674X_DOTPSU4:
        /* Printed page 249: the four signed-by-unsigned byte products summed
         * and "written as a signed 32-bit result to dst".  The largest
         * magnitude reachable is 4 x 128 x 255 = 130,560, so the sum cannot
         * leave 32 bits and the manual states no overflow case. */
        result.value = (uint64_t)(int64_t)(int32_t)
            (s8(src1, 0) * u8(src2, 0) + s8(src1, 1) * u8(src2, 1) +
             s8(src1, 2) * u8(src2, 2) + s8(src1, 3) * u8(src2, 3));
        break;
    case CDJ_C674X_DOTPU4:
        /* Printed page 252: the four unsigned byte products summed and written
         * as an unsigned 32-bit result.  4 x 255 x 255 = 260,100. */
        result.value = (uint32_t)
            (u8(src1, 0) * u8(src2, 0) + u8(src1, 1) * u8(src2, 1) +
             u8(src1, 2) * u8(src2, 2) + u8(src1, 3) * u8(src2, 3));
        break;
    default:
        result.valid = false;
        break;
    }
    return result;
}
