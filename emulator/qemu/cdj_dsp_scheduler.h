/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_DSP_SCHEDULER_H
#define CDJ_DSP_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#define CDJ_DSP_SCHEDULER_MODE_LEGACY 0u
#define CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1 1u
#define CDJ_DSP_SCHEDULER_ACTIVATION_STEPS 1000000u
#define CDJ_DSP_SCHEDULER_SLICE_STEPS 4096u

/* Pure, serializable connected-scheduler state.  It deliberately contains no
 * QEMU timer pointer and is valid for checkpoints only between begin/end calls.
 * DEFERRED_V1 is a diagnostic host-scheduling policy, not a DSP clock model. */
typedef struct {
    uint64_t activation_id;
    uint64_t slice_id;
    uint32_t remaining;
    uint32_t slice_steps;
    uint8_t pending;
    uint8_t rearm;
    uint8_t mode;
    uint8_t reserved;
} CdjDspScheduler;

void cdj_dsp_scheduler_reset(CdjDspScheduler *scheduler);
bool cdj_dsp_scheduler_valid(const CdjDspScheduler *scheduler);
bool cdj_dsp_scheduler_request(CdjDspScheduler *scheduler);
uint32_t cdj_dsp_scheduler_begin(CdjDspScheduler *scheduler);
bool cdj_dsp_scheduler_end(CdjDspScheduler *scheduler, uint32_t executed,
                           bool hint, bool fault);

#endif
