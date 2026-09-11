/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_DOTP_H
#define CDJ_C674X_DOTP_H
#include <stdbool.h>
#include <stdint.h>
/* Packed dot products on the .M unit, SPRUFE8B (July 2010).  Every one of
 * these is the Figure E-1 compound .M format - bit 11 is 0, the opfield is
 * bits 10-6 and bits 5-2 are 1100 - and every one is a four-cycle instruction
 * with three delay slots whose dst is written in E4.
 *
 *   opfield  instruction   printed page
 *   00010    DOTPSU4       249    (DOTPUS4, 251, is the same encoding: its
 *                                 syntax lists src2 first and "the assembler
 *                                 uses the DOTPSU4 (.unit) src1, src2, dst
 *                                 instruction to perform this task")
 *   00110    DOTPU4        252
 *   00111    DOTPNRSU2     240    (DOTPNRUS2, 242, same encoding)
 *   01001    DOTPN2        238
 *   01011    DOTP2         235    dst_o:dst_e form, operand type sllong
 *   01100    DOTP2         235    32-bit dst form, operand type int
 *   01101    DOTPRSU2      244    (DOTPRUS2, 247, same encoding)
 *
 * Pure value-in/value-out: nothing here includes or touches CdjC674x, so the
 * execute loop's transactional copy is unreachable from this file. */
typedef enum {
    CDJ_C674X_DOTPSU4   = 0x02,
    CDJ_C674X_DOTPU4    = 0x06,
    CDJ_C674X_DOTPNRSU2 = 0x07,
    CDJ_C674X_DOTPN2    = 0x09,
    CDJ_C674X_DOTP2L    = 0x0b,
    CDJ_C674X_DOTP2     = 0x0c,
    CDJ_C674X_DOTPRSU2  = 0x0d,
} CdjC674xDotpOp;

typedef struct {
    /* The full 64-bit result.  DOTP2's dst_o:dst_e form writes all of it; the
     * 32-bit dst forms write the low word, which printed page 236 says is the
     * same value ("The 32-bit result version returns the same results that the
     * 64-bit result version does in the lower 32 bits"). */
    uint64_t value;
    /* DOTPRSU2 / DOTPNRSU2 only: the intermediate sum left the range in which
     * the manual defines a result.  The caller must fail closed. */
    bool undefined;
    /* opfield is one of CdjC674xDotpOp. */
    bool valid;
} CdjC674xDotpResult;

CdjC674xDotpResult cdj_c674x_dotp(unsigned opfield, uint32_t src1,
                                  uint32_t src2);
#endif
