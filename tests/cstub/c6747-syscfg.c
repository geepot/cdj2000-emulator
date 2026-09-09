/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c6747_syscfg.h"
#include "cdj_c6747_pll.h"
#include "cdj_c674x.h"
static CdjC6747Pll pll;
static bool write_bus(void *p, uint32_t a, uint64_t v, unsigned n, bool commit)
{
    CdjC6747Syscfg *s = p;
    if (cdj_c6747_syscfg_write(s, a, v, n, commit)) return true;
    if (cdj_c6747_syscfg_pll_locked(s) && cdj_c6747_pll_write_mapped(a, n)) return true;
    return cdj_c6747_pll_write(&pll, a, v, n, commit);
}
static bool read_bus(void *p, uint32_t a, uint32_t *v)
{ return cdj_c6747_syscfg_read(p, a, v) || cdj_c6747_pll_read(&pll, a, v); }
int main(void)
{
    CdjC6747Syscfg s;
    CdjC6747SyscfgPriority priority;
    uint32_t v;
    cdj_c6747_syscfg_reset(&s);
    cdj_c6747_syscfg_priority_reset(&priority);
    const uint32_t priority_reset[] = {0x44442222, 0x44440000, 0x54604404};
    for (unsigned i = 0; i < 3; ++i) {
        assert(cdj_c6747_syscfg_priority_read(
                   &priority, 0x01c14110 + i * 4, &v) &&
               v == priority_reset[i]);
        assert(cdj_c6747_syscfg_priority_write(
                   &priority, &s, 0x01c14110 + i * 4,
                   priority_reset[i], 4, true));
    }
    assert(cdj_c6747_syscfg_priority_valid(&priority));
    cdj_c6747_pll_reset(&pll);
    assert(!s.unlocked);
    const uint32_t cfg_defaults[] = {0, 0, 0xef00, 0xff00, 0};
    for (unsigned i = 0; i < 5; ++i) {
        assert(read_bus(&s, CDJ_C6747_CFGCHIP0 + i * 4, &v));
        assert(v == cfg_defaults[i]);
        assert(write_bus(&s, CDJ_C6747_CFGCHIP0 + i * 4,
                         i == 2 ? 0xef09 : i >= 3 ? 0xff07 : 1, 4, true));
        assert(read_bus(&s, CDJ_C6747_CFGCHIP0 + i * 4, &v) && v == cfg_defaults[i]);
    }
    for (unsigned i = 0; i < 20; ++i) {
        assert(write_bus(&s, CDJ_C6747_PINMUX0 + i * 4, i == 19 ? 0xf : 0xffffffff, 4, true));
        assert(read_bus(&s, CDJ_C6747_PINMUX0 + i * 4, &v) && !v);
    }
    assert(read_bus(&s, CDJ_C6747_KICK0, &v) && !v);
    assert(read_bus(&s, CDJ_C6747_KICK1, &v) && !v);
    assert(!write_bus(&s, CDJ_C6747_KICK0, 0, 8, true));
    assert(!write_bus(&s, CDJ_C6747_KICK0 + 1, 0, 4, true));
    assert(!write_bus(&s, CDJ_C6747_KICK0, 0, 1, true));
    assert(!write_bus(&s, 0x01c14170, 0, 4, true));
    assert(!read_bus(&s, 0x01c14170, &v));
    assert(write_bus(&s, CDJ_C6747_KICK1, 0x95a4f1e0, 4, true));
    assert(write_bus(&s, CDJ_C6747_KICK0, 0x83e70b13, 4, true));
    assert(!s.unlocked); /* Correct keys in reverse order do not unlock. */
    assert(write_bus(&s, CDJ_C6747_KICK1, 0x95a4f1e0, 4, false));
    assert(!s.unlocked); /* Checking a pending store has no effects. */
    assert(write_bus(&s, CDJ_C6747_KICK1, 0x95a4f1e0, 4, true));
    assert(s.unlocked);
    /* Recovered firmware raises DSP MDMA to priority 1 and EDMA3TC0 to
     * priority 2 using read/modify/write on MSTPRI0/1. */
    assert(cdj_c6747_syscfg_priority_write(
               &priority, &s, 0x01c14110, 0x44442122, 4, false));
    assert(priority.mstpri[0] == priority_reset[0]);
    assert(cdj_c6747_syscfg_priority_write(
               &priority, &s, 0x01c14110, 0x44442122, 4, true));
    assert(cdj_c6747_syscfg_priority_write(
               &priority, &s, 0x01c14114, 0x44442000, 4, true));
    assert(priority.mstpri[0] == 0x44442122 &&
           priority.mstpri[1] == 0x44442000);
    assert(!cdj_c6747_syscfg_priority_write(
               &priority, &s, 0x01c14110, 0, 4, true));
    assert(!cdj_c6747_syscfg_priority_write(
               &priority, &s, 0x01c14118, 0x10, 4, true));
    assert(write_bus(&s, CDJ_C6747_CFGCHIP0, 0x1a, 4, false));
    assert(!cdj_c6747_syscfg_pll_locked(&s));
    assert(write_bus(&s, CDJ_C6747_CFGCHIP0, 0x1a, 4, true));
    assert(cdj_c6747_syscfg_pll_locked(&s));
    CdjC6747Pll pll_before = pll;
    assert(write_bus(&s, 0x01c11100, 0x1c0, 4, true));
    assert(!memcmp(&pll, &pll_before, sizeof(pll)));
    assert(write_bus(&s, CDJ_C6747_CFGCHIP0, 0, 4, true));
    assert(!cdj_c6747_syscfg_pll_locked(&s));
    assert(write_bus(&s, 0x01c11100, 0x1c0, 4, true));
    assert(pll.config[0] == 0x1c0);
    assert(write_bus(&s, CDJ_C6747_CFGCHIP0 + 4, 0x10000, 4, true));
    assert(read_bus(&s, CDJ_C6747_CFGCHIP0 + 4, &v) && v == 0x10000);
    assert(write_bus(&s, CDJ_C6747_CFGCHIP0 + 8, 0x3ef09, 4, true));
    assert(read_bus(&s, CDJ_C6747_CFGCHIP0 + 8, &v) && v == 0xef09);
    assert(write_bus(&s, CDJ_C6747_CFGCHIP0 + 12, 0xff07, 4, true));
    assert(write_bus(&s, CDJ_C6747_CFGCHIP0 + 16, 0xff07, 4, true));
    assert(read_bus(&s, CDJ_C6747_CFGCHIP0 + 16, &v) && v == 0);
    assert(s.amute_clear_pulses == 7);
    assert(!write_bus(&s, CDJ_C6747_CFGCHIP0, 3, 4, true));
    assert(!write_bus(&s, CDJ_C6747_CFGCHIP0 + 4, 0x6000, 4, true));
    assert(!write_bus(&s, CDJ_C6747_CFGCHIP0 + 4, 0x13u << 27, 4, true));
    assert(!write_bus(&s, CDJ_C6747_CFGCHIP0 + 8, 10, 4, true));
    assert(!write_bus(&s, CDJ_C6747_CFGCHIP0 + 12, 7, 4, true));
    assert(!write_bus(&s, CDJ_C6747_CFGCHIP0 + 16, 0xff08, 4, true));
    assert(read_bus(&s, CDJ_C6747_KICK0, &v) && v == 0x83e70b13);
    assert(read_bus(&s, CDJ_C6747_KICK1, &v) && v == 0x95a4f1e0);
    assert(write_bus(&s, CDJ_C6747_KICK0, 0x83e70b13, 4, true));
    assert(s.unlocked); /* Correct key rewrite does not relock. */
    assert(write_bus(&s, CDJ_C6747_KICK0, 1, 4, true));
    assert(!s.unlocked);
    assert(write_bus(&s, CDJ_C6747_KICK1, 0x95a4f1e0, 4, true));
    assert(!s.unlocked);
    assert(write_bus(&s, CDJ_C6747_KICK0, 0x83e70b13, 4, true));
    assert(write_bus(&s, CDJ_C6747_KICK1, 0x95a4f1e0, 4, true));
    assert(s.unlocked);
    assert(write_bus(&s, CDJ_C6747_KICK1, 0, 4, true));
    assert(!s.unlocked);

    /* Issue the firmware's STW through the real CPU pipeline. Two adjacent
     * stores are checked before either commits, and unlock only at E3. */
    cdj_c6747_syscfg_reset(&s);
    CdjC674x c;
    cdj_c674x_reset(&c, 0x1000);
    c.r[0][3] = 0x83e70b13; c.r[0][5] = CDJ_C6747_KICK0;
    CdjC674xPacket p = {.count = 1, .next_pc = 0x1004,
        .instructions = {{.word = 0x01940274, .pc = 0x1000}}};
    assert(cdj_c674x_execute(&c, &p, read_bus, write_bus, &s));
    assert(s.kick[0] == 0);
    c.r[0][3] = 0x95a4f1e0; c.r[0][5] = CDJ_C6747_KICK1;
    assert(cdj_c674x_execute(&c, &p, read_bus, write_bus, &s));
    assert(s.kick[0] == 0 && !s.unlocked);
    p.count = 0;
    assert(cdj_c674x_execute(&c, &p, read_bus, write_bus, &s));
    assert(s.kick[0] == 0x83e70b13 && !s.unlocked);
    assert(cdj_c674x_execute(&c, &p, read_bus, write_bus, &s));
    assert(s.unlocked && s.kick[1] == 0x95a4f1e0);
    for (unsigned i = 0; i < 20; ++i) {
        uint32_t a = CDJ_C6747_PINMUX0 + i * 4;
        uint32_t data = i == 19 ? 2 : 0x11112180 + i;
        assert(write_bus(&s, a, data, 4, false));
        assert(read_bus(&s, a, &v) && !v);
        assert(write_bus(&s, a, data, 4, true));
        assert(read_bus(&s, a, &v) && v == data);
    }
    assert(write_bus(&s, CDJ_C6747_KICK0, 0, 4, true));
    assert(write_bus(&s, CDJ_C6747_PINMUX0, 0, 4, true));
    assert(read_bus(&s, CDJ_C6747_PINMUX0, &v) && v == 0x11112180);
    cdj_c6747_syscfg_reset(&s);
    assert(!s.unlocked && !s.kick[0] && !s.kick[1]);
    for (unsigned i = 0; i < 20; ++i) assert(s.pinmux[i] == 0);
    assert(s.cfgchip[0] == 0 && s.cfgchip[1] == 0 && s.cfgchip[2] == 0xef00 &&
           s.cfgchip[3] == 0xff00 && !s.amute_clear_pulses);
    assert(!write_bus(&s, CDJ_C6747_PINMUX0 + 76, 0x10, 4, false));
    assert(!write_bus(&s, CDJ_C6747_PINMUX0 + 76, 0x10, 4, true));
    assert(!write_bus(&s, CDJ_C6747_PINMUX0 + 77, 0, 4, true));
    puts("C6747 SYSCFG tests passed");
}
