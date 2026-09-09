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

void cdj_c6747_intc_reset(CdjC6747Intc *s);
/* Latch one of the 124 external/system events (4..127). */
bool cdj_c6747_intc_event(CdjC6747Intc *s, unsigned event);
bool cdj_c6747_intc_read(const CdjC6747Intc *s, uint32_t address,
                         uint32_t *value);
/* Check phase is side-effect free. Command registers read as unmapped rather
 * than fabricating values; status registers reject writes. */
bool cdj_c6747_intc_write(CdjC6747Intc *s, uint32_t address,
                          uint64_t value, unsigned size, bool commit);

#endif
