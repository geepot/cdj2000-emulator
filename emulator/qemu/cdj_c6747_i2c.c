/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_i2c.h"
static bool locate(uint32_t address, unsigned *bank, unsigned *offset)
{
    uint32_t base = address & ~0xfffu;
    if ((base != 0x01c22000u && base != 0x01e28000u) || (address & 3)) return false;
    *bank = base == 0x01e28000u;
    *offset = address - base;
    return true;
}
void cdj_c6747_i2c_reset(CdjC6747I2c *s)
{
    *s = (CdjC6747I2c){0};
}
bool cdj_c6747_i2c_read(const CdjC6747I2c *s, uint32_t address, uint32_t *value)
{
    unsigned b, o;
    if (!locate(address, &b, &o)) return false;
    switch (o) {
    case 0x24: *value = s->mode[b]; break;
    case 0x48: *value = s->function[b]; break;
    case 0x4c: *value = s->direction[b]; break;
    case 0x54: *value = s->output[b]; break;
    /* PDSET/PDCLR reads are indeterminate (Tables 22-27/28). */
    default: return false;
    }
    return true;
}
bool cdj_c6747_i2c_write(CdjC6747I2c *s, uint32_t address,
                        uint64_t value, unsigned size, bool commit)
{
    unsigned b, o;
    if (size != 4 || value > UINT32_MAX || !locate(address, &b, &o)) return false;
    uint32_t mask;
    switch (o) {
    case 0x24:
        /* GPIO mode supplies constant ones internally to SCL/SDA (22.3.16).
         * Allow reset release only in idle slave mode (STT=0, no transfers).
         * FREE is debugger policy only. No ACK or bus-ready status supplied. */
        if ((value & 0x20) && (s->function[b] != 1 || (value & ~0x4020u)))
            return false;
        mask = 0xefff; break;
    case 0x48:
        if ((s->mode[b] & 0x20) && value != s->function[b]) return false;
        mask = 1; break;
    case 0x4c: case 0x54: case 0x58: case 0x5c: mask = 3; break;
    default: return false;
    }
    if (value & ~mask) return false;
    if (!commit) return true;
    switch (o) {
    case 0x24: s->mode[b] = value; break;
    case 0x48: s->function[b] = value; break;
    case 0x4c: s->direction[b] = value; break;
    case 0x54: s->output[b] = value; break;
    case 0x58: s->output[b] |= value; break;
    case 0x5c: s->output[b] &= ~(uint32_t)value; break;
    }
    return true;
}
