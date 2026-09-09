/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_pll.h"
static const unsigned offsets[] = {0x100,0x104,0x110,0x114,0x118,0x11c,
                                  0x120,0x124,0x128,0x160,0x164,0x168,0x16c};
static const uint32_t defaults[] = {0xf2,0x14,0x13,0x8000,0x8000,0x8001,
                                   0x8002,0x8000,0x8001,0x8003,0x8002,0x8000,0x8005};
static int index_of(uint32_t address)
{
    for (unsigned i = 0; i < 13; ++i)
        if (address == 0x01c11000u + offsets[i]) return i;
    return -1;
}
void cdj_c6747_pll_reset(CdjC6747Pll *s)
{
    for (unsigned i = 0; i < 13; ++i) s->config[i] = defaults[i];
}
bool cdj_c6747_pll_read(const CdjC6747Pll *s, uint32_t address, uint32_t *value)
{
    int i = index_of(address);
    if (i < 0) return false;
    *value = s->config[i];
    return true;
}
bool cdj_c6747_pll_write(CdjC6747Pll *s, uint32_t address,
                        uint64_t value, unsigned size, bool commit)
{
    int i = index_of(address);
    if (i < 0 || size != 4 || value > UINT32_MAX) return false;
    uint32_t mask = i == 0 ? 0x1fb : i <= 2 ? 31 : 0x801f;
    if (value & ~mask) return false;
    if (i == 0) {
        /* Reserved bit 4 must retain its default one; bits 7:6 read one.
         * PLLEN and PLLRST release need real clock/lock transition state.
         * Reject them rather than claiming an instantly running PLL. */
        if (!(value & 16) || (value & 9)) return false;
        value |= 0xc0;
    }
    if (i == 1 && value != 0x14 && value != 0x1f && (value < 0x17 || value > 0x1d))
        return false; /* OCSEL reserved source selectors. */
    /* PLL_MASTER_LOCK defaults clear; setting it through SYSCFG remains
     * unsupported there. GO/status/clock outputs are intentionally unmapped. */
    if (commit) s->config[i] = value;
    return true;
}
