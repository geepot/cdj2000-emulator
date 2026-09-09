/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_I2C_H
#define CDJ_C6747_I2C_H
#include <stdbool.h>
#include <stdint.h>
/* Reset-held configuration and idle GPIO mode, SPRUH91D 22.3.9, 22.3.16-21.
 * Bus transfers, input values and interrupts remain unsupported. */
typedef struct {
    uint32_t mode[2], function[2], direction[2], output[2];
} CdjC6747I2c;
void cdj_c6747_i2c_reset(CdjC6747I2c *s);
bool cdj_c6747_i2c_read(const CdjC6747I2c *s, uint32_t address, uint32_t *value);
bool cdj_c6747_i2c_write(CdjC6747I2c *s, uint32_t address,
                        uint64_t value, unsigned size, bool commit);
#endif
