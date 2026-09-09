/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_HPI_H
#define CDJ_C6747_HPI_H
#include <stdbool.h>
#include <stdint.h>
#define CDJ_C6747_HPI_BASE 0x01e10000u
#define CDJ_C6747_HPIC (CDJ_C6747_HPI_BASE + 0x30u)
/* Shared host/DSP HPIC state, SPRUH91D 21.2.9 and 21.3.8. FIFO/HRDY,
 * CPU interrupts, GPIO mode and HPIA ownership are outside this model. */
typedef struct {
    bool hpirst, hwob, dual_hpia, hpiasel, dspint, hint;
} CdjC6747Hpi;
void cdj_c6747_hpi_reset(CdjC6747Hpi *s);
void cdj_c6747_hpi_rom_boot_ready(CdjC6747Hpi *s);
uint32_t cdj_c6747_hpi_host_read(const CdjC6747Hpi *s);
void cdj_c6747_hpi_host_write(CdjC6747Hpi *s, uint32_t value);
bool cdj_c6747_hpi_cpu_read(const CdjC6747Hpi *s, uint32_t address,
                           uint32_t *value);
bool cdj_c6747_hpi_cpu_write(CdjC6747Hpi *s, uint32_t address,
                            uint64_t value, unsigned size, bool commit);
#endif
