/* SPDX-License-Identifier: GPL-2.0-or-later
 * NXS MAIN-facing TI UHPI transport. This is not a C674x CPU.
 * Matches the independently verified NXS upload path: byte-addressed global
 * L2, HWOB=1, HPID auto-increment and fixed-address accesses.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "cdj2000_nxs_hpi.h"

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
            info_report("nxs-hpi: DSPINT after %" PRIu64 " written words; C674x CPU not attached", s->words);
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
