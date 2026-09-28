"""Compile the NXS board's idle-loop yield proof against its real peripheral models."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


def test_dsp_idle_proof_rules(tmp_path):
    cc = shutil.which('cc')
    if not cc:
        pytest.skip('requires C compiler')
    directory = ROOT / 'emulator/qemu'
    source = (directory / 'cdj2000_nxs_hpi.c').read_text()
    prefix = source[source.index('#include "cdj_c674x.h"'):source.index('static NxsHpi *nxs_hpi;')]
    start = source.index('static uint8_t *dsp_memory_span(')
    span = source[start:source.index('\n}\n', start) + 3]
    start = source.index('/*\n * Idle-loop yield.')
    idle = source[start:source.index('static bool dsp_write(void *opaque', start)]
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef int MemoryRegion;
typedef int QEMUTimer;
typedef int AudioBackend;
typedef int SWVoiceOut;
typedef int Notifier;
typedef int QemuMutex;
static uint32_t ldl_le_p(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
'''
    checks = r'''
int main(void)
{
    NxsHpi *s = calloc(1, sizeof(*s));
    s->shared_ram = calloc(1, SHARED_RAM_SIZE);
    s->sdram = calloc(1, SDRAM_SIZE);
    assert(s && s->shared_ram && s->sdram);
    cdj_c6747_cache_reset(&s->cache);
    cdj_c6747_emifb_reset(&s->emifb);
    cdj_c6747_pll_reset(&s->pll);
    s->pll.reset_age = 17;
    s->cpu.pc = L2_BASE + 0x100;
    s->cpu.cycles = 20;
    s->cpu.control_ready[5] = 10;
    s->cpu.control_ready[31] = 7;
    assert(dsp_idle_quiescent(s));
    dsp_idle_anchor(s, 0);
    assert(dsp_idle_repeat(s));

    /* Registers, PC and in-flight work must all match. */
    s->cpu.r[0][10] = 1; assert(!dsp_idle_repeat(s)); s->cpu.r[0][10] = 0;
    s->cpu.pc += 4; assert(!dsp_idle_repeat(s)); s->cpu.pc -= 4;
    s->cpu.load_count = 1; assert(!dsp_idle_repeat(s)); s->cpu.load_count = 0;
    assert(dsp_idle_repeat(s));

    /* Passed delayed-control cycles are equivalent; pending ones and the
     * loop-context slot are not. */
    s->cpu.cycles = 30; s->cpu.control_ready[5] = 15; assert(dsp_idle_repeat(s));
    s->cpu.control_ready[5] = 40; assert(!dsp_idle_repeat(s));
    s->cpu.control_ready[5] = 15;
    s->cpu.control_ready[31] = 8; assert(!dsp_idle_repeat(s));
    s->cpu.control_ready[31] = 7;

    /* Memory must be back to its anchor value, whatever happened between. */
    s->l2[0x40] = 0x11;
    dsp_idle_note_write(s, L2_BASE + 0x41, 0x22, 1);
    assert(!s->idle_dirty && s->idle_log_count == 1);
    s->l2[0x41] = 0x22; assert(!dsp_idle_repeat(s));
    dsp_idle_note_write(s, L2_BASE + 0x40, 0x3344, 2);
    assert(s->idle_log_count == 1);
    s->l2[0x41] = 0; assert(dsp_idle_repeat(s));
    dsp_idle_note_write(s, L2_BASE + 0x7e, 0x1122334455667788u, 8);
    assert(s->idle_log_count == 4);

    /* Device writes and an overflowing write log void the proof. */
    dsp_idle_note_write(s, CDJ_C6747_TIMER0_BASE + 0x10, 1, 4);
    assert(s->idle_dirty);
    dsp_idle_anchor(s, 0);
    assert(!s->idle_dirty && !s->idle_log_count);
    for (uint32_t i = 0; i < 64; ++i)
        dsp_idle_note_write(s, L2_BASE + 0x1000 + 4 * i, 1, 4);
    assert(!s->idle_dirty);
    dsp_idle_note_write(s, L2_BASE + 0x2000, 1, 4);
    assert(s->idle_dirty);

    /* Anything clocked by DSP cycles keeps the system from being quiescent. */
    s->timers[1].tgcr = 3; assert(dsp_idle_quiescent(s));
    s->timers[1].tcr = 0x40; assert(!dsp_idle_quiescent(s));
    s->timers[1].tgcr = 0; assert(dsp_idle_quiescent(s));
    s->psc.remaining[1][0] = 2; assert(!dsp_idle_quiescent(s));
    s->psc.remaining[1][0] = 0;
    s->pll.go_remaining = 1; assert(!dsp_idle_quiescent(s));
    s->pll.go_remaining = 0;
    s->spi_transfer.phase = 1; assert(!dsp_idle_quiescent(s));
    s->spi_transfer.phase = 0;
    s->functional_audio = true; assert(!dsp_idle_quiescent(s));
    s->functional_audio = false;
    assert(dsp_idle_quiescent(s));
    free(s->shared_ram); free(s->sdram); free(s);
    return 0;
}
'''
    fixture = tmp_path / 'idle.c'
    fixture.write_text(harness + prefix + span + idle + checks)
    binary = tmp_path / 'idle-test'
    models = sorted(directory.glob('cdj_c6747_*.c'))
    subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(directory), str(fixture), *map(str, models),
                    str(directory / 'cdj_c674x_loop.c'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
