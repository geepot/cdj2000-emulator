/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "cdj_c6747_syscfg.h"
#include "cdj_c674x.h"
static bool write_bus(void *p, uint32_t a, uint64_t v, unsigned n, bool commit)
{ return cdj_c6747_syscfg_write(p, a, v, n, commit); }
static bool read_bus(void *p, uint32_t a, uint32_t *v)
{ return cdj_c6747_syscfg_read(p, a, v); }
int main(void)
{
    CdjC6747Syscfg s;
    uint32_t v;
    cdj_c6747_syscfg_reset(&s);
    assert(!s.unlocked);
    assert(read_bus(&s, CDJ_C6747_KICK0, &v) && !v);
    assert(read_bus(&s, CDJ_C6747_KICK1, &v) && !v);
    assert(!write_bus(&s, CDJ_C6747_KICK0, 0, 8, true));
    assert(!write_bus(&s, CDJ_C6747_KICK0 + 1, 0, 4, true));
    assert(!write_bus(&s, CDJ_C6747_KICK0, 0, 1, true));
    assert(!write_bus(&s, 0x01c14120, 0, 4, true));
    assert(!read_bus(&s, 0x01c14120, &v));
    assert(write_bus(&s, CDJ_C6747_KICK1, 0x95a4f1e0, 4, true));
    assert(write_bus(&s, CDJ_C6747_KICK0, 0x83e70b13, 4, true));
    assert(!s.unlocked); /* Correct keys in reverse order do not unlock. */
    assert(write_bus(&s, CDJ_C6747_KICK1, 0x95a4f1e0, 4, false));
    assert(!s.unlocked); /* Checking a pending store has no effects. */
    assert(write_bus(&s, CDJ_C6747_KICK1, 0x95a4f1e0, 4, true));
    assert(s.unlocked);
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
    cdj_c6747_syscfg_reset(&s);
    assert(!s.unlocked && !s.kick[0] && !s.kick[1]);
    puts("C6747 SYSCFG tests passed");
}
