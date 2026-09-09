/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_syscfg.h"
#define KEY0 0x83e70b13u
#define KEY1 0x95a4f1e0u
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
