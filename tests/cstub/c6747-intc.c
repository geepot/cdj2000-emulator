/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "cdj_c6747_intc.h"

int main(void)
{
    CdjC6747Intc s;
    CdjC6747IntcDelivery request;
    uint32_t value;
    cdj_c6747_intc_reset(&s);
    cdj_c6747_intc_delivery_reset(&request);
    assert(cdj_c6747_intc_cpu_pending(&request) == 0);
    assert(s.event_mask[0] == 0xf && !s.event_mask[1] &&
           s.exception_mask[0] == UINT32_MAX &&
           s.interrupt_mux[0] == 0x07060504 &&
           s.interrupt_mux[2] == 0x0f0e0d0c);

    assert(!cdj_c6747_intc_deliver_event(&s, &request, 3));
    assert(cdj_c6747_intc_deliver_event(&s, &request, 4));
    assert(cdj_c6747_intc_cpu_pending(&request) == (1u << 4));
    assert(cdj_c6747_intc_cpu_pending(&request) == 0);
    assert(cdj_c6747_intc_deliver_event(&s, &request, 34));
    assert(cdj_c6747_intc_cpu_pending(&request) == 0);
    assert(cdj_c6747_intc_write(&s, CDJ_C6747_INTC_INTMUX1 + 8,
                                0x220e0d0c, 4, true));
    /* Remapping does not replay a sticky status flag. A subsequent direct
     * event is delivered once even though EVTFLAG was already set. */
    assert(cdj_c6747_intc_cpu_pending(&request) == 0);
    assert(cdj_c6747_intc_deliver_event(&s, &request, 34));
    assert(cdj_c6747_intc_cpu_pending(&request) == (1u << 15));
    assert(cdj_c6747_intc_cpu_pending(&request) == 0);
    assert(cdj_c6747_intc_deliver_event(&s, &request, 127));
    assert(!cdj_c6747_intc_deliver_event(&s, &request, 128));
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_EVTFLAG0, &value) &&
           value == 0x10);
    assert(cdj_c6747_intc_read(&s, CDJ_C6747_INTC_EVTFLAG0 + 4, &value) &&
           value == 4);

    /* EVTMASK affects only event-combiner outputs. INT4 still receives direct
     * event 4 while masked; INT5 receives combined event 0 only on inactive to
     * active transitions, and each request pulse is consumed exactly once. */
    CdjC6747Intc combined_intc;
    CdjC6747IntcDelivery combined_request;
    cdj_c6747_intc_reset(&combined_intc);
    cdj_c6747_intc_delivery_reset(&combined_request);
    assert(cdj_c6747_intc_write_delivery(
        &combined_intc, &combined_request, CDJ_C6747_INTC_INTMUX1,
        0x07060004, 4, true));
    assert(cdj_c6747_intc_write_delivery(
        &combined_intc, &combined_request, CDJ_C6747_INTC_EVTMASK0,
        1u << 4, 4, true));
    assert(cdj_c6747_intc_deliver_event(&combined_intc, &combined_request, 4));
    assert(cdj_c6747_intc_cpu_pending(&combined_request) == (1u << 4));
    assert(cdj_c6747_intc_write_delivery(
        &combined_intc, &combined_request, CDJ_C6747_INTC_EVTMASK0,
        0, 4, true));
    assert(cdj_c6747_intc_cpu_pending(&combined_request) == (1u << 5));
    assert(cdj_c6747_intc_deliver_event(&combined_intc, &combined_request, 5));
    assert(cdj_c6747_intc_cpu_pending(&combined_request) == 0);
    assert(cdj_c6747_intc_write_delivery(
        &combined_intc, &combined_request, CDJ_C6747_INTC_EVTCLR0,
        (1u << 4) | (1u << 5), 4, true));
    assert(cdj_c6747_intc_deliver_event(&combined_intc, &combined_request, 5));
    assert(cdj_c6747_intc_cpu_pending(&combined_request) == (1u << 5));
    assert(cdj_c6747_intc_cpu_pending(&combined_request) == 0);

    /* Check-phase writes are atomic and command registers do not invent
     * readback. EVTSET cannot manufacture reserved events 0..3. */
    assert(cdj_c6747_intc_write_delivery(
        &s, &request, CDJ_C6747_INTC_EVTSET0, UINT32_MAX, 4, false));
    assert(s.event_flag[0] == 0x10);
    assert(request.cpu_request == 0);
    assert(cdj_c6747_intc_write_delivery(
        &s, &request, CDJ_C6747_INTC_EVTSET0, UINT32_MAX, 4, true));
    assert(s.event_flag[0] == 0xfffffff0);
    assert(cdj_c6747_intc_cpu_pending(&request) == 0x7ff0u);
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
