/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "cdj_c6747_spi.h"

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
        assert(cdj_c6747_spi_wm8740_write_timed(
            spis, dac, transfer, CDJ_C6747_SPI1_BASE + setup[i][0],
            setup[i][1], 4, true));
}

static void write_word(CdjC6747Spi spis[2], CdjWm8740 *dac,
                       CdjC6747SpiTransfer *transfer, uint32_t word)
{
    assert(cdj_c6747_spi_wm8740_write_timed(
        spis, dac, transfer, CDJ_C6747_SPI1_BASE + 0x3c,
        word, 4, false));
    assert(cdj_c6747_spi_wm8740_write_timed(
        spis, dac, transfer, CDJ_C6747_SPI1_BASE + 0x3c,
        word, 4, true));
}

int main(void)
{
    CdjC6747Spi spis[2];
    CdjWm8740 dac;
    CdjC6747SpiTransfer transfer;
    uint32_t value;
    assert(sizeof(transfer) == 40);

    configure(spis, &dac, &transfer);
    write_word(spis, &dac, &transfer, 0x1ff);
    assert(transfer.phase == 1 && transfer.half_ticks_remaining == 8 &&
           (spis[1].flags & 0x200) && spis[1].receive_empty);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 7));
    assert(transfer.phase == 1 && transfer.half_ticks_remaining == 1);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 1));
    assert(transfer.phase == 2 && transfer.half_ticks_remaining == 775);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 774));
    assert(spis[1].receive_empty && !dac.transfers);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 1));
    assert(transfer.phase == 3 && transfer.half_ticks_remaining == 31 &&
           !spis[1].receive_empty && !dac.transfers);
    assert(cdj_c6747_spi_wm8740_read_timed(
        spis, &transfer, CDJ_C6747_SPI1_BASE + 0x40, &value));
    assert(value == 0xffff && spis[1].receive_empty);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 30));
    assert(!dac.transfers);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 1));
    assert(transfer.phase == 4 && transfer.half_ticks_remaining == 4 &&
           dac.transfers == 1 && dac.last_word == 0x1ff);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 4));
    assert(!cdj_c6747_spi_transfer_active(&transfer) &&
           cdj_c6747_spi_transfer_valid(&transfer));

    /* A write before RX completion occupies TXBUF and sets TXFULL.  At RX the
     * queued word moves out of TXBUF, but waits through the first word's T2C
     * and its own C2T interval before shifting. */
    configure(spis, &dac, &transfer);
    write_word(spis, &dac, &transfer, 0x1ff);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 108));
    write_word(spis, &dac, &transfer, 0x3ff);
    assert(transfer.queued_valid && transfer.tx_full && !(spis[1].flags & 0x200));
    assert(cdj_c6747_spi_wm8740_read_timed(
        spis, &transfer, CDJ_C6747_SPI1_BASE + 0x40, &value));
    assert(value == 0xa0000000u);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 675));
    assert(transfer.phase == 3 && transfer.queued_valid && !transfer.tx_full &&
           (spis[1].flags & 0x300) == 0x300);
    assert(cdj_c6747_spi_wm8740_read_timed(
        spis, &transfer, CDJ_C6747_SPI1_BASE + 0x40, &value));
    assert((value & 0xffff) == 0xffff);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 31));
    assert(dac.transfers == 1 && transfer.phase == 4 &&
           transfer.half_ticks_remaining == 4);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer,
                                        4 + 8 + 775 + 31));
    assert(dac.transfers == 2 && dac.last_word == 0x3ff && transfer.phase == 4);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 4));
    assert(!transfer.phase);

    /* A write after RX completion copies directly into the vacant shift
     * register: no transient TXFULL while the first word finishes T2C. */
    configure(spis, &dac, &transfer);
    write_word(spis, &dac, &transfer, 0x1ff);
    assert(cdj_c6747_spi_wm8740_advance(spis, &dac, &transfer, 783));
    write_word(spis, &dac, &transfer, 0x3ff);
    assert(transfer.queued_valid && !transfer.tx_full &&
           (spis[1].flags & 0x200));

    /* Busy configuration writes fail before mutation.  Peripheral reset is
     * the exception: it cancels pending transfer/DAC effects safely. */
    uint32_t format = spis[1].format[0];
    assert(!cdj_c6747_spi_wm8740_write_timed(
        spis, &dac, &transfer, CDJ_C6747_SPI1_BASE + 0x50,
        0x21811, 4, false));
    assert(spis[1].format[0] == format);
    transfer.clock_phase = 7;
    assert(cdj_c6747_spi_wm8740_write_timed(
        spis, &dac, &transfer, CDJ_C6747_SPI1_BASE, 0, 4, true));
    assert(!cdj_c6747_spi_transfer_active(&transfer) &&
           transfer.clock_phase == 7 && !dac.transfers);

    /* A non-idle tuple used by checkpoint migration remains independently
     * valid and catches malformed phase/queue relationships. */
    transfer = (CdjC6747SpiTransfer){
        .half_ticks_remaining = 775,
        .active_control = 0x1ff,
        .active_format = 0x21810,
        .active_delay = 0x02020408,
        .queued_control = 0x3ff,
        .queued_format = 0x21810,
        .queued_delay = 0x02020408,
        .phase = 2,
        .queued_valid = 1,
        .tx_full = 1,
    };
    assert(cdj_c6747_spi_transfer_valid(&transfer));
    transfer.phase = 3;
    assert(!cdj_c6747_spi_transfer_valid(&transfer));
    return 0;
}
