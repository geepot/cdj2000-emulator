#include <assert.h>
#include <stdint.h>

#include "cdj_dsp_checkpoint.h"

static uint8_t l2[CDJ_DSP_L2_SIZE];
static uint8_t shared_ram[CDJ_DSP_SHARED_RAM_SIZE];
static uint8_t sdram[CDJ_DSP_SDRAM_SIZE];

int main(int argc, char **argv)
{
    assert(argc == 2);
    CdjDspCheckpointState state = {0};
    state.hpi_address = 0x11800000;
    state.event_sequence = 1;
    state.checkpoint_sequence = 1;
    state.reset_released = state.dsp_started = 1;
    state.cpu.pc = 0x11800020;
    cdj_c6747_intc_reset(&state.intc);
    cdj_c6747_timers_reset(state.timers);
    /* There is no compact header in this fetch packet. This unsupported
     * full-width word gives event-injection tests a deterministic fault. */
    l2[0x20] = 0xfe;
    l2[0x21] = 0xca;
    l2[0x22] = 0xad;
    l2[0x23] = 0xde;
    cdj_dsp_checkpoint_prepare(&state, "boot-phase boundary");

    char error[160] = {0};
    assert(cdj_dsp_checkpoint_write(argv[1], &state, l2, sizeof(l2),
                                    shared_ram, sizeof(shared_ram),
                                    sdram, sizeof(sdram), error, sizeof(error)));
    return 0;
}
