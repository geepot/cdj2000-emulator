/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_TIMER_H
#define CDJ_C6747_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#define CDJ_C6747_TIMER_COUNT 2u
#define CDJ_C6747_TIMER0_BASE 0x01c20000u
#define CDJ_C6747_TIMER1_BASE 0x01c21000u

/* SPRUH91D chapter 28 register state. Internal/external clock progression,
 * output pins, watchdog reset, DMA events and INTC delivery are deliberately
 * outside this register-level batch. */
typedef struct {
    uint32_t emumgt, gpintgpen, gpdatgpdir;
    uint32_t tim12, tim34, tim34_shadow;
    uint32_t prd12, prd34, tcr, tgcr, wdtcr;
    uint32_t rel12, rel34, cap12, cap34, intctlstat;
    uint32_t compare[8];
    uint8_t tim34_shadow_valid;
} CdjC6747Timer;

void cdj_c6747_timers_reset(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT]);
bool cdj_c6747_timers_read(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT],
                           uint32_t address, uint32_t *value);
/* Check phase is side-effect free. Reserved offsets, writes to REVID and
 * non-word accesses remain unmapped so validation runs fail closed. */
bool cdj_c6747_timers_write(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT],
                            uint32_t address, uint64_t value, unsigned size,
                            bool commit);

#endif
