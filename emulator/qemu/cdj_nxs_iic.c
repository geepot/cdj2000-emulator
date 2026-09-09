/* SPDX-License-Identifier: GPL-2.0-or-later
 * NXS SH7764 IIC adapter. Single-master, identity-only attached endpoint.
 * No certificates, signing, authentication-success or firmware bypass.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/reset.h"
#include "cdj_nxs_iic.h"
#include "cdj_sh7764_iic.h"
#include "cdj_mfi_identity.h"

typedef struct NxsIic {
    MemoryRegion io, area7;
    QEMUTimer *timer;
    CdjSh7764Iic core;
    uint8_t reg;
    bool selected, reading, transferred;
} NxsIic;

static bool identity_start(void *opaque, uint8_t address, bool read)
{
    NxsIic *s = opaque;
    if (address != 0x10) {
        return false;
    }
    s->reading = read;
    s->transferred = false;
    return true;
}

static bool identity_write(void *opaque, uint8_t byte)
{
    NxsIic *s = opaque;
    if (s->reading || s->transferred || byte > 1) {
        error_report("nxs-iic: unsupported identity write %#x", byte);
        exit(1);
    }
    s->reg = byte;
    s->selected = s->transferred = true;
    return true;
}

static bool identity_read(void *opaque, uint8_t *value)
{
    NxsIic *s = opaque;
    if (!s->reading || !s->selected || s->transferred) {
        return false;
    }
    if (!cdj_mfi_identity_read(s->reg, 1, value)) {
        return false;
    }
    s->transferred = true;
    qemu_log("nxs-iic: identity register=%#x value=%#x\n", s->reg, *value);
    return true;
}

static void identity_stop(void *opaque)
{
    /* Register selection survives STOP: Apple's POSIX driver uses separate
     * register-ID write and read transactions. */
}

static const CdjSh7764IicEndpoint endpoint = {
    .start = identity_start, .write_byte = identity_write,
    .read_byte = identity_read, .stop = identity_stop,
};

static void schedule(NxsIic *s)
{
    if (!cdj_sh7764_iic_event_pending(&s->core) || timer_pending(s->timer)) {
        return;
    }
    /* Service schematic X2=26.975MHz, mode3 Pck=2*EXTAL. Parts list instead
     * says26.965MHz; retain this unresolved board discrepancy. Address/byte
     * events aggregate nine SCL periods. STOP uses one period: functional
     * event-level approximation, not verified pin/START/STOP latency. Never
     * inherit accelerated TMU clocks or progress on register reads. */
    uint64_t periods = s->core.phase == CDJ_IIC_STOP ? 1 : 9;
    uint64_t ns = (periods * cdj_sh7764_iic_scl_period(&s->core) *
                   1000000000ULL + 53950000 - 1) / 53950000;
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void event(void *opaque)
{
    NxsIic *s = opaque;
    qemu_log("nxs-iic: event time=%" PRId64 " phase=%u status=%#x control=%#x\n",
             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
             s->core.phase, s->core.status, s->core.control);
    if (!cdj_sh7764_iic_advance(&s->core)) {
        error_report("nxs-iic: unsupported protocol event phase=%u", s->core.phase);
        exit(1);
    }
    schedule(s);
}

static void synchronize(NxsIic *s)
{
    /* vCPU polling may precede the I/O thread's timer dispatch. Publish an
     * already-due event before MMIO observes state; never advance virtual
     * time or expire a future event merely because firmware polls. */
    if (timer_pending(s->timer) && timer_expire_time_ns(s->timer) <=
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        timer_del(s->timer);
        event(s);
    }
}

static uint64_t read_reg(void *opaque, hwaddr offset, unsigned size)
{
    NxsIic *s = opaque;
    uint32_t value;
    synchronize(s);
    if (!cdj_sh7764_iic_read(&s->core, offset, size, &value)) {
        error_report("nxs-iic: unsupported read offset=%#" PRIx64 " size=%u", offset, size);
        exit(1);
    }
    return value;
}

static void write_reg(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    NxsIic *s = opaque;
    synchronize(s);
    qemu_log("nxs-iic: write time=%" PRId64 " offset=%#" PRIx64 " value=%#" PRIx64
             " phase=%u status=%#x\n", qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
             offset, value, s->core.phase, s->core.status);
    if (!cdj_sh7764_iic_write(&s->core, offset, size, value)) {
        error_report("nxs-iic: unsupported write offset=%#" PRIx64
                     " value=%#" PRIx64 " size=%u", offset, value, size);
        exit(1);
    }
    schedule(s);
}

static const MemoryRegionOps ops = {
    .read = read_reg, .write = write_reg, .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void reset(void *opaque)
{
    NxsIic *s = opaque;
    timer_del(s->timer);
    cdj_sh7764_iic_reset(&s->core);
    cdj_sh7764_iic_attach(&s->core, &endpoint, s);
    s->reg = 0;
    s->selected = s->reading = s->transferred = false;
}

void cdj_nxs_iic_init(MemoryRegion *system)
{
    NxsIic *s = g_new0(NxsIic, 1);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, event, s);
    reset(s);
    memory_region_init_io(&s->io, NULL, &ops, s, "nxs.sh7764-iic", 0x28);
    memory_region_add_subregion_overlap(system, 0xffe70000, &s->io, 1);
    /* Manual Table16.2: these are NOT related by the generic P4ADDR macro. */
    memory_region_init_alias(&s->area7, NULL, "nxs.sh7764-iic-area7", &s->io, 0, 0x28);
    memory_region_add_subregion_overlap(system, 0x1ff70000, &s->area7, 1);
    qemu_register_reset(reset, s);
}
