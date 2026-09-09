/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "cdj_c6747_spi.h"

#define SPI_GCR1_WRITE_MASK 0x01010103u
#define SPI_INTERRUPT_WRITE_MASK 0x0101035fu
#define SPI_LEVEL_WRITE_MASK 0x0000035fu
#define SPI_FLAG_W1C_MASK 0x0000015fu
#define SPI_FLAG_RESET_RESERVED 0x01000000u
#define SPI_DAT1_WRITE_MASK 0x1701ffffu
#define SPI_FORMAT_WRITE_MASK 0x3ff7ff1fu
#define SPI_RX_STATUS_MASK 0x5f000000u

static CdjC6747Spi *decode(CdjC6747Spi spis[CDJ_C6747_SPI_COUNT],
                           uint32_t address, uint32_t *offset)
{
    uint32_t base;
    unsigned index;
    if (address >= CDJ_C6747_SPI0_BASE && address < CDJ_C6747_SPI0_BASE + 0x1000) {
        base = CDJ_C6747_SPI0_BASE;
        index = 0;
    } else if (address >= CDJ_C6747_SPI1_BASE &&
               address < CDJ_C6747_SPI1_BASE + 0x1000) {
        base = CDJ_C6747_SPI1_BASE;
        index = 1;
    } else {
        return NULL;
    }
    *offset = address - base;
    return &spis[index];
}

static void reset_one(CdjC6747Spi *s)
{
    uint32_t pin_input = s->pin_input;
    uint32_t pin_input_valid = s->pin_input_valid;
    memset(s, 0, sizeof(*s));
    s->chip_select_default = 0xff;
    s->receive_empty = true;
    s->pin_input = pin_input;
    s->pin_input_valid = pin_input_valid;
}

void cdj_c6747_spis_reset(CdjC6747Spi spis[CDJ_C6747_SPI_COUNT])
{
    memset(spis, 0, sizeof(*spis) * CDJ_C6747_SPI_COUNT);
    for (unsigned i = 0; i < CDJ_C6747_SPI_COUNT; ++i) reset_one(&spis[i]);
}

void cdj_c6747_spi_set_pins(CdjC6747Spi spis[CDJ_C6747_SPI_COUNT],
                            unsigned index, uint32_t valid, uint32_t value)
{
    if (!spis || index >= CDJ_C6747_SPI_COUNT) return;
    valid &= CDJ_C6747_SPI_PIN_MASK;
    spis[index].pin_input = (spis[index].pin_input & ~valid) | (value & valid);
    spis[index].pin_input_valid |= valid;
}

static bool read_pins(const CdjC6747Spi *s, uint32_t *value)
{
    if ((s->pin_input_valid & CDJ_C6747_SPI_PIN_MASK) != CDJ_C6747_SPI_PIN_MASK)
        return false;
    *value = s->pin_input & CDJ_C6747_SPI_PIN_MASK;
    return true;
}

static uint32_t interrupt_vector(CdjC6747Spi *s)
{
    uint32_t pending = s->flags & s->interrupt_enable & s->interrupt_level;
    if (pending & 0x1f) return 0x11u << 1;
    if (pending & (1u << 6)) {
        s->flags &= ~(1u << 6);
        return 0x13u << 1;
    }
    if (pending & (1u << 8)) {
        s->flags &= ~(1u << 8);
        s->receive_empty = true;
        return 0x12u << 1;
    }
    if (pending & (1u << 9)) return 0x14u << 1;
    return 0;
}

bool cdj_c6747_spis_read(CdjC6747Spi spis[CDJ_C6747_SPI_COUNT],
                         uint32_t address, uint32_t *value)
{
    uint32_t offset;
    CdjC6747Spi *s = decode(spis, address, &offset);
    if (!s || !value || (offset & 3)) return false;
    switch (offset) {
    case 0x00: *value = s->gcr0; break;
    case 0x04: *value = s->gcr1; break;
    case 0x08: *value = s->interrupt_enable; break;
    case 0x0c: *value = s->interrupt_level; break;
    case 0x10: *value = s->flags | SPI_FLAG_RESET_RESERVED; break;
    case 0x14: *value = s->pin_function; break;
    case 0x18: *value = s->pin_direction; break;
    case 0x1c: return read_pins(s, value);
    case 0x20: *value = s->pin_output; break;
    case 0x24:
    case 0x28: return read_pins(s, value);
    case 0x38: *value = s->dat0; break;
    case 0x3c: *value = s->dat1; break;
    case 0x40:
        *value = (s->receive_empty ? UINT32_C(0x80000000) : 0) |
                 (s->receive_status & SPI_RX_STATUS_MASK) |
                 (s->receive_data & 0xffff);
        s->receive_empty = true;
        s->receive_status = 0;
        s->flags &= ~(1u << 8);
        break;
    case 0x44: *value = s->receive_data & 0xffff; break;
    case 0x48: *value = s->delay; break;
    case 0x4c: *value = s->chip_select_default; break;
    case 0x50:
    case 0x54:
    case 0x58:
    case 0x5c: *value = s->format[(offset - 0x50) / 4]; break;
    case 0x64: *value = interrupt_vector(s); break;
    default: return false;
    }
    return true;
}

static void disable(CdjC6747Spi *s)
{
    s->dat0 = 0;
    s->dat1 &= 0xffff0000u;
    s->flags = 0;
    s->receive_data = 0;
    s->receive_status = 0;
    s->receive_empty = true;
}

bool cdj_c6747_spis_write(CdjC6747Spi spis[CDJ_C6747_SPI_COUNT],
                          uint32_t address, uint64_t value, unsigned size,
                          bool commit)
{
    uint32_t offset;
    CdjC6747Spi *s = decode(spis, address, &offset);
    if (!s || size != 4 || value > UINT32_MAX || (offset & 3)) return false;
    uint32_t word = value;
    switch (offset) {
    case 0x00:
        if (commit) {
            if (word & 1) s->gcr0 = 1;
            else reset_one(s);
        }
        break;
    case 0x04:
        if ((word & 3) == 1 || (word & 3) == 2) return false;
        if (commit) {
            s->gcr1 = word & SPI_GCR1_WRITE_MASK;
            if (!(s->gcr1 & (1u << 24))) disable(s);
        }
        break;
    case 0x08:
        if (commit) s->interrupt_enable = word & SPI_INTERRUPT_WRITE_MASK;
        break;
    case 0x0c:
        if (commit) s->interrupt_level = word & SPI_LEVEL_WRITE_MASK;
        break;
    case 0x10:
        if (commit) {
            s->flags &= ~(word & SPI_FLAG_W1C_MASK);
            if (word & (1u << 8)) s->receive_empty = true;
        }
        break;
    case 0x14:
        if (commit) s->pin_function = word & CDJ_C6747_SPI_PIN_MASK;
        break;
    case 0x18:
        if (commit) s->pin_direction = word & CDJ_C6747_SPI_PIN_MASK;
        break;
    case 0x20:
    case 0x24:
    case 0x28: {
        uint32_t outputs = ~s->pin_function & s->pin_direction & CDJ_C6747_SPI_PIN_MASK;
        if (commit) {
            if (offset == 0x20) s->pin_output = (s->pin_output & ~outputs) | (word & outputs);
            else if (offset == 0x24) s->pin_output |= word & outputs;
            else s->pin_output &= ~(word & outputs);
            s->pin_input = (s->pin_input & ~outputs) | (s->pin_output & outputs);
            s->pin_input_valid |= outputs;
        }
        break;
    }
    case 0x38:
    case 0x3c:
        /* Configuration fields are writable while disabled, but an enabled
         * word write starts a transfer whose slave response/timing is absent. */
        if (s->gcr1 & (1u << 24)) return false;
        if (commit) {
            if (offset == 0x38) s->dat0 = 0;
            else s->dat1 = word & (SPI_DAT1_WRITE_MASK & 0xffff0000u);
            s->flags &= ~(1u << 9);
        }
        break;
    case 0x48: if (commit) s->delay = word; break;
    case 0x4c: if (commit) s->chip_select_default = word & 0xff; break;
    case 0x50:
    case 0x54:
    case 0x58:
    case 0x5c:
        if (commit) s->format[(offset - 0x50) / 4] = word & SPI_FORMAT_WRITE_MASK;
        break;
    case 0x1c:
    case 0x40:
    case 0x44:
    case 0x64:
    default:
        return false;
    }
    return true;
}
