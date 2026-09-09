/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_c6747_syscfg.h"
#define KEY0 0x83e70b13u
#define KEY1 0x95a4f1e0u
void cdj_c6747_syscfg_reset(CdjC6747Syscfg *s)
{
    *s = (CdjC6747Syscfg){0};
}
bool cdj_c6747_syscfg_read(const CdjC6747Syscfg *s, uint32_t address,
                          uint32_t *value)
{
    if (address != CDJ_C6747_KICK0 && address != CDJ_C6747_KICK1) return false;
    *value = s->kick[(address - CDJ_C6747_KICK0) / 4];
    return true;
}
bool cdj_c6747_syscfg_write(CdjC6747Syscfg *s, uint32_t address,
                           uint64_t value, unsigned size, bool commit)
{
    if (size != 4 || (address != CDJ_C6747_KICK0 && address != CDJ_C6747_KICK1))
        return false;
    if (!commit) return true;
    unsigned index = (address - CDJ_C6747_KICK0) / 4;
    s->kick[index] = (uint32_t)value;
    if ((uint32_t)value != (index ? KEY1 : KEY0)) s->unlocked = false;
    else if (index && s->kick[0] == KEY0) s->unlocked = true;
    return true;
}
