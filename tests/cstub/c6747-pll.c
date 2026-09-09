/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "cdj_c6747_pll.h"
int main(void)
{
    CdjC6747Pll s, before;
    cdj_c6747_pll_reset(&s);
    unsigned offsets[] = {0x100,0x104,0x110,0x114,0x118,0x11c,0x120,
                          0x124,0x128,0x160,0x164,0x168,0x16c};
    uint32_t defaults[] = {0xf2,0x14,0x13,0x8000,0x8000,0x8001,0x8002,
                           0x8000,0x8001,0x8003,0x8002,0x8000,0x8005};
    uint32_t v;
    for (unsigned i = 0; i < 13; ++i) {
        uint32_t a = 0x01c11000 + offsets[i];
        assert(cdj_c6747_pll_read(&s, a, &v) && v == defaults[i]);
        uint32_t candidate = i == 0 ? 0x1d0 : i == 1 ? 0x1f : i == 2 ? 31 : 0x801f;
        before = s;
        assert(cdj_c6747_pll_write(&s, a, candidate, 4, false));
        assert(!memcmp(&s, &before, sizeof(s)));
        assert(cdj_c6747_pll_write(&s, a, candidate, 4, true));
        assert(cdj_c6747_pll_read(&s, a, &v) && v == candidate);
        before = s;
        assert(!cdj_c6747_pll_write(&s, a, UINT32_MAX, 4, true));
        assert(!cdj_c6747_pll_write(&s, a, 0, 1, true));
        assert(!cdj_c6747_pll_write(&s, a+1, 0, 4, true));
        assert(!memcmp(&s, &before, sizeof(s)));
    }
    assert(cdj_c6747_pll_write(&s, 0x01c11100, 0x10, 4, true));
    assert(cdj_c6747_pll_read(&s, 0x01c11100, &v) && v == 0xd0);
    assert(!cdj_c6747_pll_write(&s, 0x01c11100, 0x1c0, 4, true));
    assert(!cdj_c6747_pll_write(&s, 0x01c11100, 0xd8, 4, true));
    assert(!cdj_c6747_pll_write(&s, 0x01c11100, 0xd1, 4, true));
    assert(!cdj_c6747_pll_write(&s, 0x01c11104, 0x15, 4, true));
    assert(!cdj_c6747_pll_write(&s, 0x01c11138, 1, 4, true));
    assert(!cdj_c6747_pll_read(&s, 0x01c1113c, &v));
    cdj_c6747_pll_reset(&s);
    for (unsigned i = 0; i < 13; ++i) assert(s.config[i] == defaults[i]);
    return 0;
}
