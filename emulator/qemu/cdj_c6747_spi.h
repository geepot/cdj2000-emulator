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
    uint8_t receive_empty;
} CdjC6747Spi;

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

#endif
