/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cdj_c6747_intc.h"

static bool banked(uint32_t address, uint32_t base, unsigned *bank)
{
    if (address < base || address > base + 12 || (address - base) & 3)
        return false;
    *bank = (address - base) / 4;
    return true;
}

static bool combined_active(const CdjC6747Intc *s, unsigned bank)
{
    return (s->event_flag[bank] & ~s->event_mask[bank]) != 0;
}

static void request_selected_event(const CdjC6747Intc *s,
                                   CdjC6747IntcDelivery *delivery,
                                   unsigned event)
{
    if (!delivery) return;
    for (unsigned interrupt = 4; interrupt < 16; ++interrupt) {
        unsigned selector = interrupt - 4;
        unsigned selected = (s->interrupt_mux[selector / 4] >>
                             ((selector % 4) * 8)) & 0x7f;
        if (selected == event) delivery->cpu_request |= 1u << interrupt;
    }
}

void cdj_c6747_intc_reset(CdjC6747Intc *s)
{
    memset(s, 0, sizeof(*s));
    /* Events 0..3 are internal event-combiner outputs, not flag inputs, and
     * are permanently masked. Exception inputs reset entirely masked. */
    s->event_mask[0] = 0xf;
    for (unsigned i = 0; i < 4; ++i) s->exception_mask[i] = UINT32_MAX;
    /* CPUINT4..15 select same-numbered events after reset. */
    s->interrupt_mux[0] = 0x07060504;
    s->interrupt_mux[1] = 0x0b0a0908;
    s->interrupt_mux[2] = 0x0f0e0d0c;
}

bool cdj_c6747_intc_event(CdjC6747Intc *s, unsigned event)
{
    if (event < 4 || event >= 128) return false;
    s->event_flag[event / 32] |= 1u << (event % 32);
    return true;
}

void cdj_c6747_intc_delivery_reset(CdjC6747IntcDelivery *delivery)
{
    *delivery = (CdjC6747IntcDelivery){0};
}

bool cdj_c6747_intc_deliver_event(CdjC6747Intc *s,
                                  CdjC6747IntcDelivery *delivery,
                                  unsigned event)
{
    if (event < 4 || event >= 128) return false;
    unsigned bank = event / 32;
    bool combined_was_active = combined_active(s, bank);
    cdj_c6747_intc_event(s, event);

    /* SPRUFK5A 7.2.2 and 7.4.2: original events 4..127 reach the
     * interrupt selector directly and do not depend on EVTMASK or clearing
     * EVTFLAG. The latter remains a sticky status bit. */
    request_selected_event(s, delivery, event);
    if (!combined_was_active && combined_active(s, bank))
        request_selected_event(s, delivery, bank);
    return true;
}

uint32_t cdj_c6747_intc_cpu_pending(CdjC6747IntcDelivery *delivery)
{
    uint32_t pending = delivery->cpu_request;
    delivery->cpu_request = 0;
    return pending;
}

bool cdj_c6747_intc_read(const CdjC6747Intc *s, uint32_t address,
                         uint32_t *value)
{
    unsigned bank;
    if (banked(address, CDJ_C6747_INTC_EVTFLAG0, &bank))
        *value = s->event_flag[bank];
    else if (banked(address, CDJ_C6747_INTC_EVTMASK0, &bank))
        *value = s->event_mask[bank];
    else if (banked(address, CDJ_C6747_INTC_MEVTFLAG0, &bank))
        *value = s->event_flag[bank] & ~s->event_mask[bank];
    else if (banked(address, CDJ_C6747_INTC_EXPMASK0, &bank))
        *value = s->exception_mask[bank];
    else if (banked(address, CDJ_C6747_INTC_MEXPFLAG0, &bank))
        *value = s->event_flag[bank] & ~s->exception_mask[bank];
    else if (address >= CDJ_C6747_INTC_INTMUX1 &&
             address <= CDJ_C6747_INTC_INTMUX1 + 8 &&
             !((address - CDJ_C6747_INTC_INTMUX1) & 3))
        *value = s->interrupt_mux[(address - CDJ_C6747_INTC_INTMUX1) / 4];
    else return false;
    return true;
}

bool cdj_c6747_intc_write_delivery(CdjC6747Intc *s,
                                   CdjC6747IntcDelivery *delivery,
                                   uint32_t address, uint64_t value,
                                   unsigned size, bool commit)
{
    unsigned bank;
    if (size != 4 || value > UINT32_MAX) return false;
    if (banked(address, CDJ_C6747_INTC_EVTSET0, &bank)) {
        if (commit) {
            uint32_t events = (uint32_t)value & (!bank ? ~0xfu : UINT32_MAX);
            bool combined_was_active = combined_active(s, bank);
            s->event_flag[bank] |= events;
            for (unsigned bit = 0; bit < 32; ++bit)
                if (events & (1u << bit))
                    request_selected_event(s, delivery, bank * 32 + bit);
            if (!combined_was_active && combined_active(s, bank))
                request_selected_event(s, delivery, bank);
        }
    } else if (banked(address, CDJ_C6747_INTC_EVTCLR0, &bank)) {
        if (commit) s->event_flag[bank] &= ~(uint32_t)value;
    } else if (banked(address, CDJ_C6747_INTC_EVTMASK0, &bank)) {
        if (commit) {
            bool combined_was_active = combined_active(s, bank);
            s->event_mask[bank] = (uint32_t)value | (!bank ? 0xfu : 0u);
            if (!combined_was_active && combined_active(s, bank))
                request_selected_event(s, delivery, bank);
        }
    } else if (banked(address, CDJ_C6747_INTC_EXPMASK0, &bank)) {
        if (commit)
            s->exception_mask[bank] = (uint32_t)value | (!bank ? 0xfu : 0u);
    } else if (address >= CDJ_C6747_INTC_INTMUX1 &&
               address <= CDJ_C6747_INTC_INTMUX1 + 8 &&
               !((address - CDJ_C6747_INTC_INTMUX1) & 3)) {
        if (commit)
            s->interrupt_mux[(address - CDJ_C6747_INTC_INTMUX1) / 4] =
                (uint32_t)value & 0x7f7f7f7fu;
    } else return false;
    return true;
}

bool cdj_c6747_intc_write(CdjC6747Intc *s, uint32_t address,
                          uint64_t value, unsigned size, bool commit)
{
    return cdj_c6747_intc_write_delivery(s, NULL, address, value, size,
                                         commit);
}
