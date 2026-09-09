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
#define SPI_RX_OVERRUN_STATUS (UINT32_C(1) << 30)
#define SPI_TX_FLAG (UINT32_C(1) << 9)
#define SPI_RX_FLAG (UINT32_C(1) << 8)
#define SPI_OVERRUN_FLAG (UINT32_C(1) << 6)
#define SPI_WM8740_FUNCTION_PINS 0x00000601u
#define SPI_WM8740_STRICT_PINS 0x00000e01u
#define SPI_WM8740_STRICT_GCR1 0x01000003u
#define SPI_WM8740_STRICT_FORMAT 0x00021810u
#define SPI_WM8740_STRICT_DELAY 0x02020408u

enum {
    SPI_TRANSFER_IDLE,
    SPI_TRANSFER_C2T,
    SPI_TRANSFER_SHIFT,
    SPI_TRANSFER_T2C,
    SPI_TRANSFER_GAP,
};

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

void cdj_wm8740_reset(CdjWm8740 *dac)
{
    if (!dac) return;
    memset(dac, 0, sizeof(*dac));
    dac->program[0] = dac->program[1] = 0xff;
    dac->active_attenuation[0] = dac->active_attenuation[1] = 0xff;
}

bool cdj_wm8740_valid(const CdjWm8740 *dac)
{
    if (!dac || dac->program[0] > 0x1ff || dac->program[1] > 0x1ff ||
        dac->program[2] > 0x1ff || (dac->program[3] & ~0x1dfu) ||
        (dac->program[4] & ~0x70u) || dac->register4_unlocked > 1)
        return false;
    return true;
}

static bool wm8740_write(CdjWm8740 *dac, uint16_t word, bool commit)
{
    unsigned address = word >> 9 & 7;
    uint16_t data = word & 0x1ff;
    if (!dac || (address > 3 && address != 6)) return false;
    if (!commit) return true;
    dac->last_word = word;
    ++dac->transfers;
    if (address <= 1) {
        dac->program[address] = data;
        if (data & 0x100) {
            dac->active_attenuation[0] = dac->program[0] & 0xff;
            dac->active_attenuation[1] = dac->program[1] & 0xff;
        }
    } else if (address == 2) {
        dac->program[2] = data;
        if ((data & 0x1e0) == 0x1e0) dac->register4_unlocked = true;
    } else if (address == 3) {
        dac->program[3] = data & 0x1df;
    } else if (dac->register4_unlocked) {
        dac->program[4] = data & 0x70;
    }
    return true;
}

static bool wm8740_word_valid(uint32_t control)
{
    unsigned address = (control >> 9) & 7;
    return !(control & 0xffff0000u) && (address <= 3 || address == 6);
}

void cdj_c6747_spi_transfer_reset(CdjC6747SpiTransfer *transfer)
{
    if (transfer) memset(transfer, 0, sizeof(*transfer));
}

bool cdj_c6747_spi_transfer_active(const CdjC6747SpiTransfer *transfer)
{
    return transfer && (transfer->phase != SPI_TRANSFER_IDLE ||
                        transfer->queued_valid || transfer->tx_full);
}

static bool transfer_word_valid(uint32_t control, uint32_t format,
                                uint32_t delay)
{
    return wm8740_word_valid(control) && format == SPI_WM8740_STRICT_FORMAT &&
           delay == SPI_WM8740_STRICT_DELAY;
}

bool cdj_c6747_spi_transfer_valid(const CdjC6747SpiTransfer *transfer)
{
    if (!transfer || transfer->phase > SPI_TRANSFER_GAP ||
        transfer->queued_valid > 1 || transfer->tx_full > 1 ||
        transfer->previous_cshold > 1 || transfer->fault > 1 ||
        transfer->reserved[0] || transfer->reserved[1] || transfer->reserved[2])
        return false;
    bool active = transfer->phase != SPI_TRANSFER_IDLE;
    bool shifting_word = transfer->phase >= SPI_TRANSFER_C2T &&
                         transfer->phase <= SPI_TRANSFER_T2C;
    if (active != (transfer->half_ticks_remaining != 0)) return false;
    if (shifting_word != transfer_word_valid(transfer->active_control,
                                             transfer->active_format,
                                             transfer->active_delay)) return false;
    if (!shifting_word && (transfer->active_control || transfer->active_format ||
                           transfer->active_delay)) return false;
    if ((bool)transfer->queued_valid !=
        transfer_word_valid(transfer->queued_control, transfer->queued_format,
                            transfer->queued_delay)) return false;
    if (!transfer->queued_valid &&
        (transfer->queued_control || transfer->queued_format ||
         transfer->queued_delay || transfer->tx_full)) return false;
    if (transfer->queued_valid && !active) return false;
    /* TXBUF can only remain full before the active shift finishes.  A word
     * written during T2C copies directly into the now-empty shift register. */
    if (transfer->tx_full && transfer->phase >= SPI_TRANSFER_T2C) return false;
    return true;
}

bool cdj_c6747_spi_wm8740_timed_mapped(uint32_t address)
{
    return address >= CDJ_C6747_SPI1_BASE &&
           address < CDJ_C6747_SPI1_BASE + 0x1000;
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

static void consume_receive(CdjC6747Spi *s)
{
    if (s->receive_buffer_full) {
        s->receive_data = s->receive_buffer_data;
        s->receive_buffer_full = false;
        s->receive_empty = false;
        s->flags |= SPI_RX_FLAG;
    } else {
        s->receive_empty = true;
        s->flags &= ~SPI_RX_FLAG;
    }
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
        consume_receive(s);
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
        consume_receive(s);
        /* RXOVR is sticky across SPIBUF reads (SPRUH91D 27.3.14); the other
         * per-character status and RXINT clear with the consumed word. */
        s->receive_status &= SPI_RX_OVERRUN_STATUS;
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

bool cdj_c6747_spi_wm8740_read_timed(
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT], CdjC6747SpiTransfer *transfer,
    uint32_t address, uint32_t *value)
{
    if (!cdj_c6747_spi_wm8740_timed_mapped(address) ||
        !cdj_c6747_spi_transfer_valid(transfer) || transfer->fault ||
        !cdj_c6747_spis_read(spis, address, value))
        return false;
    if (address - CDJ_C6747_SPI1_BASE == 0x40 && transfer->tx_full)
        *value |= UINT32_C(1) << 29;
    return true;
}

static void discard_transfer(CdjC6747SpiTransfer *transfer)
{
    /* SYSCLK2 continues through a peripheral reset.  Preserve the board-owned
     * fractional clock accumulator while discarding every pending bus effect. */
    uint32_t clock_phase = transfer->clock_phase;
    cdj_c6747_spi_transfer_reset(transfer);
    transfer->clock_phase = clock_phase;
}

static unsigned shift_half_ticks(uint32_t format)
{
    unsigned bits = format & 0x1f;
    unsigned prescale = (format >> 8) & 0xff;
    /* PHASE=0: the first transmit edge is the C2T boundary.  The final sample
     * edge for N bits follows after 2*N-1 half serial-clock periods. */
    return (2 * bits - 1) * (prescale + 1);
}

static unsigned c2t_half_ticks(uint32_t delay)
{
    unsigned c2t = delay >> 24;
    return c2t ? 2 * (c2t + 2) : 0;
}

static unsigned t2c_half_ticks(uint32_t format, uint32_t delay)
{
    unsigned t2c = (delay >> 16) & 0xff;
    unsigned prescale = (format >> 8) & 0xff;
    /* PHASE=0 adds one half serial-clock period after the final receive edge. */
    return (t2c ? 2 * (t2c + 1) : 0) + prescale + 1;
}

static void start_transfer(CdjC6747SpiTransfer *transfer, uint32_t control,
                           uint32_t format, uint32_t delay)
{
    transfer->active_control = control;
    transfer->active_format = format;
    transfer->active_delay = delay;
    transfer->phase = SPI_TRANSFER_C2T;
    transfer->half_ticks_remaining = c2t_half_ticks(delay);
    if (!transfer->half_ticks_remaining) {
        transfer->phase = SPI_TRANSFER_SHIFT;
        transfer->half_ticks_remaining = shift_half_ticks(format);
    }
}

static bool strict_wm8740_configuration(const CdjC6747Spi *spi,
                                         const CdjWm8740 *dac,
                                         uint32_t control)
{
    return spi && cdj_wm8740_valid(dac) &&
           spi->gcr1 == SPI_WM8740_STRICT_GCR1 &&
           spi->pin_function == SPI_WM8740_STRICT_PINS &&
           !spi->interrupt_enable && !spi->interrupt_level &&
           spi->chip_select_default == 0xff &&
           spi->format[0] == SPI_WM8740_STRICT_FORMAT &&
           spi->delay == SPI_WM8740_STRICT_DELAY &&
           wm8740_word_valid(control);
}

bool cdj_c6747_spi_wm8740_write_timed(
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT], CdjWm8740 *dac,
    CdjC6747SpiTransfer *transfer, uint32_t address, uint64_t value,
    unsigned size, bool commit)
{
    if (!cdj_c6747_spi_wm8740_timed_mapped(address) || !spis || !dac ||
        !cdj_c6747_spi_transfer_valid(transfer) || transfer->fault ||
        size != 4 || value > UINT32_MAX || (address & 3))
        return false;
    CdjC6747Spi *spi = &spis[1];
    uint32_t offset = address - CDJ_C6747_SPI1_BASE;
    uint32_t word = value;
    bool reset = offset == 0 && !(word & 1);
    bool disable = offset == 4 && !(word & (UINT32_C(1) << 24));

    if (cdj_c6747_spi_transfer_active(transfer) && !reset && !disable &&
        offset != 0x10 && offset != 0x3c)
        return false;

    if (offset != 0x3c || !(spi->gcr1 & (UINT32_C(1) << 24))) {
        if (!cdj_c6747_spis_write(spis, address, value, size, commit))
            return false;
        if (commit && (reset || disable)) discard_transfer(transfer);
        /* A disabled control write is how software initializes the otherwise
         * undefined previous CSHOLD latch before enabling the master. */
        if (commit && offset == 0x3c)
            transfer->previous_cshold = (word >> 28) & 1;
        return true;
    }

    /* Strict timing intentionally covers only the reached 4-pin, CS0,
     * 16-bit WM8740 mode.  Every mutable choice is validated before E3. */
    if (!strict_wm8740_configuration(spi, dac, word) ||
        transfer->previous_cshold || transfer->queued_valid ||
        !wm8740_write(dac, word, false))
        return false;
    if (!commit) return true;

    spi->dat1 = word & SPI_DAT1_WRITE_MASK;
    spi->flags &= ~SPI_TX_FLAG;
    if (!cdj_c6747_spi_transfer_active(transfer)) {
        start_transfer(transfer, word, spi->format[0], spi->delay);
        /* An empty shift register accepts the word directly, leaving TXBUF
         * empty again in the same transaction. */
        spi->flags |= SPI_TX_FLAG;
    } else {
        transfer->queued_control = word;
        transfer->queued_format = spi->format[0];
        transfer->queued_delay = spi->delay;
        transfer->queued_valid = true;
        if (transfer->phase == SPI_TRANSFER_T2C ||
            transfer->phase == SPI_TRANSFER_GAP) {
            /* The completed word vacated the shift register already. */
            transfer->tx_full = false;
            spi->flags |= SPI_TX_FLAG;
        } else {
            transfer->tx_full = true;
        }
    }
    return true;
}

static void receive_pulled_up_word(CdjC6747Spi *spi)
{
    if (spi->receive_empty) {
        spi->receive_data = 0xffff;
        spi->receive_status &= SPI_RX_OVERRUN_STATUS;
        spi->receive_empty = false;
        spi->flags |= SPI_RX_FLAG;
    } else if (!spi->receive_buffer_full) {
        spi->receive_buffer_data = 0xffff;
        spi->receive_buffer_full = true;
    } else {
        spi->flags |= SPI_OVERRUN_FLAG;
        spi->receive_status |= SPI_RX_OVERRUN_STATUS;
    }
}

static bool finish_chip_select(CdjC6747Spi *spi, CdjWm8740 *dac,
                               CdjC6747SpiTransfer *transfer)
{
    if (!wm8740_write(dac, transfer->active_control, true)) return false;
    transfer->previous_cshold = (transfer->active_control >> 28) & 1;
    transfer->active_control = transfer->active_format = transfer->active_delay = 0;
    /* CSHOLD=0/WDEL=0 still requires CS to remain inactive for at least two
     * module clocks before another transaction (SPRUH91D Table 27-21).
     * C2T cannot provide this gap because it starts after the next CS edge. */
    transfer->phase = SPI_TRANSFER_GAP;
    transfer->half_ticks_remaining = 4;
    if (!transfer->queued_valid)
        spi->flags |= SPI_TX_FLAG;
    return true;
}

static void finish_gap(CdjC6747Spi *spi, CdjC6747SpiTransfer *transfer)
{
    if (transfer->queued_valid) {
        uint32_t control = transfer->queued_control;
        uint32_t format = transfer->queued_format;
        uint32_t delay = transfer->queued_delay;
        transfer->queued_control = transfer->queued_format = transfer->queued_delay = 0;
        transfer->queued_valid = transfer->tx_full = false;
        start_transfer(transfer, control, format, delay);
        spi->flags |= SPI_TX_FLAG;
    } else {
        transfer->phase = SPI_TRANSFER_IDLE;
        transfer->half_ticks_remaining = 0;
    }
}

bool cdj_c6747_spi_wm8740_advance(
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT], CdjWm8740 *dac,
    CdjC6747SpiTransfer *transfer, unsigned half_module_ticks)
{
    if (!spis || !dac || !cdj_wm8740_valid(dac) ||
        !cdj_c6747_spi_transfer_valid(transfer) || transfer->fault) {
        if (transfer) transfer->fault = true;
        return false;
    }
    CdjC6747Spi *spi = &spis[1];
    while (half_module_ticks && transfer->phase != SPI_TRANSFER_IDLE) {
        if (half_module_ticks < transfer->half_ticks_remaining) {
            transfer->half_ticks_remaining -= half_module_ticks;
            break;
        }
        half_module_ticks -= transfer->half_ticks_remaining;
        transfer->half_ticks_remaining = 0;
        if (transfer->phase == SPI_TRANSFER_C2T) {
            transfer->phase = SPI_TRANSFER_SHIFT;
            transfer->half_ticks_remaining = shift_half_ticks(transfer->active_format);
        } else if (transfer->phase == SPI_TRANSFER_SHIFT) {
            receive_pulled_up_word(spi);
            if (transfer->queued_valid) {
                transfer->tx_full = false;
                spi->flags |= SPI_TX_FLAG;
            }
            transfer->phase = SPI_TRANSFER_T2C;
            transfer->half_ticks_remaining =
                t2c_half_ticks(transfer->active_format, transfer->active_delay);
        } else if (transfer->phase == SPI_TRANSFER_T2C) {
            if (!finish_chip_select(spi, dac, transfer)) {
                transfer->fault = true;
                return false;
            }
        } else if (transfer->phase == SPI_TRANSFER_GAP) {
            finish_gap(spi, transfer);
        } else {
            transfer->fault = true;
            return false;
        }
    }
    return true;
}

bool cdj_c6747_spis_write_wm8740(
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT], CdjWm8740 *dac,
    uint32_t address, uint64_t value, unsigned size,
    bool functional_timing, bool commit)
{
    uint32_t offset;
    CdjC6747Spi *s = decode(spis, address, &offset);
    if (!s || s != &spis[1] || (offset != 0x38 && offset != 0x3c) ||
        size != 4 || value > UINT32_MAX || !functional_timing)
        return false;
    uint32_t word = value;
    if (!(s->gcr1 & (1u << 24)) || (s->gcr1 & 3) != 3 ||
        (s->gcr1 & (1u << 8)) ||
        (s->pin_function & SPI_WM8740_FUNCTION_PINS) !=
            SPI_WM8740_FUNCTION_PINS)
        return false;
    uint32_t control = offset == 0x3c ? word : s->dat1;
    unsigned format_index = control >> 24 & 3;
    uint32_t format = s->format[format_index];
    if ((control & ((1u << 28) | (1u << 16))) ||
        (format & 0x1f) != 16 ||
        (format & ((1u << 22) | (1u << 21) | (1u << 20))) ||
        !wm8740_write(dac, word, false))
        return false;
    if (!commit) return true;

    if (offset == 0x38) s->dat0 = word & 0xffff;
    else s->dat1 = word & SPI_DAT1_WRITE_MASK;
    s->flags &= ~SPI_TX_FLAG;
    /* Functional breadth mode collapses the documented shift and chip-select
     * delays to this commit.  TXBUF is empty again, while the completed word
     * fills SPIBUF and raises the architectural TX/RX flags.  P5/SOMI is NC
     * on the board and has an internal pull-up in SPRS377F, hence 0xffff. */
    s->flags |= SPI_TX_FLAG;
    if (s->receive_empty) {
        s->flags |= SPI_RX_FLAG;
        s->receive_data = 0xffff;
        s->receive_status &= SPI_RX_OVERRUN_STATUS;
        s->receive_empty = false;
    } else if (!s->receive_buffer_full) {
        s->receive_buffer_data = 0xffff;
        s->receive_buffer_full = true;
    } else {
        /* SPRUH91D 27.3.14: SPIBUF is not overwritten; the newly completed
         * character is lost only after both SPIBUF and RXBUF are full. */
        s->flags |= SPI_OVERRUN_FLAG;
        s->receive_status |= SPI_RX_OVERRUN_STATUS;
    }
    return wm8740_write(dac, word, true);
}

static void disable(CdjC6747Spi *s)
{
    s->dat0 = 0;
    s->dat1 &= 0xffff0000u;
    s->flags = 0;
    s->receive_data = 0;
    s->receive_status = 0;
    s->receive_empty = true;
    s->receive_buffer_full = false;
    s->receive_buffer_data = 0;
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
            if (word & (1u << 8)) consume_receive(s);
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
