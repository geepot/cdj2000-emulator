#include "cdj_mfi_identity.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    uint8_t output[4];
    const uint8_t sentinel[] = {0xa5, 0x5a, 0xcc, 0x33};
    const size_t invalid_lengths[] = {0, 2, 3, 4, 128, 256, SIZE_MAX};
    const uint32_t wide_regs[] = {256, 257, 0x10000, UINT32_MAX};

    for (uint32_t reg = 0; reg < 256; ++reg) {
        memcpy(output, sentinel, sizeof output);
        bool ok = cdj_mfi_identity_read(reg, 1, &output[1]);
        assert(ok == (reg <= 1));
        if (ok) {
            assert(output[1] == (reg == 0 ? 5 : 1));
            assert(output[0] == sentinel[0]);
            assert(output[2] == sentinel[2]);
            assert(output[3] == sentinel[3]);
        } else {
            assert(memcmp(output, sentinel, sizeof output) == 0);
        }
        assert(!cdj_mfi_identity_read(reg, 1, NULL));
        for (size_t i = 0; i < sizeof invalid_lengths / sizeof *invalid_lengths; ++i) {
            memcpy(output, sentinel, sizeof output);
            assert(!cdj_mfi_identity_read(reg, invalid_lengths[i], &output[1]));
            assert(memcmp(output, sentinel, sizeof output) == 0);
        }
    }
    for (size_t i = 0; i < sizeof wide_regs / sizeof *wide_regs; ++i) {
        memcpy(output, sentinel, sizeof output);
        assert(!cdj_mfi_identity_read(wide_regs[i], 1, &output[1]));
        assert(memcmp(output, sentinel, sizeof output) == 0);
    }
    /* Unsupported operations cannot change subsequent identity reads. */
    for (unsigned i = 0; i < 100; ++i) {
        assert(cdj_mfi_identity_read(i & 1, 1, output));
        assert(output[0] == ((i & 1) ? 1 : 5));
    }
    return 0;
}
