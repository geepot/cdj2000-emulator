/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_SPI_CLOCK_H
#define CDJ_C6747_SPI_CLOCK_H
#include "cdj_c6747_spi.h"
#include "cdj_c6747_pll.h"

static inline unsigned cdj_spi_divider(uint32_t value)
{
    return (value & 0x8000u) ? (value & 31u) + 1 : 1;
}

/* SPRUH91D Table 6-2 requires SYSCLK2 to remain half the CPU frequency.
 * Only settled enabled dividers are supported for live serial transfers;
 * PLL GO/clock changes while active are rejected by the bus integration. */
static inline bool cdj_spi_clock_ready(const CdjC6747Pll *pll)
{
    return !pll->go_remaining &&
        (pll->active_dividers[0] & 0x8000u) &&
        (pll->active_dividers[1] & 0x8000u) &&
        cdj_spi_divider(pll->active_dividers[1]) ==
            2 * cdj_spi_divider(pll->active_dividers[0]);
}

static inline void cdj_spi_core_tick(CdjC6747Spi spis[2], CdjWm8740 *dac,
                                     CdjC6747SpiTransfer *transfer,
                                     const CdjC6747Pll *pll)
{
    if (transfer->fault) return;
    if (!cdj_spi_clock_ready(pll)) {
        if (transfer->phase || transfer->queued_valid) transfer->fault = 1;
        else transfer->clock_phase = 0;
        return;
    }
    unsigned denominator = cdj_spi_divider(pll->active_dividers[1]);
    unsigned numerator = 2 * cdj_spi_divider(pll->active_dividers[0]);
    transfer->clock_phase += numerator;
    unsigned ticks = transfer->clock_phase / denominator;
    transfer->clock_phase %= denominator;
    if (!cdj_c6747_spi_wm8740_advance(spis, dac, transfer, ticks))
        transfer->fault = 1;
}
#endif
