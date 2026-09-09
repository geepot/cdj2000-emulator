/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_SYSCFG_H
#define CDJ_C6747_SYSCFG_H
#include <stdbool.h>
#include <stdint.h>
#define CDJ_C6747_KICK0 0x01c14038u
#define CDJ_C6747_KICK1 0x01c1403cu
#define CDJ_C6747_PINMUX0 0x01c14120u
#define CDJ_C6747_CFGCHIP0 0x01c1417cu
/* TI SPRUH91D sections 10.2.1.2, 10.5.5 and 10.5.10. Kick registers
 * PINMUX0-19 and CFGCHIP0-4. Physical routing/clock consumers, AMUTE latches,
 * other SYSCFG registers and privilege faults remain unsupported. */
typedef struct {
    uint32_t kick[2], pinmux[20], cfgchip[4], amute_clear_pulses;
    bool unlocked;
} CdjC6747Syscfg;
/* Kept out of CdjC6747Syscfg because that type is embedded before the CPU and
 * peripherals in schema-1 through schema-6 checkpoints. */
typedef struct { uint32_t mstpri[3]; } CdjC6747SyscfgPriority;
void cdj_c6747_syscfg_reset(CdjC6747Syscfg *s);
bool cdj_c6747_syscfg_read(const CdjC6747Syscfg *s, uint32_t address,
                          uint32_t *value);
/* Check phase validates mapping/width only; the E3 commit applies key state.
 * These registers have side-effect-free readback. Caller must be supervisor. */
bool cdj_c6747_syscfg_write(CdjC6747Syscfg *s, uint32_t address,
                           uint64_t value, unsigned size, bool commit);
bool cdj_c6747_syscfg_pll_locked(const CdjC6747Syscfg *s);
void cdj_c6747_syscfg_priority_reset(CdjC6747SyscfgPriority *s);
bool cdj_c6747_syscfg_priority_valid(const CdjC6747SyscfgPriority *s);
bool cdj_c6747_syscfg_priority_read(const CdjC6747SyscfgPriority *s,
                                   uint32_t address, uint32_t *value);
bool cdj_c6747_syscfg_priority_write(CdjC6747SyscfgPriority *s,
                                    const CdjC6747Syscfg *syscfg,
                                    uint32_t address, uint64_t value,
                                    unsigned size, bool commit);
#endif
