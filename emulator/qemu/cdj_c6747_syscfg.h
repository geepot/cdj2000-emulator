/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_SYSCFG_H
#define CDJ_C6747_SYSCFG_H
#include <stdbool.h>
#include <stdint.h>
#define CDJ_C6747_KICK0 0x01c14038u
#define CDJ_C6747_KICK1 0x01c1403cu
#define CDJ_C6747_PINMUX0 0x01c14120u
/* TI SPRUH91D sections 10.2.1.2, 10.5.5 and 10.5.10. Kick registers
 * and PINMUX0-19 configuration storage only; physical pin routing, other
 * SYSCFG registers and privilege faults remain unsupported. */
typedef struct {
    uint32_t kick[2], pinmux[20];
    bool unlocked;
} CdjC6747Syscfg;
void cdj_c6747_syscfg_reset(CdjC6747Syscfg *s);
bool cdj_c6747_syscfg_read(const CdjC6747Syscfg *s, uint32_t address,
                          uint32_t *value);
/* Check phase validates mapping/width only; the E3 commit applies key state.
 * These registers have side-effect-free readback. Caller must be supervisor. */
bool cdj_c6747_syscfg_write(CdjC6747Syscfg *s, uint32_t address,
                           uint64_t value, unsigned size, bool commit);
#endif
