/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "cdj_c6747_i2c.h"
int main(void)
{
    CdjC6747I2c s, before;
    cdj_c6747_i2c_reset(&s);
    for (unsigned b = 0; b < 2; ++b) {
        uint32_t base = b ? 0x01e28000 : 0x01c22000, v;
        unsigned offsets[] = {0x24, 0x48, 0x4c, 0x54};
        for (unsigned i = 0; i < 4; ++i)
            assert(cdj_c6747_i2c_read(&s, base + offsets[i], &v) && v == 0);
        assert(!cdj_c6747_i2c_write(&s, base + 0x24, 32, 4, true));
        assert(cdj_c6747_i2c_write(&s, base + 0x48, 1, 4, true));
        before = s;
        assert(cdj_c6747_i2c_write(&s, base + 0x24, 32, 4, false));
        assert(cdj_c6747_i2c_write(&s, base + 0x58, 3, 4, false));
        assert(!memcmp(&s, &before, sizeof(s)));
        assert(cdj_c6747_i2c_write(&s, base + 0x24, 32, 4, true));
        assert(cdj_c6747_i2c_read(&s, base + 0x24, &v) && v == 32);
        assert(!cdj_c6747_i2c_write(&s, base + 0x48, 0, 4, true));
        assert(!cdj_c6747_i2c_write(&s, base + 0x24, 0x2420, 4, true));
        assert(cdj_c6747_i2c_write(&s, base + 0x24, 0x4020, 4, true));
        assert(cdj_c6747_i2c_write(&s, base + 0x54, 1, 4, true));
        assert(cdj_c6747_i2c_write(&s, base + 0x58, 2, 4, true));
        assert(cdj_c6747_i2c_write(&s, base + 0x5c, 1, 4, true));
        assert(cdj_c6747_i2c_write(&s, base + 0x58, 0, 4, true));
        assert(cdj_c6747_i2c_write(&s, base + 0x5c, 0, 4, true));
        assert(cdj_c6747_i2c_write(&s, base + 0x4c, 3, 4, true));
        assert(cdj_c6747_i2c_read(&s, base + 0x54, &v) && v == 2);
        assert(cdj_c6747_i2c_write(&s, base + 0x24, 0, 4, true));
        assert(cdj_c6747_i2c_write(&s, base + 0x48, 0, 4, true));
        assert(cdj_c6747_i2c_read(&s, base + 0x54, &v) && v == 2);
        unsigned unsupported[] = {8,0x18,0x20,0x28,0x50,0x58,0x5c};
        for (unsigned i = 0; i < 7; ++i)
            assert(!cdj_c6747_i2c_read(&s, base + unsupported[i], &v));
        before = s;
        assert(!cdj_c6747_i2c_write(&s, base + 0x24, 0x1000, 4, true));
        assert(!cdj_c6747_i2c_write(&s, base + 0x48, 2, 4, true));
        assert(!cdj_c6747_i2c_write(&s, base + 0x4c, 4, 4, true));
        assert(!cdj_c6747_i2c_write(&s, base + 0x4d, 0, 4, true));
        assert(!cdj_c6747_i2c_write(&s, base + 0x24, 0, 2, true));
        assert(!memcmp(&s, &before, sizeof(s)));
        if (!b) assert(s.mode[1] == 0 && s.output[1] == 0);
    }
    cdj_c6747_i2c_reset(&s);
    before = (CdjC6747I2c){0};
    assert(!memcmp(&s, &before, sizeof(s)));
    return 0;
}
