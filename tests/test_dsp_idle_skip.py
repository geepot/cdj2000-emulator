"""Compile the NXS board's idle-loop skip proof against its real peripheral models."""
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
    start = source.index('/*\n * Idle-loop skip.')
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
#define MIN(a, b) ((a) < (b) ? (a) : (b))
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
    dsp_idle_anchor(s, 0, true);
    assert(dsp_idle_repeat(s));

    /* Registers, PC and in-flight work must all match. */
    s->cpu.r[0][10] = 1; assert(!dsp_idle_repeat(s)); s->cpu.r[0][10] = 0;
    s->cpu.pc += 4; assert(!dsp_idle_repeat(s)); s->cpu.pc -= 4;
    s->cpu.load_count = 1; assert(!dsp_idle_repeat(s)); s->cpu.load_count = 0;
    assert(dsp_idle_repeat(s));

    /* A delayed-control cycle may only stay put or move by exactly one period
     * (10 cycles here) and be passed; the loop-context slot must not change. */
    s->cpu.cycles = 30; s->cpu.control_ready[5] = 20; assert(dsp_idle_repeat(s));
    s->cpu.control_ready[5] = 15; assert(!dsp_idle_repeat(s));
    s->cpu.control_ready[5] = 10; assert(dsp_idle_repeat(s));
    s->cpu.control_ready[31] = 8; assert(!dsp_idle_repeat(s));
    s->cpu.control_ready[31] = 7;

    /* Skipping k periods is k periods of counters, delayed cycles and PLL. */
    s->cpu.packets = 7;
    s->idle_anchor_packets = 3;           /* period: 4 packets, 10 cycles */
    s->cpu.control_ready[5] = 20;
    assert(dsp_idle_periods(s, 41) == 10);
    /* Dead queue slots: unchanged, or only `due` moved by one period. */
    assert(dsp_idle_queues_periodic(s));
    s->cpu.loads[4].due = s->idle_anchor_loads[4].due + 10;
    assert(dsp_idle_queues_periodic(s));
    s->cpu.loads[4].value = 1; assert(!dsp_idle_queues_periodic(s));
    s->cpu.loads[4].value = 0;
    s->cpu.stores[2].due = s->idle_anchor_stores[2].due + 9;
    assert(!dsp_idle_queues_periodic(s));
    s->cpu.stores[2].due = s->idle_anchor_stores[2].due;
    CdjC6747Pll pll = s->pll;
    for (unsigned i = 0; i < 30; ++i) cdj_c6747_pll_tick(&pll);
    dsp_idle_skip(s, 3);
    assert(s->cpu.packets == 19 && s->cpu.cycles == 60);
    assert(s->cpu.control_ready[5] == 50 && s->cpu.control_ready[31] == 7);
    assert(s->cpu.loads[4].due == s->idle_anchor_loads[4].due + 40);
    assert(s->cpu.loads[3].due == s->idle_anchor_loads[3].due);
    assert(!memcmp(&pll, &s->pll, sizeof(pll)));
    s->cpu.loads[4].due = s->idle_anchor_loads[4].due;
    s->cpu.packets = 7; s->cpu.cycles = 30; s->cpu.control_ready[5] = 10;

    /* A running functional McASP transmitter bounds the skip to its next
     * slot edge: packet-interval mode at packets % 1024, cycle clock at
     * mcasp_next_cycle. */
    s->functional_audio = true;
    assert(dsp_idle_quiescent(s));
    assert(dsp_idle_periods(s, 100000) == 25000);  /* transmitter idle */
    s->mcasp_control.gblctl[2] = 0x1f00;
    s->cpu.packets = 1000; s->idle_anchor_packets = 996;
    assert(dsp_idle_periods(s, 100000) == 5);      /* 1000 + 5 * 4 < 1024 */
    s->cpu.packets = 1020; s->idle_anchor_packets = 1016;
    assert(dsp_idle_periods(s, 100000) == 0);
    s->mcasp_control.gblctl[2] = 0;
    s->cycle_audio_clock = true;
    s->mcasp_control.gblctl[1] = 0x1f00;
    s->mcasp_next_cycle = 0; assert(dsp_idle_periods(s, 100000) == 0);
    s->mcasp_next_cycle = 101; assert(dsp_idle_periods(s, 100000) == 7);
    s->mcasp_control.gblctl[1] = 0; s->cycle_audio_clock = false;
    s->functional_audio = false;
    s->cpu.packets = 7; s->idle_anchor_packets = 3;

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
    dsp_idle_anchor(s, 0, true);
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
