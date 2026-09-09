/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_GPIO_H
#define CDJ_C6747_GPIO_H
#include <stdbool.h>
#include <stdint.h>
/* C6747: eight 16-bit banks. Configuration latches only; no external
 * input, pinmux routing, edge detection, DSP/EDMA interrupt delivery yet. */
typedef struct {
    uint32_t dir[4], output[4], rising[4], falling[4];
    uint32_t binten;
} CdjC6747Gpio;
void cdj_c6747_gpio_reset(CdjC6747Gpio *s);
bool cdj_c6747_gpio_read(const CdjC6747Gpio *s, uint32_t address, uint32_t *value);
bool cdj_c6747_gpio_write(CdjC6747Gpio *s, uint32_t address,
                         uint64_t value, unsigned size, bool commit);
#endif
