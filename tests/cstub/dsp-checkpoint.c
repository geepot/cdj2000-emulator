#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdj_dsp_checkpoint.h"

static uint8_t l2[CDJ_DSP_L2_SIZE], restored_l2[CDJ_DSP_L2_SIZE];
static uint8_t shared_ram[CDJ_DSP_SHARED_RAM_SIZE];
static uint8_t restored_shared_ram[CDJ_DSP_SHARED_RAM_SIZE];
static uint8_t sdram[CDJ_DSP_SDRAM_SIZE], restored_sdram[CDJ_DSP_SDRAM_SIZE];

static void unused_tick(void *opaque) { (void)opaque; }

#define CHECKPOINT_COMPONENTS 9u
typedef struct {
    char magic[8];
    uint32_t schema, endian, header_size, state_size;
    uint32_t component_size[CHECKPOINT_COMPONENTS];
    uint32_t l2_size, sdram_size, page_size, page_count, present_pages;
    uint64_t payload_size, payload_checksum;
} TestCheckpointHeader;

static uint64_t test_checksum(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void downgrade_to_schema10(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long end = ftell(file);
    assert(end > 0 && fseek(file, 0, SEEK_SET) == 0);
    size_t size = (size_t)end;
    uint8_t *bytes = malloc(size);
    assert(bytes && fread(bytes, 1, size, file) == size && fclose(file) == 0);

    TestCheckpointHeader *header = (TestCheckpointHeader *)bytes;
    assert(header->header_size == sizeof(*header) &&
           header->schema == 11 &&
           header->state_size == sizeof(CdjDspCheckpointState) &&
           header->payload_size == size - sizeof(*header));
    size_t alignment = _Alignof(CdjDspCheckpointState);
    size_t schema10_state_size = (offsetof(CdjDspCheckpointState, scheduler) +
                                  alignment - 1) / alignment * alignment;
    assert(schema10_state_size <= header->state_size);
    size_t removed = header->state_size - schema10_state_size;
    memmove(bytes + sizeof(*header) + schema10_state_size,
            bytes + sizeof(*header) + header->state_size,
            header->payload_size - header->state_size);
    memcpy(header->magic, "CDJDSP10", sizeof(header->magic));
    header->schema = 10;
    header->state_size = schema10_state_size;
    header->component_size[8] -= sizeof(CdjDspScheduler);
    header->payload_size -= removed;
    header->payload_checksum = test_checksum(bytes + sizeof(*header),
                                             header->payload_size);
    size -= removed;

    file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, size, file) == size &&
           fflush(file) == 0 && fclose(file) == 0);
    free(bytes);
}

static void downgrade_schema10_to_schema9(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long end = ftell(file);
    assert(end > 0 && fseek(file, 0, SEEK_SET) == 0);
    size_t size = (size_t)end;
    uint8_t *bytes = malloc(size);
    assert(bytes && fread(bytes, 1, size, file) == size && fclose(file) == 0);

    TestCheckpointHeader *header = (TestCheckpointHeader *)bytes;
    size_t alignment = _Alignof(CdjDspCheckpointState);
    size_t schema10_state_size = (offsetof(CdjDspCheckpointState, scheduler) +
                                  alignment - 1) / alignment * alignment;
    assert(header->header_size == sizeof(*header) &&
           header->schema == 10 && header->state_size == schema10_state_size &&
           header->payload_size == size - sizeof(*header));
    size_t schema9_state_size = (offsetof(CdjDspCheckpointState, spi_transfer) +
                                 alignment - 1) / alignment * alignment;
    assert(schema9_state_size <= header->state_size);
    size_t removed = header->state_size - schema9_state_size;
    memmove(bytes + sizeof(*header) + schema9_state_size,
            bytes + sizeof(*header) + header->state_size,
            header->payload_size - header->state_size);
    memcpy(header->magic, "CDJDSP9\0", sizeof(header->magic));
    header->schema = 9;
    header->state_size = schema9_state_size;
    header->component_size[8] -= sizeof(CdjC6747SpiTransfer);
    header->payload_size -= removed;
    header->payload_checksum = test_checksum(bytes + sizeof(*header),
                                             header->payload_size);
    size -= removed;

    file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, size, file) == size &&
           fflush(file) == 0 && fclose(file) == 0);
    free(bytes);
}

static void assert_schema8_peripheral_tail_preserved(
    const CdjDspCheckpointState *state)
{
    assert(state->mcasp_control.gblctl[1] == 0x1f00 &&
           state->mcasp_control.xfmt[1] == 0xf2 &&
           state->mcasp_control.srctl[1][3] == 1 &&
           state->mcasp_control.xrdy[1] == (1u << 3));
    assert(state->edma.drae[1] == 0x28 &&
           state->edma.param[3][0] == 0x00103200 &&
           state->edma.param[3][5] == 0x00024400 &&
           state->edma.irq_notifications == (1u << 2));
    assert(state->syscfg_priority.mstpri[0] == 0x44442122 &&
           state->syscfg_priority.mstpri[1] == 0x44442000 &&
           state->syscfg_priority.mstpri[2] == 0x54604404);
    assert(state->intc_delivery.cpu_request == (1u << 8));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(offsetof(CdjDspCheckpointState, scheduler) +
           sizeof(CdjDspScheduler) == sizeof(CdjDspCheckpointState));
    CdjDspCheckpointState before = {0}, after = {0};
    before.hpi_address = 0x11801234;
    before.boot_phase = 3;
    before.words = 12345;
    before.event_sequence = 456;
    before.checkpoint_sequence = 7;
    before.reset_released = before.dsp_started = before.dsp_halted = 1;
    before.cpu.pc = 0x11804468;
    before.cpu.cycles = 6132079;
    before.cpu.packets = 2599580;
    before.cpu.branch_due = 6132081;
    before.cpu.branch_target = 0x11805500;
    before.cpu.branch_count = 2;
    before.cpu.branch_queue[1].due = 6132082;
    before.cpu.branch_queue[1].target = 0x11806600;
    before.cpu.store_count = 1;
    before.cpu.stores[0].due = 6132080;
    before.cpu.stores[0].address = 0xc0001000;
    before.cpu.stores[0].value = UINT64_C(0x123456789abcdef0);
    before.cpu.stores[0].size = 8;
    before.cpu.load_count = 5;
    before.cpu.loads[4].due = 6132085;
    before.cpu.loads[4].address = 0xc000101f;
    before.cpu.loads[4].size = 8 | (5 << 8);
    before.cpu.loads[4].dst = 10;
    before.cpu.store_count = 2;
    before.cpu.stores[1].due = 6132083;
    before.cpu.stores[1].address = 0xc000103e;
    before.cpu.stores[1].value = 0x12345678;
    before.cpu.stores[1].size = 4 | (5 << 8);
    before.cpu.loads[3].due = 6132080;
    before.cpu.loads[3].address = 0x9;
    before.cpu.loads[3].size = CDJ_C674X_DELAYED_SAT;
    before.cpu.loads[0].due = 6132081;
    before.cpu.loads[0].address = 0x11802000;
    before.cpu.loads[0].bank = 1;
    before.cpu.loads[0].dst = 7;
    before.cpu.loads[0].size = 4;
    before.cpu.loads[0].sign_extend = true;
    before.cpu.loads[1].due = 6132083;
    before.cpu.loads[1].value = 0x4f800000;
    before.cpu.loads[1].address = 1u << 23;
    before.cpu.loads[1].bank = 1;
    before.cpu.loads[1].dst = 9;
    before.cpu.loads[1].size = 0;
    before.cpu.loads[1].sign_extend = true;
    before.cpu.loads[2].due = 6132084;
    before.cpu.loads[2].value = UINT64_C(0xfffffa9ba111462c);
    before.cpu.loads[2].bank = 0;
    before.cpu.loads[2].dst = 8;
    before.cpu.loads[2].size = 16;
    before.cpu.loop_active = true;
    before.cpu.loop_wait = 2;
    before.cpu.loop_tags = 9;
    before.cpu.loop_packets = 5;
    before.cpu.control_ready[31] = UINT64_C(0x8100000711803000);
    before.cpu.loop.ii = 4;
    before.cpu.loop.length = 3;
    before.cpu.loop.iterations = 14;
    before.cpu.loop.cycle = 17;
    before.cpu.loop.tags[2][7] = 91;
    before.cpu.loop.count[2] = 8;
    before.cpu.loop_instructions[91].pc = 0x11803000;
    before.cpu.loop_instructions[91].word = 0xfeedbeef;
    before.cpu.cycle_tick = unused_tick;
    before.cpu.cycle_opaque = &before;
    before.cpu.fault = "instruction not implemented";
    before.cpu.fault_pc = 0x11804468;
    before.cpu.fault_word = 0xc09868c0;
    before.syscfg.cfgchip[1] = 0x18000;
    before.psc.remaining[1][1] = 3;
    before.mcasp.pdir[2] = 0x55aa;
    before.gpio.input[2] = 0x24;
    before.i2c.mode[1] = 0x20;
    before.pll.oscin_cycles = 98765;
    before.hpi.hint = true;
    before.emifb.sdcfg = 0x12345678;
    cdj_c6747_intc_reset(&before.intc);
    assert(cdj_c6747_intc_event(&before.intc, 34));
    cdj_c6747_timers_reset(before.timers);
    before.timers[0].tgcr = 0x17;
    before.timers[0].intctlstat = 1;
    before.timers[1].prd12 = UINT32_MAX;
    cdj_c6747_spis_reset(before.spis);
    before.spis[1].gcr0 = 1;
    before.spis[1].gcr1 = 3;
    before.spis[1].pin_function = 0xe01;
    before.spis[1].format[0] = 0x00021810;
    before.spis[1].delay = 0x02020408;
    cdj_c6747_cache_reset(&before.cache);
    before.cache.l2cfg = 3;
    before.cache.l1pcc = 0x10000;
    before.cache.mar[192] = 1;
    cdj_c6747_mcasp_control_reset(&before.mcasp_control);
    before.mcasp_control.gblctl[1] = 0x1f00;
    before.mcasp_control.xfmt[1] = 0xf2;
    before.mcasp_control.srctl[1][3] = 1;
    before.mcasp_control.xrdy[1] = 1u << 3;
    cdj_c6747_edma_reset(&before.edma);
    before.edma.drae[1] = 0x28;
    before.edma.param[3][0] = 0x00103200;
    before.edma.param[3][5] = 0x00024400;
    before.edma.irq_notifications = 1u << 2;
    cdj_c6747_syscfg_priority_reset(&before.syscfg_priority);
    before.syscfg_priority.mstpri[0] = 0x44442122;
    before.syscfg_priority.mstpri[1] = 0x44442000;
    cdj_c6747_intc_delivery_reset(&before.intc_delivery);
    before.intc_delivery.cpu_request = 1u << 8;
    cdj_wm8740_reset(&before.wm8740);
    before.wm8740.program[0] = 0x1ff;
    before.wm8740.program[1] = 0x1fe;
    before.wm8740.last_word = 0x3fe;
    before.wm8740.transfers = 2;
    before.wm8740.active_attenuation[0] = 0xff;
    before.wm8740.active_attenuation[1] = 0xfe;
    cdj_c6747_spi_transfer_reset(&before.spi_transfer);
    before.spi_transfer.clock_phase = 0;
    before.spi_transfer.half_ticks_remaining = 775;
    before.spi_transfer.active_control = 0x1ff;
    before.spi_transfer.active_format = 0x00021810;
    before.spi_transfer.active_delay = 0x02020408;
    before.spi_transfer.queued_control = 0x3ff;
    before.spi_transfer.queued_format = 0x00021810;
    before.spi_transfer.queued_delay = 0x02020408;
    before.spi_transfer.phase = 2;
    before.spi_transfer.queued_valid = 1;
    before.spi_transfer.tx_full = 1;
    cdj_dsp_scheduler_reset(&before.scheduler);
    assert(cdj_dsp_scheduler_request(&before.scheduler));
    assert(cdj_dsp_scheduler_begin(&before.scheduler) ==
           CDJ_DSP_SCHEDULER_SLICE_STEPS);
    assert(cdj_dsp_scheduler_request(&before.scheduler));
    assert(cdj_dsp_scheduler_end(&before.scheduler,
                                 CDJ_DSP_SCHEDULER_SLICE_STEPS,
                                 false, false));
    l2[0] = 0x68;
    l2[sizeof(l2) - 1] = 0xa5;
    shared_ram[0] = 0x56;
    shared_ram[sizeof(shared_ram) - 1] = 0x78;
    sdram[0x1000] = 0x12;
    sdram[sizeof(sdram) - 1] = 0x34;
    cdj_dsp_checkpoint_prepare(&before, "instruction not implemented");
    assert(before.had_fault && before.cpu.cycle_tick == NULL &&
           before.cpu.cycle_opaque == NULL && before.cpu.fault == NULL);

    char error[160] = {0};
    assert(cdj_dsp_checkpoint_write(argv[1], &before, l2, sizeof(l2),
                                    shared_ram, sizeof(shared_ram),
                                    sdram, sizeof(sdram), error, sizeof(error)));
    assert(cdj_dsp_checkpoint_read(argv[1], &after, restored_l2,
                                   sizeof(restored_l2), restored_shared_ram,
                                   sizeof(restored_shared_ram), restored_sdram,
                                   sizeof(restored_sdram), error, sizeof(error)));
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    assert(memcmp(l2, restored_l2, sizeof(l2)) == 0);
    assert(memcmp(shared_ram, restored_shared_ram, sizeof(shared_ram)) == 0);
    assert(memcmp(sdram, restored_sdram, sizeof(sdram)) == 0);
    assert(after.cpu.store_count == 2 && after.cpu.load_count == 5);
    assert(after.cpu.loads[4].size == (8 | (5 << 8)) &&
           after.cpu.loads[4].address == 0xc000101f && after.cpu.loads[4].dst == 10);
    assert(after.cpu.stores[1].size == (4 | (5 << 8)) &&
           after.cpu.stores[1].address == 0xc000103e && after.cpu.stores[1].value == 0x12345678);
    assert(after.cpu.loads[3].size == CDJ_C674X_DELAYED_SAT &&
           after.cpu.loads[3].address == 0x9 && after.cpu.loads[3].due == 6132080);
    assert(after.cpu.loads[1].size == 0 &&
           after.cpu.loads[1].value == 0x4f800000 &&
           after.cpu.loads[1].address == (1u << 23) &&
           after.cpu.loads[1].sign_extend);
    assert(after.cpu.loads[2].size == 16 && after.cpu.loads[2].dst == 8 &&
           after.cpu.loads[2].value == UINT64_C(0xfffffa9ba111462c));
    assert(after.cpu.loop_active && after.cpu.loop.tags[2][7] == 91);
    assert(after.cpu.loop_instructions[91].word == 0xfeedbeef);
    assert(after.intc.event_flag[1] == 4 &&
           after.intc.exception_mask[0] == UINT32_MAX);
    assert(after.timers[0].tgcr == 0x17 &&
           after.timers[0].intctlstat == 1 &&
           after.timers[1].prd12 == UINT32_MAX);
    assert(after.spis[1].gcr0 == 1 && after.spis[1].gcr1 == 3 &&
           after.spis[1].pin_function == 0xe01 &&
           after.spis[1].format[0] == 0x00021810 &&
           after.spis[1].delay == 0x02020408);
    assert(after.cache.l2cfg == 3 && after.cache.l1pcc == 0x10000 &&
           after.cache.mar[192] == 1);
    assert(after.mcasp_control.gblctl[1] == 0x1f00 &&
           after.mcasp_control.xfmt[1] == 0xf2 &&
           after.mcasp_control.srctl[1][3] == 1 &&
           after.mcasp_control.xrdy[1] == (1u << 3));
    assert(after.edma.drae[1] == 0x28 &&
           after.edma.param[3][0] == 0x00103200 &&
           after.edma.param[3][5] == 0x00024400 &&
           after.edma.irq_notifications == (1u << 2));
    assert(after.syscfg_priority.mstpri[0] == 0x44442122 &&
           after.syscfg_priority.mstpri[1] == 0x44442000 &&
           after.syscfg_priority.mstpri[2] == 0x54604404);
    assert(after.intc_delivery.cpu_request == (1u << 8));
    assert(after.wm8740.program[0] == 0x1ff &&
           after.wm8740.program[1] == 0x1fe &&
           after.wm8740.last_word == 0x3fe &&
           after.wm8740.transfers == 2 &&
           after.wm8740.active_attenuation[0] == 0xff &&
           after.wm8740.active_attenuation[1] == 0xfe);
    assert(after.spi_transfer.half_ticks_remaining == 775 &&
           after.spi_transfer.active_control == 0x1ff &&
           after.spi_transfer.queued_control == 0x3ff &&
           after.spi_transfer.phase == 2 && after.spi_transfer.queued_valid &&
           after.spi_transfer.tx_full &&
           cdj_c6747_spi_transfer_valid(&after.spi_transfer));
    assert(after.scheduler.activation_id == 1 &&
           after.scheduler.slice_id == 1 &&
           after.scheduler.remaining ==
               CDJ_DSP_SCHEDULER_ACTIVATION_STEPS -
               CDJ_DSP_SCHEDULER_SLICE_STEPS &&
           after.scheduler.slice_steps == CDJ_DSP_SCHEDULER_SLICE_STEPS &&
           after.scheduler.pending && after.scheduler.rearm &&
           after.scheduler.mode == CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1 &&
           cdj_dsp_scheduler_valid(&after.scheduler));
    CdjDspCheckpointState invalid = before;
    invalid.spi_transfer.reserved[0] = 1;
    assert(!cdj_dsp_checkpoint_write(argv[1], &invalid, l2, sizeof(l2),
                                     shared_ram, sizeof(shared_ram), sdram,
                                     sizeof(sdram), error, sizeof(error)));
    invalid = before;
    invalid.scheduler.reserved = 1;
    assert(!cdj_dsp_checkpoint_write(argv[1], &invalid, l2, sizeof(l2),
                                     shared_ram, sizeof(shared_ram), sdram,
                                     sizeof(sdram), error, sizeof(error)));

    downgrade_to_schema10(argv[1]);
    memset(&after, 0xa5, sizeof(after));
    assert(cdj_dsp_checkpoint_read(argv[1], &after, restored_l2,
                                   sizeof(restored_l2), restored_shared_ram,
                                   sizeof(restored_shared_ram), restored_sdram,
                                   sizeof(restored_sdram), error, sizeof(error)));
    CdjDspScheduler legacy_scheduler = {0};
    assert(memcmp(&after.scheduler, &legacy_scheduler,
                  sizeof(legacy_scheduler)) == 0 &&
           after.scheduler.mode == CDJ_DSP_SCHEDULER_MODE_LEGACY &&
           cdj_dsp_scheduler_valid(&after.scheduler));
    assert(after.spi_transfer.half_ticks_remaining == 775 &&
           after.spi_transfer.active_control == 0x1ff &&
           after.spi_transfer.queued_control == 0x3ff &&
           after.spi_transfer.phase == 2 && after.spi_transfer.queued_valid &&
           after.spi_transfer.tx_full &&
           cdj_c6747_spi_transfer_valid(&after.spi_transfer));
    assert(after.wm8740.program[0] == 0x1ff &&
           after.wm8740.program[1] == 0x1fe &&
           after.wm8740.last_word == 0x3fe &&
           after.wm8740.transfers == 2);
    assert_schema8_peripheral_tail_preserved(&after);

    downgrade_schema10_to_schema9(argv[1]);
    memset(&after, 0xa5, sizeof(after));
    assert(cdj_dsp_checkpoint_read(argv[1], &after, restored_l2,
                                   sizeof(restored_l2), restored_shared_ram,
                                   sizeof(restored_shared_ram), restored_sdram,
                                   sizeof(restored_sdram), error, sizeof(error)));
    CdjC6747SpiTransfer reset_transfer;
    cdj_c6747_spi_transfer_reset(&reset_transfer);
    assert(memcmp(&after.spi_transfer, &reset_transfer,
                  sizeof(reset_transfer)) == 0);
    assert(after.wm8740.program[0] == 0x1ff &&
           after.wm8740.program[1] == 0x1fe &&
           after.wm8740.last_word == 0x3fe &&
           after.wm8740.transfers == 2 &&
           after.wm8740.active_attenuation[0] == 0xff &&
           after.wm8740.active_attenuation[1] == 0xfe);
    assert(memcmp(&after.scheduler, &legacy_scheduler,
                  sizeof(legacy_scheduler)) == 0 &&
           cdj_dsp_scheduler_valid(&after.scheduler));
    assert_schema8_peripheral_tail_preserved(&after);
    assert(memcmp(l2, restored_l2, sizeof(l2)) == 0);
    assert(memcmp(shared_ram, restored_shared_ram, sizeof(shared_ram)) == 0);
    assert(memcmp(sdram, restored_sdram, sizeof(sdram)) == 0);

    FILE *file = fopen(argv[1], "r+b");
    assert(file && fputc('X', file) != EOF && fclose(file) == 0);
    assert(!cdj_dsp_checkpoint_read(argv[1], &after, restored_l2,
                                    sizeof(restored_l2), restored_shared_ram,
                                    sizeof(restored_shared_ram), restored_sdram,
                                    sizeof(restored_sdram), error, sizeof(error)));
    puts("DSP checkpoint schema-11 scheduler round trip and schema-10/9 migrations passed");
    return 0;
}
