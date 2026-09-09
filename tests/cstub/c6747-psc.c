#include <assert.h>
#include <string.h>
#include "cdj_c6747_psc.h"
int main(void)
{
    CdjC6747Psc s;
    cdj_c6747_psc_reset(&s);
    uint32_t v;
    const uint32_t base[] = {0x01c10000, 0x01e27000};
    for (unsigned b = 0; b < 2; ++b) {
        uint32_t module = base[b] + 0xa04; /* EDMA TC0 / USB0 */
        assert(cdj_c6747_psc_read(&s, base[b] + 0x128, &v) && v == 0);
        assert(cdj_c6747_psc_read(&s, module - 0x200, &v) && v == 0xb00);
        CdjC6747Psc before = s;
        assert(cdj_c6747_psc_write(&s, module, 3, 4, false));
        assert(!memcmp(&s, &before, sizeof(s)));
        assert(cdj_c6747_psc_write(&s, module, 3, 4, true));
        for (unsigned i = 0; i < 10; ++i) cdj_c6747_psc_tick(&s);
        assert(cdj_c6747_psc_read(&s, module - 0x200, &v) && v == 0xb00);
        assert(cdj_c6747_psc_write(&s, base[b] + 0x120, 1, 4, true));
        /* Latch NEXT at GO; changing NEXT does not alter the pending request. */
        assert(cdj_c6747_psc_write(&s, module, 2, 4, true));
        for (unsigned i = 0; i < 7; ++i) {
            assert(cdj_c6747_psc_read(&s, base[b] + 0x128, &v) && v == 1);
            cdj_c6747_psc_tick(&s);
        }
        assert(cdj_c6747_psc_read(&s, base[b] + 0x128, &v) && v == 1);
        cdj_c6747_psc_tick(&s);
        assert(cdj_c6747_psc_read(&s, base[b] + 0x128, &v) && v == 0);
        assert(cdj_c6747_psc_read(&s, module - 0x200, &v) && v == 0x1f03);
        assert(cdj_c6747_psc_write(&s, base[b] + 0x120, 1, 4, true));
        for (unsigned i = 0; i < 8; ++i) cdj_c6747_psc_tick(&s);
        assert(cdj_c6747_psc_read(&s, module - 0x200, &v) && v == 0xf02);
        assert(!cdj_c6747_psc_write(&s, module, 4, 4, true));
        assert(!cdj_c6747_psc_write(&s, module, 3, 2, true));
        assert(!cdj_c6747_psc_write(&s, base[b] + 0x128, 0, 4, true));
    }
    assert(!cdj_c6747_psc_read(&s, 0x01c10818, &v)); /* unused module */
    assert(!cdj_c6747_psc_write(&s, 0x01c10a3c, 0, 4, true)); /* DSP reset */
    assert(cdj_c6747_psc_write(&s, 0x01c10a2c, 0, 4, true)); /* SCR1 stays enabled */
    assert(cdj_c6747_psc_write(&s, 0x01c10120, 1, 4, true));
    for (unsigned i = 0; i < 8; ++i) cdj_c6747_psc_tick(&s);
    assert(cdj_c6747_psc_read(&s, 0x01c1082c, &v) && v == 0x1f03);
    return 0;
}
