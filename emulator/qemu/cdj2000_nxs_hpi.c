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

#define HPI_BASE 0x0c000000u
#define L2_BASE 0x11800000u
#define L2_SIZE 0x40000u

typedef struct {
    MemoryRegion registers;
    uint8_t l2[L2_SIZE];
    uint32_t address;
    bool hwob;
    bool dspint;
    uint64_t words;
    CdjC674x cpu;
    void (*hint)(void *, bool);
    void *opaque;
} NxsHpi;
static NxsHpi *nxs_hpi;

bool cdj_nxs_hpi_port(hwaddr address)
{
    return nxs_hpi && (address == HPI_BASE + 0x80000 || address == HPI_BASE + 0xc0000);
}

static bool valid_data(NxsHpi *s)
{
    return s->hwob && !(s->address & 3) && s->address >= L2_BASE &&
           s->address <= L2_BASE + L2_SIZE - 4;
}

static bool dsp_read(void *opaque, uint32_t address, uint32_t *value)
{
    NxsHpi *s = opaque;
    if (address >= 0x00800000 && address < 0x00840000) address += 0x11000000;
    if ((address & 3) || address < L2_BASE || address > L2_BASE + L2_SIZE - 4) return false;
    *value = ldl_le_p(s->l2 + address - L2_BASE);
    return true;
}

static bool dsp_write(void *opaque, uint32_t address, uint64_t value,
                      unsigned size, bool commit)
{
    NxsHpi *s = opaque;
    if (address >= 0x00800000 && address < 0x00840000) address += 0x11000000;
    if ((size != 1 && size != 2 && size != 4 && size != 8) || (address & (size - 1)) ||
        address < L2_BASE || address > L2_BASE + L2_SIZE - size) return false;
    if (commit) {
        if (size == 8) stq_le_p(s->l2 + address - L2_BASE, value);
        else if (size == 1) s->l2[address - L2_BASE] = value;
        else if (size == 2) stw_le_p(s->l2 + address - L2_BASE, value);
        else stl_le_p(s->l2 + address - L2_BASE, value);
    }
    return true;
}

static void start_dsp(NxsHpi *s)
{
    /* Boot-ROM handoff abstraction: the host supplies the entry in L2[0].
     * No claim to execute the unavailable ROM. The uploaded code is decoded. */
    cdj_c674x_reset(&s->cpu, ldl_le_p(s->l2));
    unsigned budget = 10000;
    while (budget-- && cdj_c674x_step(&s->cpu, dsp_read, dsp_write, s)) {}
    info_report("nxs-c674x: packets=%" PRIu64 " cycles=%" PRIu64
                " pc=%#x word=%#x stop=%s B15=%#x B14=%#x B3=%#x",
                s->cpu.packets, s->cpu.cycles, s->cpu.fault ? s->cpu.fault_pc : s->cpu.pc,
                s->cpu.fault_word, s->cpu.fault ? s->cpu.fault : "startup budget",
                s->cpu.r[1][15], s->cpu.r[1][14], s->cpu.r[1][3]);
}

static uint64_t hpi_read(void *opaque, hwaddr offset, unsigned size)
{
    NxsHpi *s = opaque;
    uint32_t result = 0xffffffff;
    if (offset == 0) return (s->hwob ? 0x01010101u : 0) | (s->dspint ? 0x00020002u : 0);
    if (offset == 0x40000) return s->address;
    if ((offset == 0x80000 || offset == 0xc0000) && valid_data(s)) {
        result = ldl_le_p(s->l2 + s->address - L2_BASE);
        if (offset == 0x80000) s->address += 4;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "nxs-hpi: unsupported read offset=%" HWADDR_PRIx " address=%#x HWOB=%d\n", offset, s->address, s->hwob);
    }
    return result;
}

static void hpi_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    NxsHpi *s = opaque;
    if (offset == 0) {
        s->hwob = value & 1;
        /* Host acknowledges HINT: the active-low wire returns high. */
        if ((value & 0x00040004) && s->hint) s->hint(s->opaque, true);
        if ((value & 0x00020002) && !s->dspint) {
            s->dspint = true;
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
        }
        return;
    }
    if (offset == 0x40000) { s->address = value; return; }
    if ((offset == 0x80000 || offset == 0xc0000) && valid_data(s)) {
        stl_le_p(s->l2 + s->address - L2_BASE, value);
        ++s->words;
        if (offset == 0x80000) s->address += 4;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "nxs-hpi: unsupported write offset=%" HWADDR_PRIx " address=%#x HWOB=%d\n", offset, s->address, s->hwob);
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
    s->hint = hint;
    s->opaque = opaque;
    memory_region_init_io(&s->registers, NULL, &hpi_ops, s, "nxs.uhpi", 0x100000);
    memory_region_add_subregion(system, HPI_BASE, &s->registers);
}
