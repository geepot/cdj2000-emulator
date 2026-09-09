/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include "cdj_c6747_spi_clock.h"

static void configure(CdjC6747Spi spis[2], CdjWm8740 *dac,
                      CdjC6747SpiTransfer *transfer)
{
    cdj_c6747_spis_reset(spis);
    cdj_wm8740_reset(dac);
    cdj_c6747_spi_transfer_reset(transfer);
    const uint32_t setup[][2] = {
        {0, 1}, {4, 3}, {0x14, 0xe01}, {0x3c, 0}, {0x3c, 0},
        {0x50, 0x21810}, {0x48, 0x02020408}, {8, 0}, {0xc, 0},
        {4, 0x01000003},
    };
    for (unsigned i = 0; i < sizeof(setup) / sizeof(setup[0]); ++i)
        assert(cdj_c6747_spi_wm8740_write_timed(spis, dac, transfer,
                   CDJ_C6747_SPI1_BASE + setup[i][0], setup[i][1], 4, true));
    assert(cdj_c6747_spi_wm8740_write_timed(spis, dac, transfer,
               CDJ_C6747_SPI1_BASE + 0x3c, 0x1ff, 4, true));
}

int main(void)
{
    CdjC6747Pll pll;
    CdjC6747Spi spis[2];
    CdjWm8740 dac;
    CdjC6747SpiTransfer transfer;
    /* SYSCLK2's required 1:2 ratio, independent of common divider scaling. */
    for (unsigned divisor = 1; divisor <= 16; ++divisor) {
        cdj_c6747_pll_reset(&pll);
        pll.active_dividers[0] = 0x8000 | (divisor - 1);
        pll.active_dividers[1] = 0x8000 | (2 * divisor - 1);
        assert(cdj_spi_clock_ready(&pll));
        configure(spis, &dac, &transfer);
        /* Fig27-10: first TX at C2T end; final RX is 15.5 periods later.
         * 8 + 31*25 half-module ticks = 783 CPU cycles. */
        for (unsigned i = 1; i <= 814; ++i) {
            cdj_spi_core_tick(spis, &dac, &transfer, &pll);
            assert(!transfer.fault && !transfer.clock_phase);
            assert(spis[1].receive_empty == (i < 783));
            assert(dac.transfers == (i >= 814));
        }
        /* CS has risen, but the mandatory two-module-clock gap remains. */
        assert(transfer.phase && transfer.half_ticks_remaining == 4);
        for (unsigned gap = 0; gap < 4; ++gap) {
            cdj_spi_core_tick(spis, &dac, &transfer, &pll);
            assert(!transfer.fault && dac.transfers == 1);
        }
        assert(!transfer.phase);
    }
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        cdj_c6747_pll_reset(&pll);
        configure(spis, &dac, &transfer);
        if (invalid == 0) pll.go_remaining = 1;
        if (invalid == 1) pll.active_dividers[1] = 0x8000;
        if (invalid == 2) pll.active_dividers[0] = 0;
        assert(!cdj_spi_clock_ready(&pll));
        cdj_spi_core_tick(spis, &dac, &transfer, &pll);
        assert(transfer.fault && !dac.transfers && spis[1].receive_empty);
    }
    return 0;
}
