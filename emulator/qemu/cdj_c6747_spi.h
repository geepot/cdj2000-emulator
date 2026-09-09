/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_SPI_H
#define CDJ_C6747_SPI_H

#include <stdbool.h>
#include <stdint.h>

#define CDJ_C6747_SPI_COUNT 2u
#define CDJ_C6747_SPI0_BASE 0x01c41000u
#define CDJ_C6747_SPI1_BASE 0x01e12000u
#define CDJ_C6747_SPI_PIN_MASK 0x00000f01u

/* SPRUH91D chapter 27 register state. An enabled write to SPIDAT0/1 needs a
 * physical slave (or a separately timed loopback engine) and therefore stays
 * fail-closed in this register-only model. External pin values are likewise
 * unavailable until supplied explicitly through cdj_c6747_spi_set_pins(). */
typedef struct {
    uint32_t gcr0, gcr1, interrupt_enable, interrupt_level, flags;
    uint32_t pin_function, pin_direction, pin_input, pin_input_valid, pin_output;
    uint32_t dat0, dat1, receive_data, receive_status;
    uint32_t delay, chip_select_default, format[4];
    /* These four trailing bytes occupy the structure's original padding, so
     * schema-8 native SPI state keeps the same ABI size. */
    uint8_t receive_empty, receive_buffer_full;
    uint16_t receive_buffer_data;
} CdjC6747Spi;

/* IC505 on the CDJ-2000NXS DSP board.  The WM8740 software interface is a
 * write-only 16-bit shift register: bits 11:9 select a program register and
 * bits 8:0 carry its data.  The state below is the genuine latched control
 * state, not an audio-rendering substitute. */
typedef struct {
    uint16_t program[5];
    uint16_t last_word;
    uint64_t transfers;
    uint8_t active_attenuation[2];
    uint8_t register4_unlocked;
} CdjWm8740;

/* Serializable timing state for the schematic-confirmed SPI1/WM8740 path.
 * Time is counted in half SPI-module-clock periods so PHASE-dependent half
 * serial-clock edges remain integral for the reached even SYSCLK1/SYSCLK2
 * ratio.  The board clock adapter owns clock_phase; this core only consumes
 * already-derived half-module ticks.  Keep this separate from CdjC6747Spi so
 * existing schema-5..9 SPI state retains its native ABI. */
typedef struct {
    uint32_t clock_phase, half_ticks_remaining;
    uint32_t active_control, active_format, active_delay;
    uint32_t queued_control, queued_format, queued_delay;
    uint8_t phase, queued_valid, tx_full, previous_cshold, fault;
    uint8_t reserved[3];
} CdjC6747SpiTransfer;

void cdj_c6747_spis_reset(CdjC6747Spi spis[CDJ_C6747_SPI_COUNT]);
void cdj_c6747_spi_set_pins(CdjC6747Spi spis[CDJ_C6747_SPI_COUNT],
                            unsigned index, uint32_t valid, uint32_t value);
bool cdj_c6747_spis_read(CdjC6747Spi spis[CDJ_C6747_SPI_COUNT],
                         uint32_t address, uint32_t *value);
/* Check phase is side-effect free. Reserved offsets, writes to receive/vector
 * registers, non-word accesses, and transfers without an endpoint fail closed. */
bool cdj_c6747_spis_write(CdjC6747Spi spis[CDJ_C6747_SPI_COUNT],
                          uint32_t address, uint64_t value, unsigned size,
                          bool commit);
/* Breadth-first endpoint used only when functional timing is explicitly
 * enabled.  The board routes SPI1 CS0/CLK/SIMO to the write-only WM8740 and
 * leaves SOMI unconnected on a documented internal pull-up.  Transfer timing
 * is collapsed to the committing write; all configuration and status effects
 * are architectural, and unsupported topologies still fail closed. */
bool cdj_c6747_spis_write_wm8740(
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT], CdjWm8740 *dac,
    uint32_t address, uint64_t value, unsigned size,
    bool functional_timing, bool commit);
void cdj_c6747_spi_transfer_reset(CdjC6747SpiTransfer *transfer);
bool cdj_c6747_spi_transfer_valid(const CdjC6747SpiTransfer *transfer);
bool cdj_c6747_spi_transfer_active(const CdjC6747SpiTransfer *transfer);
/* The timed wrapper owns the complete SPI1 window.  Callers must not fall
 * through to the register-only writer after a mapped rejection, otherwise a
 * busy configuration mutation could evade fail-closed validation. */
bool cdj_c6747_spi_wm8740_timed_mapped(uint32_t address);
bool cdj_c6747_spi_wm8740_read_timed(
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT], CdjC6747SpiTransfer *transfer,
    uint32_t address, uint32_t *value);
bool cdj_c6747_spi_wm8740_write_timed(
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT], CdjWm8740 *dac,
    CdjC6747SpiTransfer *transfer, uint32_t address, uint64_t value,
    unsigned size, bool commit);
/* Advance by elapsed half SPI-module-clock periods.  All write acceptance is
 * checked up front, so false indicates corrupt serialized state rather than a
 * late external-bus rejection. */
bool cdj_c6747_spi_wm8740_advance(
    CdjC6747Spi spis[CDJ_C6747_SPI_COUNT], CdjWm8740 *dac,
    CdjC6747SpiTransfer *transfer, unsigned half_module_ticks);
void cdj_wm8740_reset(CdjWm8740 *dac);
bool cdj_wm8740_valid(const CdjWm8740 *dac);

#endif
