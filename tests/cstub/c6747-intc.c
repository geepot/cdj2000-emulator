/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "cdj_c6747_intc.h"

int main(void)
{
    CdjC6747Intc s;
    uint32_t value;
    cdj_c6747_intc_reset(&s);
    assert(s.event_mask[0] == 0xf && !s.event_mask[1] &&
           s.exception_mask[0] == UINT32_MAX &&
           s.interrupt_mux[0] == 0x07060504 &&
           s.interrupt_mux[2] == 0x0f0e0d0c);

    assert(!cdj_c6747_intc_event(&s, 3));
    assert(cdj_c6747_intc_event(&s, 4));
    assert(cdj_c6747_intc_event(&s, 34));
    assert(cdj_c6747_intc_event(&s, 127));
    assert(!cdj_c6747_intc_event(&s, 128));
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_EVTFLAG0, &value) &&
           value == 0x10);
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_EVTFLAG0 + 4, &value) &&
           value == 4);

    /* Check-phase writes are atomic and command registers do not invent
     * readback. EVTSET cannot manufacture reserved events 0..3. */
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EVTSET0,
                                UINT32_MAX, 4, false));
    assert(s.event_flag[0] == 0x10);
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EVTSET0,
                                UINT32_MAX, 4, true));
    assert(s.event_flag[0] == 0xfffffff0);
    assert(!cdj_c6747_intc_read(&s, CDJ_C6747_INTC_EVTSET0, &value));
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EVTCLR0,
                                0x80000010, 4, true));
    assert(s.event_flag[0] == 0x7fffffe0);

    /* Masked event and exception views are derived, not stale latches. */
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EVTMASK0,
                                0x7fffff00, 4, true));
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_EVTMASK0, &value) &&
           value == 0x7fffff0f);
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_MEVTFLAG0, &value) &&
           value == 0xe0);
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EXPMASK0,
                                0x7fffff00, 4, true));
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_MEXPFLAG0, &value) &&
           value == 0xe0);

    /* Four 7-bit selectors occupy separate bytes; reserved high bits read 0. */
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_INTMUX1,
                                UINT32_MAX, 4, true));
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_INTMUX1, &value) &&
           value == 0x7f7f7f7f);

    assert(!cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EVTFLAG0, 0, 4, true));
    assert(!cdj_c6747_intc_write(&s, CDJ_C6747_INTC_EVTCLR0, 0, 2, true));
    assert(!cdj_c6747_intc_read(&s, CDJ_C6747_INTC_BASE + 0x10, &value));
    puts("C6747 interrupt controller register semantics passed");
}
