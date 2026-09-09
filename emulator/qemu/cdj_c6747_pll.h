/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_PLL_H
#define CDJ_C6747_PLL_H
#include <stdbool.h>
#include <stdint.h>
/* SPRUH91D 7.4.3-15. Reset-held PLL configuration only. POR defaults
 * substitute for unknown ROM handoff values; clocks are not yet driven. */
typedef struct { uint32_t config[13]; } CdjC6747Pll;
void cdj_c6747_pll_reset(CdjC6747Pll *s);
bool cdj_c6747_pll_read(const CdjC6747Pll *s, uint32_t address, uint32_t *value);
bool cdj_c6747_pll_write(CdjC6747Pll *s, uint32_t address,
                        uint64_t value, unsigned size, bool commit);
#endif
