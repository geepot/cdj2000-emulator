/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "cdj_c6747_emifb.h"

int main(void)
{
    CdjC6747Emifb s, before;
    uint32_t v;
    cdj_c6747_emifb_reset(&s);
    assert(cdj_c6747_emifb_read(&s, CDJ_C6747_EMIFB_REVID, &v) &&
           v == 0x4033131f);
    assert(cdj_c6747_emifb_read(&s, CDJ_C6747_EMIFB_SDCFG, &v) &&
           v == 0x00010620);
    assert(cdj_c6747_emifb_sdram_enabled(&s));

    before = s;
    assert(cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDCFG,
                                 0x0080c621, 4, false));
    assert(!memcmp(&s, &before, sizeof(s)));
    assert(cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDCFG,
                                 0x0080c621, 4, true));
    /* The first BOOT_UNLOCK write cannot yet clear protected SDREN. */
    assert(s.sdcfg == 0x0081c621 && s.init_sequences == 1);
    assert(cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDTIM1,
                                 0x10912a08, 4, true));
    assert(cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDTIM2,
                                 0x600a0007, 4, true));
    assert(cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDRFC,
                                 0x000003f6, 4, true));
    assert(cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDCFG,
                                 0x00814621, 4, true));
    assert(cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDCFG,
                                 0x00014621, 4, true));
    assert(s.sdcfg == 0x00014621 && s.sdtim1 == 0x10912a08 &&
           s.sdtim2 == 0x600a0007 && s.sdrfc == 0x3f6 &&
           s.init_sequences == 3);

    /* Locked legal writes complete but protected fields and timing retain. */
    assert(cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDCFG,
                                 0x00004621, 4, true));
    assert(s.sdcfg == 0x00014621);
    assert(cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDTIM1,
                                 0x14d93890, 4, true));
    assert(s.sdtim1 == 0x10912a08);

    /* Values below 0x100 load twice T_RFC as documented. */
    s.sdcfg |= 1u << 15;
    assert(cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDRFC, 1, 4, true));
    assert(s.sdrfc == 16);
    assert(!cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDTIM1, 7, 4, true));
    assert(!cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDTIM2,
                                  0x80000000, 4, true));
    assert(!cdj_c6747_emifb_write(&s, CDJ_C6747_EMIFB_SDCFG,
                                  0x00000220, 2, true));
    assert(!cdj_c6747_emifb_read(&s, CDJ_C6747_EMIFB_BASE + 4, &v));
    return 0;
}
