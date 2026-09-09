/* SPDX-License-Identifier: GPL-2.0-or-later
 * Headless replay of an NXS pre-execution UHPI L2 dump. No boot ROM or
 * external processors run here; this is a deterministic core diagnostic. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include "cdj_c674x.h"
#include "cdj_c6747_syscfg.h"
#include "cdj_c6747_psc.h"
#include "cdj_c6747_mcasp.h"
#include "cdj_c6747_gpio.h"
#include "cdj_c6747_i2c.h"
#include "cdj_c6747_pll.h"
#include "cdj_c6747_hpi.h"
#include "cdj_c6747_emifb.h"
#include "cdj_dsp_checkpoint.h"
static uint8_t ram[0x40000];
static uint8_t sdram[0x2000000];
static CdjC6747Syscfg syscfg;
static CdjC6747Psc psc;
static CdjC6747Mcasp mcasp;
static CdjC6747Gpio gpio;
static CdjC6747I2c i2c;
static CdjC6747Pll pll;
static CdjC6747Hpi hpi;
static CdjC6747Emifb emifb;
static CdjC674x cpu;
static CdjDspCheckpointState checkpoint_state;

static void restore_devices(const CdjDspCheckpointState *state)
{
    cpu = state->cpu;
    syscfg = state->syscfg;
    psc = state->psc;
    mcasp = state->mcasp;
    gpio = state->gpio;
    i2c = state->i2c;
    pll = state->pll;
    hpi = state->hpi;
    emifb = state->emifb;
}

static void capture_devices(CdjDspCheckpointState *state, const char *reason)
{
    state->cpu = cpu;
    state->syscfg = syscfg;
    state->psc = psc;
    state->mcasp = mcasp;
    state->gpio = gpio;
    state->i2c = i2c;
    state->pll = pll;
    state->hpi = hpi;
    state->emifb = emifb;
    state->dsp_started = true;
    state->dsp_halted = cpu.fault != NULL;
    ++state->checkpoint_sequence;
    cdj_dsp_checkpoint_prepare(state, reason);
}
static void cycle_tick(void *unused)
{
    (void)unused;
    cdj_c6747_pll_tick(&pll);
}
static uint32_t global(uint32_t a)
{ return a >= 0x00800000 && a < 0x00840000 ? a + 0x11000000 : a; }
static bool read_bus(void *unused, uint32_t a, uint32_t *v)
{
    (void)unused;
    if (cdj_c6747_syscfg_read(&syscfg, a, v)) return true;
    if (cdj_c6747_psc_read(&psc, a, v)) return true;
    if (cdj_c6747_mcasp_read(&mcasp, a, v)) return true;
    if (cdj_c6747_gpio_read(&gpio, a, v)) return true;
    if (cdj_c6747_i2c_read(&i2c, a, v)) return true;
    if (cdj_c6747_pll_read(&pll, a, v)) return true;
    if (cdj_c6747_emifb_read(&emifb, a, v)) return true;
    if ((syscfg.cfgchip[1] & 0x8000) && cdj_c6747_hpi_cpu_read(&hpi, a, v)) return true;
    if (cdj_c6747_emifb_sdram_enabled(&emifb) && !(a & 3) &&
        a >= 0xc0000000 && a <= 0xc1fffffc) {
        unsigned offset = a - 0xc0000000;
        *v = sdram[offset] | (uint32_t)sdram[offset + 1] << 8 |
             (uint32_t)sdram[offset + 2] << 16 | (uint32_t)sdram[offset + 3] << 24;
        return true;
    }
    a = global(a);
    if ((a & 3) || a < 0x11800000 || a > 0x1183fffc) return false;
    a -= 0x11800000;
    *v = ram[a] | (uint32_t)ram[a+1] << 8 | (uint32_t)ram[a+2] << 16 | (uint32_t)ram[a+3] << 24;
    return true;
}
static bool write_bus(void *unused, uint32_t a, uint64_t v, unsigned size, bool commit)
{
    (void)unused;
    bool ok = cdj_c6747_syscfg_write(&syscfg, a, v, size, commit);
    if (!ok) ok = cdj_c6747_psc_write(&psc, a, v, size, commit);
    if (!ok) ok = cdj_c6747_mcasp_write(&mcasp, a, v, size, commit);
    if (!ok) ok = cdj_c6747_gpio_write(&gpio, a, v, size, commit);
    if (!ok) ok = cdj_c6747_i2c_write(&i2c, a, v, size, commit);
    if (!ok && cdj_c6747_syscfg_pll_locked(&syscfg) &&
        cdj_c6747_pll_write_mapped(a, size)) ok = true;
    if (!ok) ok = cdj_c6747_pll_write(&pll, a, v, size, commit);
    if (!ok) ok = cdj_c6747_emifb_write(&emifb, a, v, size, commit);
    if (!ok && (syscfg.cfgchip[1] & 0x8000))
        ok = cdj_c6747_hpi_cpu_write(&hpi, a, v, size, commit);
    if (!ok && cdj_c6747_emifb_sdram_enabled(&emifb) &&
        (size == 1 || size == 2 || size == 4 || size == 8) &&
        a >= 0xc0000000 && a <= 0xc2000000 - size) {
        ok = true;
        if (commit) for (unsigned i = 0; i < size; ++i)
            sdram[a - 0xc0000000 + i] = v >> (8 * i);
    }
    uint32_t physical = global(a);
    if (!ok && (size == 1 || size == 2 || size == 4 || size == 8) &&
        physical >= 0x11800000 && physical <= 0x11840000 - size) {
        ok = true;
        if (commit) for (unsigned i = 0; i < size; ++i)
            ram[physical - 0x11800000 + i] = v >> (8*i);
    }
    if (commit || !ok)
        printf("{\"event\":\"%s\",\"address\":%" PRIu32 ",\"value\":%" PRIu64 ",\"size\":%u}\n",
               ok ? "write" : "rejected_write", a, v, size);
    return ok;
}
int main(int argc, char **argv)
{
    if (argc != 5 && argc != 6) return 2;
    char *end;
    errno = 0;
    unsigned long long limit = strtoull(argv[2], &end, 10);
    if (errno || *end || !limit || argv[2][0] == '-') return 2;
    errno = 0;
    unsigned long long breakpoint = strtoull(argv[3], &end, 0);
    if (errno || *end || breakpoint > UINT32_MAX) return 2;
    errno = 0;
    unsigned long boot_phase = strtoul(argv[4], &end, 0);
    if (errno || *end || boot_phase > 7) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("dump"); return 2; }
    char magic[8];
    bool checkpoint = fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
                      memcmp(magic, "CDJDSP1\0", sizeof(magic)) == 0;
    rewind(f);
    bool valid = false;
    if (!checkpoint)
        valid = fread(ram, 1, sizeof(ram), f) == sizeof(ram) &&
                fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (checkpoint) {
        char error[160] = {0};
        if (!cdj_dsp_checkpoint_read(argv[1], &checkpoint_state, ram, sizeof(ram),
                                     sdram, sizeof(sdram), error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            return 2;
        }
        restore_devices(&checkpoint_state);
        /* A fault is recorded after an atomic rejected packet. Clearing only
         * its diagnostic latch retries the same PC against the current core. */
        cpu.fault = NULL;
        cpu.fault_pc = 0;
        cpu.fault_word = 0;
        printf("{\"event\":\"checkpoint_restore\",\"captured_reason\":\"%s\","
               "\"captured_fault\":\"%s\",\"event_sequence\":%" PRIu64 ","
               "\"checkpoint_sequence\":%" PRIu64 "}\n",
               checkpoint_state.stop_reason, checkpoint_state.fault,
               checkpoint_state.event_sequence, checkpoint_state.checkpoint_sequence);
    } else {
        if (!valid) { fputs("expected exactly 256 KiB of L2 or a compatible checkpoint\n", stderr); return 2; }
        cdj_c6747_syscfg_reset(&syscfg);
        cdj_c6747_psc_reset(&psc);
        cdj_c6747_mcasp_reset(&mcasp);
        cdj_c6747_gpio_reset(&gpio);
        cdj_c6747_i2c_reset(&i2c);
        cdj_c6747_pll_reset(&pll);
        cdj_c6747_hpi_reset(&hpi);
        cdj_c6747_emifb_reset(&emifb);
        cdj_c6747_gpio_set_input(&gpio, 4, 5, boot_phase & 1);
        cdj_c6747_gpio_set_input(&gpio, 4, 2, boot_phase & 2);
        cdj_c6747_gpio_set_input(&gpio, 4, 3, boot_phase & 4);
        cdj_c6747_hpi_rom_boot_ready(&hpi);
        cdj_c6747_hpi_host_write(&hpi, 0x01050105); /* MAIN acks ROM HINT and selects HWOB. */
        cdj_c6747_hpi_host_write(&hpi, 0x01030103); /* Captured dump precedes DSPINT. */
        uint32_t entry;
        read_bus(NULL, 0x11800000, &entry);
        cdj_c674x_reset(&cpu, entry);
        checkpoint_state.boot_phase = boot_phase;
        checkpoint_state.reset_released = checkpoint_state.dsp_started = true;
    }
    cpu.cycle_tick = cycle_tick;
    const char *reason = "step_limit";
    for (unsigned long long step = 0; step < limit; ++step) {
        if (breakpoint && cpu.pc == breakpoint) { reason = "breakpoint"; break; }
        printf("{\"event\":\"step\",\"pc\":%" PRIu32 ",\"cycles\":%" PRIu64
               ",\"loop_active\":%s,\"branch_due\":%" PRIu64 "}\n",
               cpu.pc, cpu.cycles, cpu.loop_active ? "true" : "false", cpu.branch_due);
        if (!cdj_c674x_step(&cpu, read_bus, write_bus, NULL)) { reason = "fault"; break; }
        cdj_c6747_psc_tick(&psc);
        if (hpi.hint) { reason = "host_event_required"; break; }
    }
    /* Fault strings originate in the interpreter and contain no JSON escapes. */
    printf("{\"event\":\"stop\",\"reason\":\"%s\",\"fault\":\"%s\",\"pc\":%" PRIu32
           ",\"fault_pc\":%" PRIu32 ",\"fault_word\":%" PRIu32
           ",\"packets\":%" PRIu64 ",\"cycles\":%" PRIu64 ",\"registers\":[",
           reason, cpu.fault ? cpu.fault : "", cpu.pc, cpu.fault_pc, cpu.fault_word,
           cpu.packets, cpu.cycles);
    for (unsigned bank = 0; bank < 2; ++bank) {
        printf("%s[", bank ? "," : "");
        for (unsigned i = 0; i < 32; ++i) printf("%s%" PRIu32, i ? "," : "", cpu.r[bank][i]);
        printf("]");
    }
    printf("],\"pending_stores\":%u,\"pending_loads\":%u,\"syscfg_unlocked\":%s,\"pll_legacy_bit4_used\":%s,"
           "\"pll_oscin_cycles\":%" PRIu64 ",\"pll_reset_age\":%u,\"pll_lock_wait_remaining\":%u,"
           "\"pll_early_enable\":%s,\"cfgchip\":[%u,%u,%u,%u],"
           "\"amute_clear_pulses\":%u,\"hpi\":{\"reset\":%s,\"hwob\":%s,"
           "\"dspint\":%s,\"hint\":%s},\"emifb\":{\"sdcfg\":%u,"
           "\"sdrfc\":%u,\"sdtim1\":%u,\"sdtim2\":%u,"
           "\"init_sequences\":%u}}\n",
           cpu.store_count, cpu.load_count, syscfg.unlocked ? "true" : "false",
           pll.legacy_bit4_used ? "true" : "false", pll.oscin_cycles,
           pll.reset_age, pll.lock_wait_remaining, pll.early_enable ? "true" : "false",
           syscfg.cfgchip[0], syscfg.cfgchip[1], syscfg.cfgchip[2],
           syscfg.cfgchip[3], syscfg.amute_clear_pulses,
           hpi.hpirst ? "true" : "false", hpi.hwob ? "true" : "false",
           hpi.dspint ? "true" : "false", hpi.hint ? "true" : "false",
           emifb.sdcfg, emifb.sdrfc, emifb.sdtim1, emifb.sdtim2,
           emifb.init_sequences);
    if (argc == 6) {
        char error[160] = {0};
        capture_devices(&checkpoint_state, reason);
        if (!cdj_dsp_checkpoint_write(argv[5], &checkpoint_state, ram, sizeof(ram),
                                      sdram, sizeof(sdram), error, sizeof(error))) {
            fprintf(stderr, "%s\n", error);
            return 2;
        }
    }
    return ferror(stdout) ? 2 : 0;
}
