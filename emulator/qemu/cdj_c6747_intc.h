/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_INTC_H
#define CDJ_C6747_INTC_H
#include <stdbool.h>
#include <stdint.h>

#define CDJ_C6747_INTC_BASE 0x01800000u
#define CDJ_C6747_INTC_EVTFLAG0 (CDJ_C6747_INTC_BASE + 0x00u)
#define CDJ_C6747_INTC_EVTSET0 (CDJ_C6747_INTC_BASE + 0x20u)
#define CDJ_C6747_INTC_EVTCLR0 (CDJ_C6747_INTC_BASE + 0x40u)
#define CDJ_C6747_INTC_EVTMASK0 (CDJ_C6747_INTC_BASE + 0x80u)
#define CDJ_C6747_INTC_MEVTFLAG0 (CDJ_C6747_INTC_BASE + 0xa0u)
#define CDJ_C6747_INTC_EXPMASK0 (CDJ_C6747_INTC_BASE + 0xc0u)
#define CDJ_C6747_INTC_MEXPFLAG0 (CDJ_C6747_INTC_BASE + 0xe0u)
#define CDJ_C6747_INTC_INTMUX1 (CDJ_C6747_INTC_BASE + 0x104u)

/* SPRUFK5A chapter 7 event, event-combiner, exception-combiner and interrupt
 * selector registers. Event-to-CPU delivery, exception/drop state, AEG and
 * acknowledgement are deliberately outside this register-level model. */
typedef struct {
    uint32_t event_flag[4], event_mask[4], exception_mask[4];
    uint32_t interrupt_mux[3];
} CdjC6747Intc;

/* Delivery state is separate because CdjC6747Intc is embedded in checkpoint
 * schemas 3 onward and its historical size is part of their native ABI. */
typedef struct {
    uint32_t cpu_request;
} CdjC6747IntcDelivery;

void cdj_c6747_intc_reset(CdjC6747Intc *s);
/* Latch one of the 124 external/system events (4..127). */
bool cdj_c6747_intc_event(CdjC6747Intc *s, unsigned event);
void cdj_c6747_intc_delivery_reset(CdjC6747IntcDelivery *delivery);
/* Latch event status and generate any direct/combined CPU request pulses. */
bool cdj_c6747_intc_deliver_event(CdjC6747Intc *s,
                                  CdjC6747IntcDelivery *delivery,
                                  unsigned event);
/* Consume CPU INT4-15 request pulses selected by INTMUX1-3. Sticky event flags
 * remain available through EVTFLAG/MEVTFLAG until software clears them. */
uint32_t cdj_c6747_intc_cpu_pending(CdjC6747IntcDelivery *delivery);
bool cdj_c6747_intc_read(const CdjC6747Intc *s, uint32_t address,
                         uint32_t *value);
/* Check phase is side-effect free. Command registers read as unmapped rather
 * than fabricating values; status registers reject writes. */
bool cdj_c6747_intc_write(CdjC6747Intc *s, uint32_t address,
                          uint64_t value, unsigned size, bool commit);
/* Delivery-aware MMIO path; EVTSET and event-combiner activation may generate
 * CPU request pulses in addition to changing the register state. */
bool cdj_c6747_intc_write_delivery(CdjC6747Intc *s,
                                   CdjC6747IntcDelivery *delivery,
                                   uint32_t address, uint64_t value,
                                   unsigned size, bool commit);

#endif
