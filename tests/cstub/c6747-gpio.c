/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "cdj_c6747_gpio.h"
int main(void)
{
    CdjC6747Gpio s, before;
    cdj_c6747_gpio_reset(&s);
    uint32_t v;
    assert(cdj_c6747_gpio_read(&s, 0x01e26008, &v) && v == 0);
    assert(!cdj_c6747_gpio_write(&s, 0x01e26008, 256, 4, true));
    assert(cdj_c6747_gpio_write(&s, 0x01e26008, 255, 4, false));
    assert(s.binten == 0);
    assert(cdj_c6747_gpio_write(&s, 0x01e26008, 255, 4, true));
    assert(s.binten == 255);
    for (unsigned p = 0; p < 4; ++p) {
        uint32_t base = 0x01e26010 + p * 0x28;
        assert(cdj_c6747_gpio_read(&s, base, &v) && v == UINT32_MAX);
        assert(cdj_c6747_gpio_set_input(&s, p * 2, 0, true));
        assert(cdj_c6747_gpio_set_input(&s, p * 2 + 1, 0, true));
        assert(cdj_c6747_gpio_read(&s, base + 16, &v) && v == 0x00010001);
        assert(cdj_c6747_gpio_set_input(&s, p * 2, 0, false));
        assert(cdj_c6747_gpio_set_input(&s, p * 2 + 1, 0, false));
        before = s;
        for (unsigned o = 0; o < 36; o += 4) {
            if (o == 16) continue;
            assert(cdj_c6747_gpio_write(&s, base + o, 0xa5000001, 4, false));
            assert(!memcmp(&s, &before, sizeof(s)));
        }
        assert(cdj_c6747_gpio_write(&s, base + 4, 0xa5000001, 4, true));
        assert(cdj_c6747_gpio_write(&s, base, 0, 4, true));
        const unsigned set[] = {8,20,28}, clear[] = {12,24,32};
        for (unsigned i = 0; i < 3; ++i) {
            assert(cdj_c6747_gpio_write(&s, base + set[i], UINT32_MAX, 4, true));
            assert(cdj_c6747_gpio_write(&s, base + clear[i], 0xa5000001, 4, true));
            assert(cdj_c6747_gpio_read(&s, base + set[i], &v) && v == 0x5afffffe);
            assert(cdj_c6747_gpio_read(&s, base + clear[i], &v) && v == 0x5afffffe);
            assert(cdj_c6747_gpio_write(&s, base + set[i], 0, 4, true));
            assert(cdj_c6747_gpio_write(&s, base + clear[i], 0, 4, true));
            assert(cdj_c6747_gpio_read(&s, base + set[i], &v) && v == 0x5afffffe);
        }
        assert(cdj_c6747_gpio_read(&s, base + 4, &v) && v == 0x5afffffe);
        assert(cdj_c6747_gpio_read(&s, base + 16, &v) && v == 0x5afffffe);
        assert(!cdj_c6747_gpio_read(&s, base + 36, &v));
        before = s;
        assert(!cdj_c6747_gpio_write(&s, base + 36, 0, 4, true));
        assert(!cdj_c6747_gpio_write(&s, base, 0, 2, true));
        assert(!cdj_c6747_gpio_write(&s, base + 1, 0, 4, true));
        assert(!memcmp(&s, &before, sizeof(s)));
        for (unsigned other = p + 1; other < 4; ++other)
            assert(s.dir[other] == UINT32_MAX && s.output[other] == 0);
    }
    assert(!cdj_c6747_gpio_read(&s, 0x01e260b0, &v));
    assert(!cdj_c6747_gpio_set_input(&s, 8, 0, true));
    assert(!cdj_c6747_gpio_set_input(&s, 0, 16, true));
    assert(!cdj_c6747_gpio_write(&s, 0x01e260b0, 0, 4, true));
    return 0;
}
