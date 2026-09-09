#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cdj_dsp_checkpoint.h"

static uint8_t l2[CDJ_DSP_L2_SIZE], restored_l2[CDJ_DSP_L2_SIZE];
static uint8_t shared_ram[CDJ_DSP_SHARED_RAM_SIZE];
static uint8_t restored_shared_ram[CDJ_DSP_SHARED_RAM_SIZE];
static uint8_t sdram[CDJ_DSP_SDRAM_SIZE], restored_sdram[CDJ_DSP_SDRAM_SIZE];

static void unused_tick(void *opaque) { (void)opaque; }

int main(int argc, char **argv)
{
    assert(argc == 2);
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
    before.cpu.load_count = 3;
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
    assert(after.cpu.store_count == 1 && after.cpu.load_count == 3);
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

    FILE *file = fopen(argv[1], "r+b");
    assert(file && fputc('X', file) != EOF && fclose(file) == 0);
    assert(!cdj_dsp_checkpoint_read(argv[1], &after, restored_l2,
                                    sizeof(restored_l2), restored_shared_ram,
                                    sizeof(restored_shared_ram), restored_sdram,
                                    sizeof(restored_sdram), error, sizeof(error)));
    puts("DSP checkpoint pipeline, loop, peripheral, L2, shared RAM and SDRAM round trip passed");
    return 0;
}
