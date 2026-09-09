/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>

#include "cdj_c6747_timer.h"

int main(void)
{
    CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT];
    uint32_t value;
    cdj_c6747_timers_reset(timers);
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE, &value) &&
           value == 0x4472020c);
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER1_BASE, &value) &&
           value == 0x4472020c);

    /* Check phase is atomic; reserved bits and TCR.TSTAT read zero. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  UINT32_MAX, 4, false));
    assert(timers[0].tgcr == 0);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  UINT32_MAX, 4, true));
    assert(timers[0].tgcr == 0xff1f);
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x20,
                                  UINT32_MAX, 4, true));
    assert(timers[0].tcr == 0x04c03ffe);

    /* The firmware's Timer64P0 setup sequence selects unchained/Plus mode,
     * releases both halves and enables period interrupt 12. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0x17, 4, true));
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x44,
                                  1, 4, true));
    assert(timers[0].tgcr == 0x17 && timers[0].intctlstat == 1);

    /* Read-reset is Plus-only and valid only in unchained mode. */
    timers[0].tim12 = 123;
    timers[0].tcr = 1u << 10;
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x10,
                                 &value) && value == 123 && timers[0].tim12 == 0);

    /* A 64-bit TIM12/TIM34 read is coherent through the upper shadow. */
    timers[0].tgcr = 3;
    timers[0].tim12 = 0xffffffff;
    timers[0].tim34 = 7;
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x10, &value) &&
           value == UINT32_MAX);
    timers[0].tim34 = 8;
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x14, &value) &&
           value == 7);
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x14, &value) &&
           value == 8);

    /* Clearing reset bits clears the corresponding counters. */
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24,
                                  0, 4, true));
    assert(!timers[0].tim12 && !timers[0].tim34);

    /* INTCTLSTAT status is W1C while enable bits take the written value. */
    timers[1].intctlstat = 0x000a000a;
    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER1_BASE + 0x44,
                                  0x00020005, 4, true));
    assert(timers[1].intctlstat == 0x0008000f);

    assert(cdj_c6747_timers_write(timers, CDJ_C6747_TIMER1_BASE + 0x7c,
                                  0x12345678, 4, true));
    assert(cdj_c6747_timers_read(timers, CDJ_C6747_TIMER1_BASE + 0x7c, &value) &&
           value == 0x12345678);
    assert(!cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE, 0, 4, true));
    assert(!cdj_c6747_timers_write(timers, CDJ_C6747_TIMER0_BASE + 0x24, 0, 2, true));
    assert(!cdj_c6747_timers_read(timers, CDJ_C6747_TIMER0_BASE + 0x2c, &value));
    puts("C6747 Timer64P register semantics passed");
}
