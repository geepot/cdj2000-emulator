/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_syscfg.h"
#define KEY0 0x83e70b13u
#define KEY1 0x95a4f1e0u
#define MSTPRI0 0x01c14110u
static const uint32_t mstpri_reset[3] = {
    0x44442222u, 0x44440000u, 0x54604404u
};
static const uint32_t mstpri_writable[3] = {
    0x00007700u, 0x00007777u, 0x77707707u
};
static bool pinmux_address(uint32_t address)
{
    return address >= CDJ_C6747_PINMUX0 && address <= CDJ_C6747_PINMUX0 + 19 * 4 &&
           !(address & 3);
}
void cdj_c6747_syscfg_reset(CdjC6747Syscfg *s)
{
    *s = (CdjC6747Syscfg){0};
    s->cfgchip[2] = 0xef00;
    s->cfgchip[3] = 0xff00;
}
bool cdj_c6747_syscfg_read(const CdjC6747Syscfg *s, uint32_t address,
                          uint32_t *value)
{
    if (pinmux_address(address)) {
        *value = s->pinmux[(address - CDJ_C6747_PINMUX0) / 4];
        return true;
    }
    if (address >= CDJ_C6747_CFGCHIP0 && address <= CDJ_C6747_CFGCHIP0 + 16 &&
        !(address & 3)) {
        unsigned index = (address - CDJ_C6747_CFGCHIP0) / 4;
        *value = index == 4 ? 0 : s->cfgchip[index];
        return true;
    }
    if (address != CDJ_C6747_KICK0 && address != CDJ_C6747_KICK1) return false;
    *value = s->kick[(address - CDJ_C6747_KICK0) / 4];
    return true;
}
bool cdj_c6747_syscfg_write(CdjC6747Syscfg *s, uint32_t address,
                           uint64_t value, unsigned size, bool commit)
{
    if (size == 4 && address >= CDJ_C6747_CFGCHIP0 &&
        address <= CDJ_C6747_CFGCHIP0 + 16 && !(address & 3)) {
        unsigned index = (address - CDJ_C6747_CFGCHIP0) / 4;
        uint32_t v = value;
        if (value > UINT32_MAX) return false;
        if (index == 0 && ((v & ~0x1fu) || (v & 3) == 3 || ((v >> 2) & 3) == 3))
            return false;
        if (index == 1) {
            if ((v & 0x6000) || (v & 15) > 8 || ((v >> 4) & 15) > 8 ||
                ((v >> 8) & 15) > 8 || ((v >> 17) & 31) > 0x12 ||
                ((v >> 22) & 31) > 0x12 || ((v >> 27) & 31) > 0x12)
                return false;
        }
        if (index == 2 && ((v & 0xfffc0000u) || (v & 15) > 9)) return false;
        if ((index == 3 || index == 4) &&
            ((v & ~0xff07u) || (v & 0xff00) != 0xff00)) return false;
        /* Locked writes complete on the bus but leave MMR state unchanged. */
        if (commit && s->unlocked) {
            if (index < 4) {
                if (index == 2) v &= 0xffff; /* PHY status bits 17:16 are read-only. */
                s->cfgchip[index] = v;
            } else {
                /* Pulse bookkeeping only. McASP AMUTE latch inputs are not modeled. */
                s->amute_clear_pulses |= v & 7;
            }
        }
        return true;
    }
    if (size == 4 && pinmux_address(address)) {
        /* PINMUX19 bits 31:4 are reserved zero (Table 10-41). Stop on
         * unsupported programming rather than inventing reserved readback. */
        if (address == CDJ_C6747_PINMUX0 + 19 * 4 && (value & 0xfffffff0u))
            return false;
        /* Protection is evaluated when the bus transfer commits, not when
         * the CPU queues it. Locked writes leave configuration unchanged. */
        if (commit && s->unlocked)
            s->pinmux[(address - CDJ_C6747_PINMUX0) / 4] = (uint32_t)value;
        return true;
    }
    if (size != 4 || (address != CDJ_C6747_KICK0 && address != CDJ_C6747_KICK1))
        return false;
    if (!commit) return true;
    unsigned index = (address - CDJ_C6747_KICK0) / 4;
    s->kick[index] = (uint32_t)value;
    if ((uint32_t)value != (index ? KEY1 : KEY0)) s->unlocked = false;
    else if (index && s->kick[0] == KEY0) s->unlocked = true;
    return true;
}
bool cdj_c6747_syscfg_pll_locked(const CdjC6747Syscfg *s)
{
    return (s->cfgchip[0] & 16) != 0;
}

void cdj_c6747_syscfg_priority_reset(CdjC6747SyscfgPriority *s)
{
    for (unsigned i = 0; i < 3; ++i) s->mstpri[i] = mstpri_reset[i];
}

bool cdj_c6747_syscfg_priority_valid(const CdjC6747SyscfgPriority *s)
{
    for (unsigned i = 0; i < 3; ++i)
        if ((s->mstpri[i] & ~mstpri_writable[i]) !=
            (mstpri_reset[i] & ~mstpri_writable[i]))
            return false;
    return true;
}

bool cdj_c6747_syscfg_priority_read(const CdjC6747SyscfgPriority *s,
                                   uint32_t address, uint32_t *value)
{
    if (address < MSTPRI0 || address > MSTPRI0 + 8 ||
        ((address - MSTPRI0) & 3)) return false;
    *value = s->mstpri[(address - MSTPRI0) / 4];
    return true;
}

bool cdj_c6747_syscfg_priority_write(CdjC6747SyscfgPriority *s,
                                    const CdjC6747Syscfg *syscfg,
                                    uint32_t address, uint64_t value,
                                    unsigned size, bool commit)
{
    if (size != 4 || value > UINT32_MAX || address < MSTPRI0 ||
        address > MSTPRI0 + 8 || ((address - MSTPRI0) & 3)) return false;
    unsigned index = (address - MSTPRI0) / 4;
    uint32_t v = value;
    if ((v & ~mstpri_writable[index]) !=
        (mstpri_reset[index] & ~mstpri_writable[index])) return false;
    /* MSTPRI registers share KICK protection with PINMUX/CFGCHIP. */
    if (commit && syscfg->unlocked) s->mstpri[index] = v;
    return true;
}
