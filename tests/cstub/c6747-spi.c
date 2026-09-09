#include <assert.h>
#include <stdio.h>

#include "cdj_c6747_spi.h"

int main(void)
{
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT];
    CdjWm8740 dac;
    uint32_t value;
    cdj_c6747_spis_reset(spis);
    cdj_wm8740_reset(&dac);

    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE, &value) && value == 0);
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI1_BASE + 0x40, &value) &&
           value == 0x80000000);
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x10, &value) &&
           value == 0x01000000);
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x4c, &value) &&
           value == 0xff);

    /* The reached firmware initialization sequence is accepted as one family,
     * with reserved bits masked and no fabricated transfer completion. */
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE, 1, 4, false));
    assert(spis[1].gcr0 == 0);
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE, 1, 4, true));
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE + 0x04, 3, 4, true));
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE + 0x14,
                                0xffff, 4, true));
    assert(spis[1].pin_function == CDJ_C6747_SPI_PIN_MASK);
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE + 0x3c, 0, 4, true));
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE + 0x50,
                                0xe00fffff, 4, true));
    assert(spis[1].format[0] == 0x2007ff1f);
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE + 0x50,
                                0x00021810, 4, true));
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE + 0x48,
                                0x02020408, 4, true));
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE + 0x08,
                                UINT32_MAX, 4, true));
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE + 0x0c,
                                UINT32_MAX, 4, true));
    assert(spis[1].interrupt_enable == 0x0101035f &&
           spis[1].interrupt_level == 0x35f);
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE + 0x04,
                                0x01000003, 4, true));
    assert(!cdj_c6747_spis_write(spis, CDJ_C6747_SPI1_BASE + 0x3c,
                                 0x1234, 4, false));
    assert(spis[1].dat1 == 0);

    /* Breadth mode attaches the schematic-confirmed write-only WM8740 to
     * SPI1. Check phase stays atomic, while commit completes one 16-bit word
     * and exposes controller status. The NC SOMI pad has an internal pull-up,
     * so this is a sampled 0xffff bus value rather than a DAC response. */
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x1ff, 4, true, false));
    assert(spis[1].dat1 == 0 && spis[1].receive_empty && !spis[1].flags &&
           dac.transfers == 0 && dac.program[0] == 0xff);
    assert(!cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x1ff, 4, false, true));
    /* CSHOLD has no per-word chip-select release for the DAC to latch. Keep
     * that sequence fail-closed until continuous-selection timing exists. */
    assert(!cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c,
        (1u << 28) | 0x1ff, 4, true, true));
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x1ff, 4, true, true));
    assert(spis[1].dat1 == 0x1ff && spis[1].flags == 0x300 &&
           !spis[1].receive_empty && spis[1].receive_data == 0xffff &&
           dac.transfers == 1 && dac.last_word == 0x1ff &&
           dac.program[0] == 0x1ff && dac.active_attenuation[0] == 0xff &&
           dac.active_attenuation[1] == 0xff && cdj_wm8740_valid(&dac));

    /* Acceptance cannot depend on mutable receive fullness: multiple DSP
     * stores may all preflight before their E3 commits. The second completion
     * occupies RXBUF; only a third unread completion reports overrun and the
     * existing SPIBUF/RXBUF words remain intact. */
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x3ff, 4, true, false));
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x400, 4, true, false));
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x3ff, 4, true, true));
    assert(spis[1].receive_buffer_full && spis[1].flags == 0x300 &&
           dac.transfers == 2);
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x400, 4, true, true));
    assert(spis[1].receive_buffer_full && spis[1].flags == 0x340 &&
           spis[1].receive_status == 0x40000000 && dac.transfers == 3);
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI1_BASE + 0x40, &value) &&
           value == 0x4000ffff && !spis[1].receive_empty &&
           !spis[1].receive_buffer_full && spis[1].flags == 0x340);
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI1_BASE + 0x40, &value) &&
           value == 0x4000ffff && spis[1].receive_empty &&
           spis[1].flags == 0x240);
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x3ff, 4, true, true));
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI1_BASE + 0x40, &value));
    assert(dac.transfers == 4 && dac.program[1] == 0x1ff &&
           dac.active_attenuation[0] == 0xff &&
           dac.active_attenuation[1] == 0xff);
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x400, 4, true, true));
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI1_BASE + 0x40, &value));
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x618, 4, true, true));
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI1_BASE + 0x40, &value));
    assert(dac.program[2] == 0 && dac.program[3] == 0x18);
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0xc70, 4, true, true));
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI1_BASE + 0x40, &value));
    assert(dac.program[4] == 0 && !dac.register4_unlocked);
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0x5e0, 4, true, true));
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI1_BASE + 0x40, &value));
    assert(cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI1_BASE + 0x3c, 0xc70, 4, true, true));
    assert(dac.register4_unlocked && dac.program[4] == 0x70 &&
           cdj_wm8740_valid(&dac));

    cdj_c6747_spis_reset(spis);
    cdj_wm8740_reset(&dac);
    assert(!cdj_c6747_spis_write_wm8740(
        spis, &dac, CDJ_C6747_SPI0_BASE + 0x3c, 0x1ff, 4, true, true));

    /* GPIO writes affect only pins selected as GPIO outputs. Pin reads stay
     * unavailable until every external/input value has evidence. */
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI0_BASE, 1, 4, true));
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI0_BASE + 0x18,
                                CDJ_C6747_SPI_PIN_MASK, 4, true));
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI0_BASE + 0x24,
                                0x901, 4, true));
    assert(spis[0].pin_output == 0x901);
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI0_BASE + 0x28,
                                0x100, 4, true));
    assert(spis[0].pin_output == 0x801);
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x1c, &value) &&
           value == 0x801);
    cdj_c6747_spis_reset(spis);
    assert(!cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x1c, &value));
    cdj_c6747_spi_set_pins(spis, 0, CDJ_C6747_SPI_PIN_MASK, 0x401);
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x24, &value) &&
           value == 0x401);

    /* Status/vector side effects follow SPRUH91D 27.3.5/14/19. */
    spis[0].flags = 0x35f;
    spis[0].receive_empty = false;
    spis[0].receive_data = 0x5678;
    spis[0].receive_status = 0x5f000000;
    assert(cdj_c6747_spis_write(spis, CDJ_C6747_SPI0_BASE + 0x10,
                                0x15f, 4, true));
    assert(spis[0].flags == 0x200 && spis[0].receive_empty);
    spis[0].flags = 0x340;
    spis[0].interrupt_enable = 0x340;
    spis[0].interrupt_level = 0x340;
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x64, &value) &&
           value == 0x26 && !(spis[0].flags & 0x40));
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x64, &value) &&
           value == 0x24 && !(spis[0].flags & 0x100));
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x64, &value) &&
           value == 0x28 && (spis[0].flags & 0x200));
    spis[0].receive_empty = false;
    spis[0].receive_status = 0x5f000000;
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x44, &value) &&
           value == 0x5678 && !spis[0].receive_empty);
    assert(cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x40, &value) &&
           value == 0x5f005678 && spis[0].receive_empty &&
           spis[0].receive_status == 0x40000000 &&
           (spis[0].flags & 0x100) == 0);

    assert(!cdj_c6747_spis_write(spis, CDJ_C6747_SPI0_BASE + 0x04, 1, 4, true));
    assert(!cdj_c6747_spis_write(spis, CDJ_C6747_SPI0_BASE + 0x40, 0, 4, true));
    assert(!cdj_c6747_spis_write(spis, CDJ_C6747_SPI0_BASE + 0x48, 0, 2, true));
    assert(!cdj_c6747_spis_read(spis, CDJ_C6747_SPI0_BASE + 0x2c, &value));
    puts("C6747 SPI register family passed");
    return 0;
}
