/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_gpio.h"
/* SPRUH91D 20.3.2-11; C6747 SPRS377F Table 6-8. Unlike McASP,
 * GPIO direction 1 means input. Bank 8 from the generic manual is absent. */
static bool locate(uint32_t address, unsigned *pair, unsigned *offset)
{
    if (address < 0x01e26010u || address >= 0x01e260b0u || (address & 3))
        return false;
    *pair = (address - 0x01e26010u) / 0x28;
    *offset = (address - 0x01e26010u) % 0x28;
    return true;
}
void cdj_c6747_gpio_reset(CdjC6747Gpio *s)
{
    *s = (CdjC6747Gpio){0};
    for (unsigned p = 0; p < 4; ++p) s->dir[p] = UINT32_MAX;
}
bool cdj_c6747_gpio_set_input(CdjC6747Gpio *s, unsigned bank, unsigned pin,
                              bool high)
{
    if (bank >= 8 || pin >= 16) return false;
    unsigned pair = bank / 2, bit = pin + (bank & 1) * 16;
    if (high) s->input[pair] |= 1u << bit;
    else s->input[pair] &= ~(1u << bit);
    return true;
}
bool cdj_c6747_gpio_read(const CdjC6747Gpio *s, uint32_t address, uint32_t *value)
{
    unsigned p, o;
    if (address == 0x01e26008u) { *value = s->binten; return true; }
    if (!locate(address, &p, &o)) return false;
    switch (o) {
    case 0: *value = s->dir[p]; break;
    case 4: case 8: case 12: *value = s->output[p]; break;
    case 16: *value = (s->output[p] & ~s->dir[p]) |
                      (s->input[p] & s->dir[p]); break;
    case 20: case 24: *value = s->rising[p]; break;
    case 28: case 32: *value = s->falling[p]; break;
    /* INTSTAT needs edge/event state, not a guessed zero value. */
    default: return false;
    }
    return true;
}
bool cdj_c6747_gpio_write(CdjC6747Gpio *s, uint32_t address,
                         uint64_t value, unsigned size, bool commit)
{
    unsigned p, o;
    if (size != 4 || value > UINT32_MAX) return false;
    if (address == 0x01e26008u) {
        if (value & ~255u) return false;
        if (commit) s->binten = value;
        return true;
    }
    if (!locate(address, &p, &o) || o == 16 || o == 36) return false;
    if (!commit) return true;
    /* Output latch retains writes independently of direction; only physical
     * drive would be gated by DIR and PINMUX. This interpretation of
     * "writes do not affect pins" is not board-measured. */
    switch (o) {
    case 0: s->dir[p] = value; break;
    case 4: s->output[p] = value; break;
    case 8: s->output[p] |= value; break;
    case 12: s->output[p] &= ~(uint32_t)value; break;
    case 20: s->rising[p] |= value; break;
    case 24: s->rising[p] &= ~(uint32_t)value; break;
    case 28: s->falling[p] |= value; break;
    case 32: s->falling[p] &= ~(uint32_t)value; break;
    }
    return true;
}
