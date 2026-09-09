/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_hpi.h"
static uint16_t host_value(const CdjC6747Hpi *s)
{
    /* Reserved reset fields 6:5=2 and bit 3=1 read back as documented. */
    return 0x48 | (s->hpirst ? 0x80 : 0) | (s->hwob ? 0x101 : 0) |
           (s->dual_hpia ? 0x200 : 0) | (s->hpiasel ? 0x800 : 0) |
           (s->hint ? 4 : 0) | (s->dspint ? 2 : 0);
}
void cdj_c6747_hpi_reset(CdjC6747Hpi *s)
{
    *s = (CdjC6747Hpi){.hpirst = true};
}
void cdj_c6747_hpi_rom_boot_ready(CdjC6747Hpi *s)
{
    /* SPRABB1C 4.1: after reset in HPI boot mode, the ROM bootloader releases
     * the HPI and sets HINT to tell the host it may begin the download. */
    s->hpirst = false;
    s->hint = true;
    s->dspint = false;
}
uint32_t cdj_c6747_hpi_host_read(const CdjC6747Hpi *s)
{
    uint32_t v = host_value(s);
    return v | v << 16; /* HPIC ignores HHWIL and mirrors both halfwords. */
}
void cdj_c6747_hpi_host_write(CdjC6747Hpi *s, uint32_t value)
{
    /* Either host halfword carries the same register transaction. Genuine
     * MAIN duplicates both; accepting their union also models 16-bit hosts. */
    uint16_t v = value | value >> 16;
    bool reset = (v & 0x80) != 0;
    s->hwob = (v & 1) != 0;
    s->dual_hpia = (v & 0x200) != 0;
    s->hpiasel = (v & 0x800) != 0;
    if (v & 4) s->hint = false; /* Host W1C acknowledgement. */
    if (v & 2) s->dspint = true;
    s->hpirst = reset;
    if (reset) s->hint = s->dspint = false;
}
bool cdj_c6747_hpi_cpu_read(const CdjC6747Hpi *s, uint32_t address,
                           uint32_t *value)
{
    if (address == CDJ_C6747_HPI_BASE) {
        *value = 0x4421210a; /* REVID, SPRUH91D Figure 21-17. */
        return true;
    }
    if (address != CDJ_C6747_HPIC) return false;
    /* CPU observes host controls through status bits; HWOB itself reads 0. */
    *value = host_value(s) & ~1u;
    return true;
}
bool cdj_c6747_hpi_cpu_write(CdjC6747Hpi *s, uint32_t address,
                            uint64_t value, unsigned size, bool commit)
{
    if (address != CDJ_C6747_HPIC || size != 4 || value > UINT32_MAX) return false;
    if (!commit) return true;
    uint32_t v = value;
    s->hpirst = (v & 0x80) != 0;
    if (v & 2) s->dspint = false; /* CPU acknowledges host interrupt. */
    if (v & 4) s->hint = true;    /* CPU raises active-low UHPI_HINT. */
    if (s->hpirst) s->hint = s->dspint = false;
    return true;
}
