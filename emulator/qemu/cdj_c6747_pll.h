/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_PLL_H
#define CDJ_C6747_PLL_H
#include <stdbool.h>
#include <stdint.h>
/* SPRUH91D 7.4.3-17. Configuration plus synthetic divider GO transition.
 * POR defaults substitute for ROM handoff; physical clocks are not driven. */
typedef struct {
    uint32_t config[13];
    bool legacy_bit4_used; /* Explicit unverified C6747 readback assumption. */
    uint32_t active_dividers[7], target_dividers[7], command;
    unsigned go_remaining;
} CdjC6747Pll;
/* One DSP-cycle edge before bus commits. GO lasts eight subsequent cycles;
 * still synthetic latency, not physical OSCIN/PLL alignment timing. */
void cdj_c6747_pll_tick(CdjC6747Pll *s);
void cdj_c6747_pll_reset(CdjC6747Pll *s);
bool cdj_c6747_pll_read(const CdjC6747Pll *s, uint32_t address, uint32_t *value);
bool cdj_c6747_pll_write(CdjC6747Pll *s, uint32_t address,
                        uint64_t value, unsigned size, bool commit);
#endif
