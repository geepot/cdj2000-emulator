/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_LOOP_H
#define CDJ_C674X_LOOP_H
#include <stdbool.h>
#include <stdint.h>
/* Unconditional SPLOOP scheduling model, TI SPRUFE8B chapter 7.
 * Tags identify decoded instructions; the CPU owns their contents. SPLOOP's
 * own packet is outside this timeline. Masks, reload and interrupts are not
 * implemented. The caller supplies each program-memory packet at its original
 * cycle while loading; NOP cycles have no tags but still advance time. */
typedef struct {
    uint32_t tags[48][8];
    unsigned count[48], ii, length;
    uint32_t iterations;
    uint64_t cycle, post_cycle, end_cycle;
    bool sealed, predicate_loop;
} CdjC674xLoop;
bool cdj_c674x_loop_init(CdjC674xLoop *, unsigned ii, uint32_t iterations);
/* Add the current cycle's instructions; finish marks SPKERNEL. delay is the
 * decoded fstg*ii+fcyc. Control marker instructions are not included as tags. */
bool cdj_c674x_loop_load(CdjC674xLoop *, const uint32_t *, unsigned count,
                       bool finish, unsigned delay);
/* Return this cycle's simultaneous instructions and post-loop fetch permission.
 * No mutation on failure (e.g. more than eight simultaneous instructions). */
bool cdj_c674x_loop_issue(CdjC674xLoop *, uint32_t tags[8], unsigned *count,
                        bool *post_fetch, bool *drained);
#endif
