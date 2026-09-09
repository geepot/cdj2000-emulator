#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "cdj_dsp_checkpoint.h"

static uint8_t l2[CDJ_DSP_L2_SIZE];
static uint8_t shared_ram[CDJ_DSP_SHARED_RAM_SIZE];
static uint8_t sdram[CDJ_DSP_SDRAM_SIZE];

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    CdjDspCheckpointState state = {0};
    state.hpi_address = 0x11800000;
    state.event_sequence = 1;
    state.checkpoint_sequence = 1;
    state.reset_released = state.dsp_started = 1;
    state.cpu.pc = 0x11800020;
    cdj_c6747_intc_reset(&state.intc);
    cdj_c6747_timers_reset(state.timers);
    cdj_c6747_spis_reset(state.spis);
    cdj_c6747_cache_reset(&state.cache);
    cdj_c6747_mcasp_control_reset(&state.mcasp_control);
    cdj_c6747_edma_reset(&state.edma);
    cdj_c6747_syscfg_priority_reset(&state.syscfg_priority);
    cdj_c6747_intc_delivery_reset(&state.intc_delivery);
    /* There is no compact header in this fetch packet. This unsupported
     * full-width word gives event-injection tests a deterministic fault. */
    l2[0x20] = 0xfe;
    l2[0x21] = 0xca;
    l2[0x22] = 0xad;
    l2[0x23] = 0xde;
    if (argc == 3 && !strncmp(argv[2], "deferred-", 9)) {
        cdj_dsp_scheduler_reset(&state.scheduler);
        if (!strcmp(argv[2], "deferred-overflow")) {
            state.scheduler.activation_id = UINT64_MAX;
            state.scheduler.slice_id = UINT64_MAX - 1;
        } else if (strcmp(argv[2], "deferred-start") &&
                   strcmp(argv[2], "deferred-phase")) {
            assert(cdj_dsp_scheduler_request(&state.scheduler));
        }
        if (!strcmp(argv[2], "deferred-high-ids")) {
            state.scheduler.activation_id = UINT64_C(0x100000007);
            state.scheduler.slice_id = UINT64_C(0x10000000b);
        }
        if (!strcmp(argv[2], "deferred-running")) {
            memset(l2 + 0x20, 0, 0x24 - 0x20);
            /* Full-width NOP packets until fault at the second slice. */
            l2[0x20020] = 0xfe; l2[0x20021] = 0xca;
            l2[0x20022] = 0xad; l2[0x20023] = 0xde;
        }
    }
    if (argc == 3 && !strcmp(argv[2], "phase budget exhausted")) {
        /* A pure later DSPINT must both resume replay and traverse the genuine
         * INTMUX/CPU interrupt path before the next instruction fetch. */
        state.cpu.control[1] = 1u; /* CSR.GIE */
        state.cpu.control[4] = 3u | (1u << 15); /* IER.NMI + INT15 */
        state.cpu.control[5] = 0x11800000u; /* ISTP */
        assert(cdj_c6747_intc_write(&state.intc,
                                    CDJ_C6747_INTC_INTMUX1 + 8,
                                    0x220e0d0c, 4, true));
        l2[0x1e0] = 0xfe;
        l2[0x1e1] = 0xca;
        l2[0x1e2] = 0xad;
        l2[0x1e3] = 0xde;
    }
    cdj_dsp_checkpoint_prepare(
        &state, argc == 3 && !strcmp(argv[2], "deferred-start") ?
        "DSP start boundary" : argc == 3 &&
        (!strcmp(argv[2], "deferred-phase") || !strcmp(argv[2], "deferred-phase-pending")) ?
        "boot-phase boundary" : argc == 3 ? argv[2] : "boot-phase boundary");

    char error[160] = {0};
    assert(cdj_dsp_checkpoint_write(argv[1], &state, l2, sizeof(l2),
                                    shared_ram, sizeof(shared_ram),
                                    sdram, sizeof(sdram), error, sizeof(error)));
    return 0;
}
