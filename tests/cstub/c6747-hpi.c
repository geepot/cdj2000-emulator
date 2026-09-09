/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "cdj_c6747_hpi.h"
int main(void)
{
    CdjC6747Hpi s, before;
    uint32_t v;
    cdj_c6747_hpi_reset(&s);
    assert(cdj_c6747_hpi_host_read(&s) == 0x00c800c8);
    assert(cdj_c6747_hpi_cpu_read(&s, CDJ_C6747_HPI_BASE, &v) && v == 0x4421210a);
    assert(cdj_c6747_hpi_cpu_read(&s, CDJ_C6747_HPIC, &v) && v == 0xc8);
    cdj_c6747_hpi_rom_boot_ready(&s);
    assert(!s.hpirst && s.hint && cdj_c6747_hpi_host_read(&s) == 0x004c004c);
    cdj_c6747_hpi_host_write(&s, 0x01050105);
    assert(!s.hpirst && s.hwob && cdj_c6747_hpi_host_read(&s) == 0x01490149);
    cdj_c6747_hpi_host_write(&s, 0x01030103); /* Host read-modify-write DSPINT. */
    assert(s.dspint && cdj_c6747_hpi_cpu_read(&s, CDJ_C6747_HPIC, &v) && v == 0x14a);
    before = s;
    assert(cdj_c6747_hpi_cpu_write(&s, CDJ_C6747_HPIC, 6, 4, false));
    assert(!memcmp(&s, &before, sizeof(s)));
    assert(cdj_c6747_hpi_cpu_write(&s, CDJ_C6747_HPIC, 6, 4, true));
    assert(!s.dspint && s.hint);
    cdj_c6747_hpi_host_write(&s, 0x01050105); /* Host read-modify-write HINT ack. */
    assert(!s.hint);
    cdj_c6747_hpi_host_write(&s, 0x08010801); /* HPIASEL, HWOB, no reset */
    assert(s.hpiasel && s.hwob);
    assert(cdj_c6747_hpi_cpu_read(&s, CDJ_C6747_HPIC, &v) && v == 0x948);
    assert(!cdj_c6747_hpi_cpu_write(&s, CDJ_C6747_HPIC, 0, 2, true));
    assert(!cdj_c6747_hpi_cpu_read(&s, CDJ_C6747_HPIC + 1, &v));
    cdj_c6747_hpi_host_write(&s, 0x00800080);
    assert(s.hpirst && !s.hint && !s.dspint && !s.hwob);
    return 0;
}
