/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "cdj_c6747_mcasp.h"
int main(void)
{
    CdjC6747Mcasp s, before;
    cdj_c6747_mcasp_reset(&s);
    const uint32_t masks[] = {0xfe00ffffu, 0xfe000fffu, 0xfe00000fu};
    for (unsigned b = 0; b < 3; ++b) {
        uint32_t base = 0x01d00000 + b * 0x4000, v;
        for (unsigned o = 0x10; o <= 0x18; o += 4) {
            assert(cdj_c6747_mcasp_read(&s, base + o, &v) && v == 0);
            before = s;
            assert(cdj_c6747_mcasp_write(&s, base + o, masks[b], 4, false));
            assert(!memcmp(&s, &before, sizeof(s)));
            assert(cdj_c6747_mcasp_write(&s, base + o, masks[b], 4, true));
            assert(cdj_c6747_mcasp_read(&s, base + o, &v) && v == masks[b]);
        }
        before = s;
        assert(cdj_c6747_mcasp_write(&s, base + 0x20, 5, 4, false));
        assert(cdj_c6747_mcasp_write(&s, base + 0x1c, 5, 4, false));
        assert(!memcmp(&s, &before, sizeof(s)));
        assert(cdj_c6747_mcasp_write(&s, base + 0x20, 5, 4, true));
        assert(cdj_c6747_mcasp_read(&s, base + 0x18, &v) && v == (masks[b] & ~5u));
        assert(cdj_c6747_mcasp_write(&s, base + 0x1c, 1, 4, true));
        assert(cdj_c6747_mcasp_write(&s, base + 0x1c, 0, 4, true));
        assert(cdj_c6747_mcasp_write(&s, base + 0x20, 0, 4, true));
        assert(cdj_c6747_mcasp_read(&s, base + 0x18, &v) && v == (masks[b] & ~4u));
        assert(cdj_c6747_mcasp_write(&s, base + 0x10, 0, 4, true));
        assert(cdj_c6747_mcasp_write(&s, base + 0x14, 0, 4, true));
        assert(cdj_c6747_mcasp_read(&s, base + 0x18, &v) && v == (masks[b] & ~4u));
        before = s;
        for (unsigned o = 0x10; o <= 0x20; o += 4) {
            assert(!cdj_c6747_mcasp_write(&s, base + o, ~masks[b], 4, true));
            assert(!cdj_c6747_mcasp_write(&s, base + o, 0, 1, true));
            assert(!cdj_c6747_mcasp_write(&s, base + o, UINT64_C(1)<<32, 4, true));
        }
        assert(!cdj_c6747_mcasp_read(&s, base + 0x1c, &v));
        assert(!cdj_c6747_mcasp_read(&s, base + 0x20, &v));
        assert(!cdj_c6747_mcasp_read(&s, base + 0x44, &v));
        assert(!cdj_c6747_mcasp_write(&s, base + 0x44, 0, 4, true));
        assert(!cdj_c6747_mcasp_read(&s, base + 0x15, &v));
        assert(!cdj_c6747_mcasp_write(&s, base + 0x15, 0, 4, true));
        assert(!memcmp(&s, &before, sizeof(s)));
        for (unsigned other = b + 1; other < 3; ++other)
            assert(s.pfunc[other] == 0 && s.pdir[other] == 0 && s.pdout[other] == 0);
    }
    cdj_c6747_mcasp_reset(&s);
    before = (CdjC6747Mcasp){0};
    assert(!memcmp(&s, &before, sizeof(s)));
    return 0;
}
