#include "cdj_sh7764_iic.h"
#include <assert.h>
#include <string.h>

static uint32_t read8(CdjSh7764Iic *s, uint32_t offset)
{
    uint32_t value = 0xdeadbeef;
    assert(cdj_sh7764_iic_read(s, offset, 1, &value));
    return value;
}
static void reject(CdjSh7764Iic *s, unsigned offset, unsigned size, unsigned v)
{
    CdjSh7764Iic before = *s;
    assert(!cdj_sh7764_iic_write(s, offset, size, v));
    assert(memcmp(&before, s, sizeof before) == 0);
}
int main(void)
{
    CdjSh7764Iic s;
    cdj_sh7764_iic_reset(&s);
    assert(read8(&s, 4) == 0x40); /* pulled-up idle bus, not reset constant */
    for (unsigned o = 0; o <= 0x24; o += 4)
        if (o != 4) assert(read8(&s, o) == 0);
    assert(cdj_sh7764_iic_write(&s, 0x24, 1, 0xa5));
    assert(read8(&s, 0x24) == 0 && s.tx == 0xa5);
    /* Firmware preserves SCGD across its second read-modify-write. A
     * zero-read trap incorrectly turns this into 2 instead of 0x0e. */
    assert(cdj_sh7764_iic_write(&s, 0x18, 1, read8(&s, 0x18) | 0x0c));
    assert(cdj_sh7764_iic_write(&s, 0x18, 1,
                               (read8(&s, 0x18) & 0xfc) | 2));
    assert(read8(&s, 0x18) == 0x0e);
    assert(cdj_sh7764_iic_scl_period(&s) == 132);
    for (unsigned cdf = 0; cdf < 4; ++cdf) {
        for (unsigned scgd = 0; scgd < 64; ++scgd) {
            assert(cdj_sh7764_iic_write(&s, 0x18, 1, scgd * 4 + cdf));
            assert(cdj_sh7764_iic_scl_period(&s) ==
                   (cdf + 1) * (20 + scgd * 8));
        }
    }
    for (unsigned address = 0; address < 128; address++) {
        cdj_sh7764_iic_reset(&s);
        assert(cdj_sh7764_iic_write(&s, 0x18, 1, 0xc));
        assert(read8(&s, 0x18) == 0xc);
        assert(cdj_sh7764_iic_write(&s, 0x18, 1, 2));
        assert(cdj_sh7764_iic_write(&s, 0x20, 1, address << 1));
        assert(cdj_sh7764_iic_write(&s, 0x24, 1, 0));
        assert(cdj_sh7764_iic_write(&s, 4, 1, 0x89));
        assert(s.issued_address == address << 1);
        assert(read8(&s, 0xc) == 0);
        CdjSh7764Iic before = s;
        for (int i = 0; i < 100; i++) (void)read8(&s, 4);
        assert(memcmp(&before, &s, sizeof s) == 0);
        reject(&s, 4, 1, 0x89); /* repeated START */
        reject(&s, 4, 1, 0); /* active disable */
        reject(&s, 4, 1, 8); /* active transfer-mode change */
        reject(&s, 0x18, 1, 0);
        reject(&s, 0x20, 1, 0);
        reject(&s, 0x24, 1, 1);
        assert(cdj_sh7764_iic_advance(&s));
        assert(read8(&s, 0xc) == 0x49); /* MAT/MDE/MNR; no MDT/MDR */
        assert(cdj_sh7764_iic_write(&s, 4, 1, 0x88));
        assert(cdj_sh7764_iic_advance(&s));
        assert(read8(&s, 0xc) == 0x59);
        assert(read8(&s, 4) == 0xc8);
        assert(read8(&s, 0x24) == 0);
        assert(cdj_sh7764_iic_write(&s, 0xc, 1, 0x7f));
        assert(read8(&s, 0xc) == 0x59);
        assert(cdj_sh7764_iic_write(&s, 0xc, 1, 0x3f));
        assert(read8(&s, 0xc) == 0x19);
        assert(cdj_sh7764_iic_write(&s, 0xc, 1, 0));
        assert(read8(&s, 0xc) == 0);
        assert(cdj_sh7764_iic_write(&s, 4, 1, 0x8a));
        assert(cdj_sh7764_iic_advance(&s));
        assert(read8(&s, 0xc) == 0); /* idle FSB isn't a transfer */
    }
    cdj_sh7764_iic_reset(&s);
    reject(&s, 0, 1, 4); reject(&s, 0x10, 1, 1);
    reject(&s, 0x14, 1, 1); reject(&s, 4, 1, 0x10);
    reject(&s, 4, 1, 0xc); reject(&s, 4, 1, 0xb);
    reject(&s, 4, 1, 1); reject(&s, 0xc, 1, 128);
    reject(&s, 0x25, 1, 0); reject(&s, 0x18, 2, 0);
    reject(&s, 0x18, 1, 256);
    assert(cdj_sh7764_iic_write(&s, 0x20, 1, 0x21));
    reject(&s, 4, 1, 9); /* receiver unsupported, no invented RX data */
    uint32_t value = 0xdeadbeef;
    assert(!cdj_sh7764_iic_read(&s, 1, 1, &value));
    assert(!cdj_sh7764_iic_read(&s, 4, 4, &value));
    assert(value == 0xdeadbeef);
    s.phase = 255;
    CdjSh7764Iic before = s;
    assert(!cdj_sh7764_iic_advance(&s));
    assert(memcmp(&before, &s, sizeof s) == 0);
    return 0;
}
