/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_pll.h"
static const unsigned offsets[] = {0x100,0x104,0x110,0x114,0x118,0x11c,
                                  0x120,0x124,0x128,0x160,0x164,0x168,0x16c};
static const uint32_t defaults[] = {0xf2,0x14,0x13,0x8000,0x8000,0x8001,
                                   0x8002,0x8000,0x8001,0x8003,0x8002,0x8000,0x8005};
static const unsigned divider_index[] = {4,5,6,9,10,11,12};
static int index_of(uint32_t address)
{
    for (unsigned i = 0; i < 13; ++i)
        if (address == 0x01c11000u + offsets[i]) return i;
    return -1;
}
void cdj_c6747_pll_reset(CdjC6747Pll *s)
{
    *s = (CdjC6747Pll){0};
    for (unsigned i = 0; i < 13; ++i) s->config[i] = defaults[i];
    for (unsigned i = 0; i < 7; ++i)
        s->active_dividers[i] = s->target_dividers[i] = defaults[divider_index[i]];
}
void cdj_c6747_pll_tick(CdjC6747Pll *s)
{
    if (s->go_remaining && --s->go_remaining == 0)
        for (unsigned i = 0; i < 7; ++i) s->active_dividers[i] = s->target_dividers[i];
}
bool cdj_c6747_pll_read(const CdjC6747Pll *s, uint32_t address, uint32_t *value)
{
    if (address == 0x01c11138u) { *value = s->command; return true; }
    if (address == 0x01c1113cu) {
        /* STABLE is oscillator-counter completion, not PLL lock. Assume
         * it completed before ROM handoff (7.4.17); no pin timing modeled. */
        *value = 4 | (s->go_remaining != 0); return true;
    }
    int i = index_of(address);
    if (i < 0) return false;
    *value = s->config[i];
    return true;
}
bool cdj_c6747_pll_write(CdjC6747Pll *s, uint32_t address,
                        uint64_t value, unsigned size, bool commit)
{
    if (address == 0x01c11138u) {
        if (size != 4 || value > 1 || (value && s->go_remaining)) return false;
        if (commit) {
            s->command = value; /* W0C command latch, SPRUH91D 7.4.16. */
            if (value) {
                for (unsigned d = 0; d < 7; ++d)
                    s->target_dividers[d] = s->config[divider_index[d]];
                s->go_remaining = 8;
            }
        }
        return true;
    }
    int i = index_of(address);
    if (i < 0 || size != 4 || value > UINT32_MAX) return false;
    /* Reprogramming during phase alignment is not modeled. */
    if (s->go_remaining) return false;
    uint32_t mask = i == 0 ? 0x1fb : i <= 2 ? 31 : 0x801f;
    if (value & ~mask) return false;
    if (i == 0) {
        /* TI marks bit 4 reserved-one, but legacy DaVinci PLL code clears
         * it as PLLDIS (Linux v6.1 drivers/clk/davinci/pll.c). Preserve the
         * writable latch as a flagged compatibility assumption, NOT proof
         * of C6747 physical clock semantics. PLLEN/PLLRST still stop. */
        if (value & 9) return false;
        value |= 0xc0;
    }
    if (i == 1 && value != 0x14 && value != 0x1f && (value < 0x17 || value > 0x1d))
        return false; /* OCSEL reserved source selectors. */
    /* PLL_MASTER_LOCK defaults clear; setting it through SYSCFG remains
     * unsupported there. Physical clock outputs remain unconnected. */
    if (commit) {
        s->config[i] = value;
        if (i == 0 && !(value & 16)) s->legacy_bit4_used = true;
    }
    return true;
}
