"""Compile the board's actual read dispatcher against its real peripheral models."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_dsp_ram_and_peripheral_dispatch(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    directory = ROOT / 'emulator/qemu'
    source = (directory / 'cdj2000_nxs_hpi.c').read_text()
    # Only QEMU's unused enclosing register/timer types need stand-ins.
    prefix = source[source.index('#include "cdj_c674x.h"'):source.index('static NxsHpi *nxs_hpi;')]
    start = source.index('static bool dsp_read(')
    read = source[start:source.index('\nstatic uint8_t *dsp_memory_span', start)]
    start = source.index('static uint8_t *host_memory(')
    host = source[start:source.index('\nstatic bool valid_data', start)]
    start = source.index('static uint8_t *dsp_memory_span(')
    span = source[start:source.index('\ntypedef struct {', start)]
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef int MemoryRegion;
typedef int QEMUTimer;
static uint32_t ldl_le_p(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
'''
    checks = r'''
static void check_ram(NxsHpi *s, uint32_t base, uint32_t size, uint8_t *bytes)
{
    uint32_t value;
    bytes[0] = 0x12; bytes[size - 1] = 0x34;
    assert(dsp_read(s, base, &value) && value == 0x12);
    assert(dsp_read(s, base + size - 4, &value) && value == 0x34000000);
    assert(!dsp_read(s, base - 4, &value));
    assert(!dsp_read(s, base + size, &value));
    assert(!dsp_read(s, base + 1, &value));
    assert(!dsp_read(s, base + size - 3, &value));
}
int main(void)
{
    NxsHpi *s = calloc(1, sizeof(*s));
    s->shared_ram = calloc(1, SHARED_RAM_SIZE);
    s->sdram = calloc(1, SDRAM_SIZE);
    assert(s->shared_ram && s->sdram);
    check_ram(s, L2_BASE, L2_SIZE, s->l2);
    check_ram(s, 0x00800000, L2_SIZE, s->l2);
    check_ram(s, SHARED_RAM_BASE, SHARED_RAM_SIZE, s->shared_ram);
    uint32_t value = 0;
    cdj_c6747_emifb_reset(&s->emifb);
    assert(cdj_c6747_emifb_sdram_enabled(&s->emifb));
    s->sdram[0] = 0x12; s->sdram[SDRAM_SIZE - 1] = 0x34;
    for (uint32_t base = SDRAM_BASE; base < 0xe0000000; base += SDRAM_SIZE) {
        assert(dsp_read(s, base, &value) && value == 0x12);
        assert(dsp_read(s, base + SDRAM_SIZE - 4, &value) && value == 0x34000000);
        assert(!dsp_read(s, base + 1, &value));
    }
    s->sdram[0x00ccff9c] = 0xa5;
    assert(dsp_read(s, 0xd2ccff9c, &value) && value == 0xa5);
    assert(host_memory(s, 0xd2ccff9c) == s->sdram + 0x00ccff9c);
    assert(dsp_memory_span(s, 0xd2ccff9c, 8) == s->sdram + 0x00ccff9c);
    assert(!dsp_memory_span(s, 0xd3fffffc, 8));
    assert(!dsp_memory_span(s, 0xdffffffc, 8));
    assert(!dsp_read(s, SDRAM_BASE - 4, &value));
    assert(!dsp_read(s, 0xe0000000, &value));
    s->emifb.sdcfg &= ~(1u << 16);
    assert(!dsp_read(s, SDRAM_BASE, &value));
    assert(!dsp_read(s, 0xd2ccff9c, &value));
    assert(!host_memory(s, 0xd2ccff9c));
    assert(!dsp_memory_span(s, 0xd2ccff9c, 8));
    assert(dsp_read(s, CDJ_C6747_EMIFB_REVID, &value) && value == 0x4033131f);
    assert(dsp_read(s, CDJ_C6747_EMIFB_SDCFG, &value) && value == s->emifb.sdcfg);
    assert(!dsp_read(s, 0xffffffff, &value));
    free(s->shared_ram); free(s->sdram); free(s);
}
'''
    fixture = tmp_path / 'read.c'
    fixture.write_text(harness + prefix + host + read + span + checks)
    binary = tmp_path / 'read-test'
    models = sorted(directory.glob('cdj_c6747_*.c'))
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(directory), str(fixture), *map(str, models),
                    str(directory / 'cdj_c674x_loop.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
