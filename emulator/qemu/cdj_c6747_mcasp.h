/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_MCASP_H
#define CDJ_C6747_MCASP_H
#include <stdbool.h>
#include <stdint.h>
/* SPRUH91D 24.1: configuration and transmit-buffer state. No physical pin
 * routing, external input, serializer clock progression, FIFO or interrupt
 * delivery yet. */
typedef struct {
    uint32_t pfunc[3], pdir[3], pdout[3];
} CdjC6747Mcasp;
/* Keep control state separate from CdjC6747Mcasp: the pin-only type is part
 * of older checkpoint layouts and must remain exactly 36 bytes. */
typedef struct {
    uint32_t undocumented04[3];
    uint32_t gblctl[3], amute[3], dlbctl[3], ditctl[3];
    uint32_t xmask[3], xfmt[3], afsxctl[3];
    uint32_t aclkxctl[3], ahclkxctl[3], xtdm[3];
    uint32_t xintctl[3], xstat[3], xclkchk[3];
    uint32_t dit[3][24];
    uint32_t srctl[3][16], xrdy[3];
    /* XBUF holds only genuine words accepted through one of the two hardware
     * ports.  Sequence numbers retain their order without claiming that a
     * serializer clock has shifted any word to an external audio device. */
    uint32_t xbuf[3][16], xdma_next[3];
    uint64_t xbuf_sequence[3][16], xbuf_writes[3], axevt_generation[3];
    /* Schema-7 state ends above.  These appended fields record only explicit
     * slot-boundary calls and genuine accepted XBUF words; they do not imply
     * a physical serializer clock or an external audio sample. */
    uint32_t xrsr[3][16], xslot[3];
    uint64_t xrsr_source_sequence[3][16], tx_slot_boundaries[3];
} CdjC6747McaspControl;
void cdj_c6747_mcasp_reset(CdjC6747Mcasp *s);
bool cdj_c6747_mcasp_read(const CdjC6747Mcasp *s, uint32_t address,
                         uint32_t *value);
bool cdj_c6747_mcasp_write(CdjC6747Mcasp *s, uint32_t address,
                          uint64_t value, unsigned size, bool commit);
void cdj_c6747_mcasp_control_reset(CdjC6747McaspControl *s);
bool cdj_c6747_mcasp_control_valid(const CdjC6747McaspControl *s);
bool cdj_c6747_mcasp_control_read(const CdjC6747McaspControl *s,
                                 uint32_t address, uint32_t *value);
bool cdj_c6747_mcasp_control_write(CdjC6747McaspControl *s,
                                  uint32_t address, uint64_t value,
                                  unsigned size, bool commit);
/* Level query for the documented XDATA/AXEVT service condition.  A false
 * result for an invalid instance also keeps callers from inventing events. */
bool cdj_c6747_mcasp_axevt_ready(const CdjC6747McaspControl *s,
                                unsigned instance);
/* Advance one caller-timed, architecture-valid transmit slot.  On success,
 * axevt reports whether this active slot rearmed XDATA/AXEVT.  Inactive TDM
 * slots still advance XSLOT but do not consume XBUF or generate an event.
 * Failure is side-effect free and covers stopped resets/clocks and unsupported
 * or inconsistent frame configuration. */
bool cdj_c6747_mcasp_tx_slot(CdjC6747McaspControl *s, unsigned instance,
                            bool *axevt);
#endif
