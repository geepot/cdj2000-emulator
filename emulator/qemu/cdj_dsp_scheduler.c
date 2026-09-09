/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_dsp_scheduler.h"

#include <limits.h>
#include <string.h>

static bool scheduler_idle(const CdjDspScheduler *scheduler)
{
    return !scheduler->activation_id && !scheduler->slice_id &&
        !scheduler->remaining && !scheduler->slice_steps &&
        !scheduler->pending && !scheduler->rearm;
}

void cdj_dsp_scheduler_reset(CdjDspScheduler *scheduler)
{
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->slice_steps = CDJ_DSP_SCHEDULER_SLICE_STEPS;
    scheduler->mode = CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1;
}

bool cdj_dsp_scheduler_valid(const CdjDspScheduler *scheduler)
{
    if (!scheduler || scheduler->reserved || scheduler->pending > 1 ||
        scheduler->rearm > 1) return false;
    if (scheduler->mode == CDJ_DSP_SCHEDULER_MODE_LEGACY)
        return scheduler_idle(scheduler);
    if (scheduler->mode != CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1 ||
        scheduler->slice_steps != CDJ_DSP_SCHEDULER_SLICE_STEPS ||
        scheduler->remaining > CDJ_DSP_SCHEDULER_ACTIVATION_STEPS ||
        scheduler->pending != !!scheduler->remaining ||
        (scheduler->rearm && !scheduler->pending)) return false;
    if (!scheduler->activation_id)
        return !scheduler->slice_id && !scheduler->remaining &&
            !scheduler->pending && !scheduler->rearm;
    if (!scheduler->slice_id &&
        scheduler->remaining != CDJ_DSP_SCHEDULER_ACTIVATION_STEPS)
        return false;
    if (scheduler->remaining &&
        (CDJ_DSP_SCHEDULER_ACTIVATION_STEPS - scheduler->remaining) %
            scheduler->slice_steps) return false;
    return true;
}

bool cdj_dsp_scheduler_request(CdjDspScheduler *scheduler)
{
    if (!cdj_dsp_scheduler_valid(scheduler) ||
        scheduler->mode != CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1) return false;
    if (scheduler->pending) {
        scheduler->rearm = 1;
        return true;
    }
    if (scheduler->activation_id == UINT64_MAX) return false;
    ++scheduler->activation_id;
    scheduler->remaining = CDJ_DSP_SCHEDULER_ACTIVATION_STEPS;
    scheduler->pending = 1;
    return true;
}

uint32_t cdj_dsp_scheduler_begin(CdjDspScheduler *scheduler)
{
    if (!cdj_dsp_scheduler_valid(scheduler) ||
        scheduler->mode != CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1 ||
        !scheduler->pending || scheduler->slice_id == UINT64_MAX) return 0;
    ++scheduler->slice_id;
    return scheduler->remaining < scheduler->slice_steps ?
        scheduler->remaining : scheduler->slice_steps;
}

bool cdj_dsp_scheduler_end(CdjDspScheduler *scheduler, uint32_t executed,
                           bool hint, bool fault)
{
    if (!cdj_dsp_scheduler_valid(scheduler) ||
        scheduler->mode != CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1 ||
        !scheduler->pending) return false;
    uint32_t quota = scheduler->remaining < scheduler->slice_steps ?
        scheduler->remaining : scheduler->slice_steps;
    if (executed > quota || (!hint && !fault && executed != quota)) return false;

    CdjDspScheduler next = *scheduler;
    if (hint || fault) {
        next.remaining = 0;
        next.pending = 0;
        next.rearm = 0;
    } else {
        next.remaining -= executed;
        if (!next.remaining) {
            if (next.rearm) {
                if (next.activation_id == UINT64_MAX) return false;
                ++next.activation_id;
                next.remaining = CDJ_DSP_SCHEDULER_ACTIVATION_STEPS;
                next.rearm = 0;
            } else {
                next.pending = 0;
            }
        }
    }
    if (!cdj_dsp_scheduler_valid(&next)) return false;
    *scheduler = next;
    return true;
}
