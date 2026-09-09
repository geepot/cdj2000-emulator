/* SPDX-License-Identifier: GPL-2.0-or-later
 * NXS MAIN-facing TI UHPI transport with a partial C674x execution core.
 * Matches the independently verified NXS upload path: byte-addressed global
 * L2, HWOB=1, HPID auto-increment and fixed-address accesses.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "cdj2000_nxs_hpi.h"
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

#define HPI_BASE 0x0c000000u
#define L2_BASE 0x11800000u
#define L2_SIZE 0x40000u
#define SDRAM_BASE 0xc0000000u
#define SDRAM_SIZE 0x02000000u
/* Cooperative QEMU scheduling quantum, not a C6747 timing property. The
 * observed 32 KiB CPU-copy handshake completes in fewer than 9k packets. */
#define DSP_RUN_BUDGET 100000u

typedef struct {
    MemoryRegion registers;
    uint8_t l2[L2_SIZE];
    uint32_t address;
    CdjC6747Hpi hpi;
    bool reset_released, dsp_started, dsp_halted, dsp_running;
    unsigned boot_phase;
    uint64_t words;
    uint64_t event_sequence, checkpoint_sequence;
    CdjC674x cpu;
    CdjC6747Syscfg syscfg;
    CdjC6747Psc psc;
    CdjC6747Mcasp mcasp;
    CdjC6747Gpio gpio;
    CdjC6747I2c i2c;
    CdjC6747Pll pll;
    CdjC6747Emifb emifb;
    uint8_t *sdram;
    void (*hint)(void *, bool);
    void *opaque;
    FILE *event_log;
} NxsHpi;
static NxsHpi *nxs_hpi;
static void run_dsp(NxsHpi *s);

static void record_event(NxsHpi *s, const char *type, uint64_t offset,
                         uint64_t address, uint64_t value, unsigned size)
{
    const char *path = getenv("CDJ_NXS_DSP_EVENTS");
    if (!path || !*path) return;
    if (!s->event_log) {
        s->event_log = fopen(path, "a");
        if (!s->event_log) {
            error_report("nxs-hpi: cannot open DSP event transcript %s", path);
            return;
        }
    }
    ++s->event_sequence;
    if (fprintf(s->event_log,
                "{\"sequence\":%" PRIu64 ",\"event\":\"%s\","
                "\"offset\":%" PRIu64 ",\"address\":%" PRIu64 ","
                "\"value\":%" PRIu64 ",\"size\":%u,\"boot_phase\":%u,"
                "\"hint\":%s,\"dspint\":%s,\"packets\":%" PRIu64 ","
                "\"cycles\":%" PRIu64 "}\n",
                s->event_sequence, type, offset, address, value, size,
                s->boot_phase, s->hpi.hint ? "true" : "false",
                s->hpi.dspint ? "true" : "false", s->cpu.packets,
                s->cpu.cycles) < 0 || fflush(s->event_log))
        error_report("nxs-hpi: DSP event transcript write failed");
}

static void capture_checkpoint(NxsHpi *s, const char *reason)
{
    const char *directory = getenv("CDJ_NXS_DSP_CHECKPOINT_DIR");
    if (!directory || !*directory || !s->sdram) return;
    if (g_mkdir_with_parents(directory, 0700)) {
        error_report("nxs-hpi: cannot create DSP checkpoint directory %s", directory);
        return;
    }
    CdjDspCheckpointState state = {0};
    state.hpi_address = s->address;
    state.boot_phase = s->boot_phase;
    state.words = s->words;
    state.event_sequence = s->event_sequence;
    state.checkpoint_sequence = ++s->checkpoint_sequence;
    state.reset_released = s->reset_released;
    state.dsp_started = s->dsp_started;
    state.dsp_halted = s->dsp_halted;
    state.cpu = s->cpu;
    state.syscfg = s->syscfg;
    state.psc = s->psc;
    state.mcasp = s->mcasp;
    state.gpio = s->gpio;
    state.i2c = s->i2c;
    state.pll = s->pll;
    state.hpi = s->hpi;
    state.emifb = s->emifb;
    cdj_dsp_checkpoint_prepare(&state, reason);
    g_autofree char *name = g_strdup_printf("%020" PRIu64 ".cdjdsp",
                                             state.checkpoint_sequence);
    g_autofree char *path = g_build_filename(directory, name, NULL);
    char error[160] = {0};
    if (!cdj_dsp_checkpoint_write(path, &state, s->l2, sizeof(s->l2),
                                  s->sdram, SDRAM_SIZE, error, sizeof(error)))
        error_report("nxs-hpi: checkpoint failed: %s", error);
    else
        info_report("nxs-hpi: checkpoint=%s reason=%s event-sequence=%" PRIu64,
                    path, reason, state.event_sequence);
}

bool cdj_nxs_hpi_port(hwaddr address)
{
    return nxs_hpi && (address == HPI_BASE + 0x80000 || address == HPI_BASE + 0xc0000);
}

void cdj_nxs_hpi_reset_line(bool released)
{
    NxsHpi *s = nxs_hpi;

    if (!s || released == s->reset_released) return;
    s->reset_released = released;
    if (!released) {
        /* External DSP reset coverage is intentionally limited to the HPI
         * boot contract and interpreter lifecycle. Other peripheral reset
         * domains remain explicit models with their own reset entry points. */
        cdj_c6747_hpi_reset(&s->hpi);
        s->dsp_started = s->dsp_halted = s->dsp_running = false;
        if (s->hint) s->hint(s->opaque, true);
        record_event(s, "reset_assert", 0, 0, 0, 0);
        info_report("nxs-hpi: DSP reset asserted; HPI boot state reset");
        return;
    }

    /* The on-chip ROM itself is not executed. TI SPRABB1C section 4.1 says
     * its HPI boot path sets HINT when ready for the host download. This is
     * the sole ROM handoff abstraction; uploaded firmware remains genuine. */
    cdj_c6747_hpi_rom_boot_ready(&s->hpi);
    if (s->hint) s->hint(s->opaque, false);
    record_event(s, "rom_hpi_ready", 0, 0, 1, 0);
    info_report("nxs-hpi: DSP reset released; ROM HPI-ready HINT asserted");
}

void cdj_nxs_hpi_boot_phase(unsigned phase)
{
    NxsHpi *s = nxs_hpi;

    if (!s || phase > 7) return;
    bool changed = phase != s->boot_phase;
    s->boot_phase = phase;
    /* Schematic-confirmed CPU_PH0/1/2 reach GP4[5]/GP4[2]/GP4[3]. */
    cdj_c6747_gpio_set_input(&s->gpio, 4, 5, phase & 1);
    cdj_c6747_gpio_set_input(&s->gpio, 4, 2, phase & 2);
    cdj_c6747_gpio_set_input(&s->gpio, 4, 3, phase & 4);
    info_report("nxs-hpi: MAIN boot phase=%u -> DSP GP4 inputs=%#x",
                phase, s->gpio.input[2] & 0x2c);
    record_event(s, "boot_phase", 0, 0, phase, 0);
    /* A phase-budget yield is a cooperative scheduling boundary, not a DSP
     * halt.  Resume when the genuine MAIN firmware changes the sideband that
     * the DSP is polling. */
    if (changed) {
        if (s->dsp_started && !s->dsp_halted)
            capture_checkpoint(s, "boot-phase boundary");
        run_dsp(s);
    }
}

static bool valid_data(NxsHpi *s)
{
    return s->hpi.hwob && !s->hpi.hpirst && !(s->address & 3) && s->address >= L2_BASE &&
           s->address <= L2_BASE + L2_SIZE - 4;
}

static bool dsp_read(void *opaque, uint32_t address, uint32_t *value)
{
    NxsHpi *s = opaque;
    if (cdj_c6747_syscfg_read(&s->syscfg, address, value)) return true;
    if (cdj_c6747_psc_read(&s->psc, address, value)) return true;
    if (cdj_c6747_mcasp_read(&s->mcasp, address, value)) return true;
    if (cdj_c6747_gpio_read(&s->gpio, address, value)) return true;
    if (cdj_c6747_i2c_read(&s->i2c, address, value)) return true;
    if (cdj_c6747_pll_read(&s->pll, address, value)) return true;
    if (cdj_c6747_emifb_read(&s->emifb, address, value)) return true;
    if ((s->syscfg.cfgchip[1] & 0x8000) &&
        cdj_c6747_hpi_cpu_read(&s->hpi, address, value)) return true;
    if (cdj_c6747_emifb_sdram_enabled(&s->emifb) && !(address & 3) &&
        address >= SDRAM_BASE && address <= SDRAM_BASE + SDRAM_SIZE - 4) {
        *value = ldl_le_p(s->sdram + address - SDRAM_BASE);
        return true;
    }
    if (address >= 0x00800000 && address < 0x00840000) address += 0x11000000;
    if ((address & 3) || address < L2_BASE || address > L2_BASE + L2_SIZE - 4) return false;
    *value = ldl_le_p(s->l2 + address - L2_BASE);
    return true;
}

static bool dsp_write(void *opaque, uint32_t address, uint64_t value,
                      unsigned size, bool commit)
{
    NxsHpi *s = opaque;
    if (cdj_c6747_syscfg_pll_locked(&s->syscfg) &&
        cdj_c6747_pll_write_mapped(address, size)) {
        if (commit) info_report("nxs-pll: locked write ignored address=%#x value=%#x",
                                address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_pll_write(&s->pll, address, value, size, commit)) {
        if (commit) info_report("nxs-pll: write address=%#x value=%#x legacy-bit4-assumption=%d",
                                address, (uint32_t)value, s->pll.legacy_bit4_used);
        return true;
    }
    if (cdj_c6747_i2c_write(&s->i2c, address, value, size, commit)) {
        if (commit) info_report("nxs-i2c: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_gpio_write(&s->gpio, address, value, size, commit)) {
        if (commit) info_report("nxs-gpio: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_mcasp_write(&s->mcasp, address, value, size, commit)) {
        if (commit) info_report("nxs-mcasp: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_psc_write(&s->psc, address, value, size, commit)) {
        if (commit) info_report("nxs-psc: write address=%#x value=%#x", address, (uint32_t)value);
        return true;
    }
    if (cdj_c6747_syscfg_write(&s->syscfg, address, value, size, commit)) {
        if (commit) info_report("nxs-syscfg: write address=%#x value=%#x unlocked=%d",
                                address, (uint32_t)value, s->syscfg.unlocked);
        return true;
    }
    if (cdj_c6747_emifb_write(&s->emifb, address, value, size, commit)) {
        if (commit) info_report("nxs-emifb: write address=%#x value=%#x init-sequences=%u",
                                address, (uint32_t)value, s->emifb.init_sequences);
        return true;
    }
    if ((s->syscfg.cfgchip[1] & 0x8000)) {
        bool old_hint = s->hpi.hint;
        if (cdj_c6747_hpi_cpu_write(&s->hpi, address, value, size, commit)) {
            if (commit) {
                info_report("nxs-hpi: DSP HPIC write value=%#x DSPINT=%d HINT=%d",
                            (uint32_t)value, s->hpi.dspint, s->hpi.hint);
                record_event(s, "dsp_hpic_write", 0, address, value, size);
                if (!old_hint && s->hpi.hint && s->hint) s->hint(s->opaque, false);
            }
            return true;
        }
    }
    if (cdj_c6747_emifb_sdram_enabled(&s->emifb) &&
        (size == 1 || size == 2 || size == 4 || size == 8) &&
        address >= SDRAM_BASE && address <= SDRAM_BASE + SDRAM_SIZE - size) {
        if (commit) {
            if (size == 8) stq_le_p(s->sdram + address - SDRAM_BASE, value);
            else if (size == 1) s->sdram[address - SDRAM_BASE] = value;
            else if (size == 2) stw_le_p(s->sdram + address - SDRAM_BASE, value);
            else stl_le_p(s->sdram + address - SDRAM_BASE, value);
        }
        return true;
    }
    if (address >= 0x00800000 && address < 0x00840000) address += 0x11000000;
    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        address < L2_BASE || address > L2_BASE + L2_SIZE - size) return false;
    if (commit) {
        if (size == 8) stq_le_p(s->l2 + address - L2_BASE, value);
        else if (size == 1) s->l2[address - L2_BASE] = value;
        else if (size == 2) stw_le_p(s->l2 + address - L2_BASE, value);
        else stl_le_p(s->l2 + address - L2_BASE, value);
    }
    return true;
}

static void dsp_cycle_tick(void *opaque)
{
    NxsHpi *s = opaque;
    cdj_c6747_pll_tick(&s->pll);
}

static void report_dsp(NxsHpi *s, const char *reason)
{
    info_report("nxs-c674x: packets=%" PRIu64 " cycles=%" PRIu64
                " pc=%#x word=%#x stop=%s B15=%#x B14=%#x B3=%#x",
                s->cpu.packets, s->cpu.cycles, s->cpu.fault ? s->cpu.fault_pc : s->cpu.pc,
                s->cpu.fault_word, reason, s->cpu.r[1][15], s->cpu.r[1][14], s->cpu.r[1][3]);
    record_event(s, "dsp_stop", 0, s->cpu.fault ? s->cpu.fault_pc : s->cpu.pc,
                 s->cpu.fault_word, 0);
    capture_checkpoint(s, reason);
}

static void run_dsp(NxsHpi *s)
{
    if (!s->dsp_started || s->dsp_halted || s->dsp_running) return;
    s->dsp_running = true;
    const char *reason = "phase budget exhausted";
    unsigned steps = 0;
    for (; steps < DSP_RUN_BUDGET; ++steps) {
        if (!cdj_c674x_step(&s->cpu, dsp_read, dsp_write, s)) {
            reason = s->cpu.fault ? s->cpu.fault : "CPU stopped";
            s->dsp_halted = true;
            break;
        }
        cdj_c6747_psc_tick(&s->psc);
        if (s->hpi.hint) { reason = "HINT host-event yield"; break; }
    }
    s->dsp_running = false;
    report_dsp(s, reason);
}

static void start_dsp(NxsHpi *s)
{
    /* Boot-ROM handoff abstraction: the host supplies the entry in L2[0].
     * No claim to execute the unavailable ROM. The uploaded code is decoded. */
    cdj_c674x_reset(&s->cpu, ldl_le_p(s->l2));
    s->cpu.cycle_tick = dsp_cycle_tick;
    s->cpu.cycle_opaque = s;
    s->dsp_started = true;
    record_event(s, "dsp_start", 0, s->cpu.pc, 0, 0);
    capture_checkpoint(s, "DSP start boundary");
    run_dsp(s);
    info_report("nxs-pll: oscin-cycles=%" PRIu64 " reset-age=%u lock-wait-remaining=%u early-enable=%d",
                s->pll.oscin_cycles, s->pll.reset_age, s->pll.lock_wait_remaining,
                s->pll.early_enable);
    info_report("nxs-syscfg: cfgchip=%#x,%#x,%#x,%#x amute-clear-pulses=%#x",
                s->syscfg.cfgchip[0], s->syscfg.cfgchip[1], s->syscfg.cfgchip[2],
                s->syscfg.cfgchip[3], s->syscfg.amute_clear_pulses);
}

static uint64_t hpi_read(void *opaque, hwaddr offset, unsigned size)
{
    NxsHpi *s = opaque;
    uint32_t result = 0xffffffff;
    uint32_t address = s->address;
    bool data_access = offset == 0x80000 || offset == 0xc0000;
    bool data_valid = data_access && valid_data(s);
    if (offset == 0) result = cdj_c6747_hpi_host_read(&s->hpi);
    else if (offset == 0x40000) result = s->address;
    if (data_valid) {
        result = ldl_le_p(s->l2 + s->address - L2_BASE);
        if (offset == 0x80000) s->address += 4;
    } else if (offset != 0 && offset != 0x40000) {
        qemu_log_mask(LOG_GUEST_ERROR, "nxs-hpi: unsupported read offset=%" HWADDR_PRIx " address=%#x HWOB=%d reset=%d\n",
                      offset, s->address, s->hpi.hwob, s->hpi.hpirst);
    }
    /* HPIC polling is DSP-to-MAIN observation, not an external stimulus, and
     * produced hundreds of thousands of redundant records. Data-port reads do
     * advance HPIA and therefore remain ordered state-changing events. */
    if (data_valid)
        record_event(s, "hpi_host_data_read", offset, address, result, size);
    return result;
}

static void hpi_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    NxsHpi *s = opaque;
    uint32_t address = s->address;
    if (offset == 0) {
        bool old_hint = s->hpi.hint, old_dspint = s->hpi.dspint;
        cdj_c6747_hpi_host_write(&s->hpi, value);
        record_event(s, "hpi_host_control_write", offset, address, value, size);
        if (old_hint != s->hpi.hint && s->hint) s->hint(s->opaque, !s->hpi.hint);
        if (!old_dspint && s->hpi.dspint && !s->dsp_started) {
            info_report("nxs-hpi: DSPINT after %" PRIu64 " written words; starting partial C674x interpreter", s->words);
            const char *path = getenv("CDJ_NXS_HPI_DUMP");
            if (path && *path) {
                FILE *file = fopen(path, "wb");
                if (!file) error_report("nxs-hpi: cannot open L2 dump %s", path);
                else {
                    size_t count = fwrite(s->l2, 1, sizeof(s->l2), file);
                    int status = fclose(file);
                    if (count != sizeof(s->l2) || status) error_report("nxs-hpi: incomplete L2 dump");
                }
            }
            start_dsp(s);
        } else if (old_hint && !s->hpi.hint) {
            run_dsp(s);
        } else if (!old_dspint && s->hpi.dspint && s->dsp_started) {
            error_report("nxs-hpi: later DSPINT pending; C674x interrupt delivery not implemented");
        }
        return;
    }
    if (offset == 0x40000) {
        s->address = value;
        record_event(s, "hpi_host_address_write", offset, address, value, size);
        return;
    }
    if ((offset == 0x80000 || offset == 0xc0000) && valid_data(s)) {
        stl_le_p(s->l2 + s->address - L2_BASE, value);
        ++s->words;
        if (offset == 0x80000) s->address += 4;
        record_event(s, offset == 0x80000 ? "hpi_host_data_autoincrement_write" :
                     "hpi_host_data_fixed_write", offset, address, value, size);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "nxs-hpi: unsupported write offset=%" HWADDR_PRIx " address=%#x HWOB=%d reset=%d\n",
                      offset, s->address, s->hpi.hwob, s->hpi.hpirst);
    }
}

static const MemoryRegionOps hpi_ops = {
    .read = hpi_read, .write = hpi_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void cdj_nxs_hpi_init(MemoryRegion *system, void (*hint)(void *, bool), void *opaque)
{
    NxsHpi *s = g_new0(NxsHpi, 1);
    nxs_hpi = s;
    cdj_c6747_syscfg_reset(&s->syscfg);
    cdj_c6747_psc_reset(&s->psc);
    cdj_c6747_mcasp_reset(&s->mcasp);
    cdj_c6747_gpio_reset(&s->gpio);
    cdj_c6747_i2c_reset(&s->i2c);
    cdj_c6747_pll_reset(&s->pll);
    cdj_c6747_hpi_reset(&s->hpi);
    cdj_c6747_emifb_reset(&s->emifb);
    s->sdram = g_malloc0(SDRAM_SIZE);
    s->hint = hint;
    s->opaque = opaque;
    if (s->hint) s->hint(s->opaque, true); /* UHPI_HINT is active low and idle high. */
    memory_region_init_io(&s->registers, NULL, &hpi_ops, s, "nxs.uhpi", 0x100000);
    memory_region_add_subregion(system, HPI_BASE, &s->registers);
}
