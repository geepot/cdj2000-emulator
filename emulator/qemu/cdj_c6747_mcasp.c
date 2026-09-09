/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_mcasp.h"
/* C6747 datasheet SPRS377F Table 6-43: 16, 12, 4 serializers.
 * The summary's "16/9" wording is not the per-instance configuration. */
static const uint32_t pin_mask[3] = {0xfe00ffffu, 0xfe000fffu, 0xfe00000fu};
static bool locate(uint32_t address, unsigned *bank, unsigned *offset)
{
    if (address < 0x01d00000u || address >= 0x01d0c000u || (address & 3))
        return false;
    *bank = (address - 0x01d00000u) / 0x4000;
    *offset = (address - 0x01d00000u) % 0x4000;
    return true;
}
void cdj_c6747_mcasp_reset(CdjC6747Mcasp *s)
{
    *s = (CdjC6747Mcasp){0};
}
bool cdj_c6747_mcasp_read(const CdjC6747Mcasp *s, uint32_t address,
                         uint32_t *value)
{
    unsigned b, o;
    if (!locate(address, &b, &o)) return false;
    switch (o) {
    case 0x10: *value = s->pfunc[b]; return true;
    case 0x14: *value = s->pdir[b]; return true;
    case 0x18: *value = s->pdout[b]; return true;
    /* 0x1c reads PDIN, not PDOUT. External inputs are not modeled.
     * PDCLR is a write-only alias; do not invent its readback either. */
    default: return false;
    }
}
bool cdj_c6747_mcasp_write(CdjC6747Mcasp *s, uint32_t address,
                          uint64_t value, unsigned size, bool commit)
{
    unsigned b, o;
    if (size != 4 || value > UINT32_MAX || !locate(address, &b, &o)) return false;
    if (o < 0x10 || o > 0x20 || (value & ~pin_mask[b])) return false;
    /* Nonzero reserved bits explicitly stop; TI cautions against writing
     * them. Validation never changes state. E3 commit updates the latch. */
    if (!commit) return true;
    switch (o) {
    case 0x10: s->pfunc[b] = value; break;
    case 0x14: s->pdir[b] = value; break;
    case 0x18: s->pdout[b] = value; break;
    case 0x1c: s->pdout[b] |= value; break;
    case 0x20: s->pdout[b] &= ~(uint32_t)value; break;
    }
    return true;
}
