/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_PSC_H
#define CDJ_C6747_PSC_H
#include <stdbool.h>
#include <stdint.h>
/* Register-level PSC0/1, SPRUH91D chapter 8. Eight caller ticks are a
 * deterministic approximation, NOT measured hardware timing. The caller
 * ticks once per DSP step; physical clock/reset wiring is not implemented. */
typedef struct {
    uint32_t control[2][32], status[2][32], target[2][32];
    unsigned remaining[2][2];
} CdjC6747Psc;
void cdj_c6747_psc_reset(CdjC6747Psc *);
void cdj_c6747_psc_tick(CdjC6747Psc *);
bool cdj_c6747_psc_read(const CdjC6747Psc *, uint32_t, uint32_t *);
bool cdj_c6747_psc_write(CdjC6747Psc *, uint32_t, uint64_t, unsigned, bool);
#endif
