/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "cdj_dsp_scheduler.h"

_Static_assert(sizeof(CdjDspScheduler) == 32, "checkpoint ABI");
_Static_assert(_Alignof(CdjDspScheduler) == 8, "checkpoint ABI alignment");

static void finish_activation(CdjDspScheduler *scheduler)
{
    uint64_t activation = scheduler->activation_id;
    while (scheduler->pending && scheduler->activation_id == activation) {
        uint32_t quota = cdj_dsp_scheduler_begin(scheduler);
        assert(quota && cdj_dsp_scheduler_end(scheduler, quota, false, false));
    }
}

int main(void)
{
    CdjDspScheduler scheduler;
    cdj_dsp_scheduler_reset(&scheduler);
    assert(cdj_dsp_scheduler_valid(&scheduler));
    assert(scheduler.mode == CDJ_DSP_SCHEDULER_MODE_DEFERRED_V1);
    assert(scheduler.slice_steps == 4096 && !scheduler.pending);

    assert(cdj_dsp_scheduler_request(&scheduler));
    assert(scheduler.activation_id == 1 && scheduler.remaining == 1000000 &&
           scheduler.pending && !scheduler.rearm);
    assert(cdj_dsp_scheduler_begin(&scheduler) == 4096);
    assert(scheduler.slice_id == 1);
    assert(cdj_dsp_scheduler_request(&scheduler));
    assert(scheduler.rearm && scheduler.remaining == 1000000);
    CdjDspScheduler before = scheduler;
    assert(!cdj_dsp_scheduler_end(&scheduler, 4095, false, false));
    assert(!memcmp(&scheduler, &before, sizeof(scheduler)));
    assert(cdj_dsp_scheduler_end(&scheduler, 4096, false, false));
    assert(scheduler.remaining == 995904 && scheduler.pending && scheduler.rearm);

    /* A coalesced wake creates one, and only one, new bounded activation. */
    finish_activation(&scheduler);
    assert(scheduler.activation_id == 2 && scheduler.remaining == 1000000 &&
           scheduler.pending && !scheduler.rearm);
    finish_activation(&scheduler);
    assert(!scheduler.pending && !scheduler.remaining &&
           scheduler.activation_id == 2);

    assert(cdj_dsp_scheduler_request(&scheduler));
    assert(cdj_dsp_scheduler_begin(&scheduler) == 4096);
    assert(cdj_dsp_scheduler_end(&scheduler, 7, true, false));
    assert(!scheduler.pending && !scheduler.remaining && !scheduler.rearm);
    assert(cdj_dsp_scheduler_request(&scheduler));
    assert(cdj_dsp_scheduler_begin(&scheduler) == 4096);
    assert(cdj_dsp_scheduler_end(&scheduler, 0, false, true));
    assert(!scheduler.pending && !scheduler.remaining);

    CdjDspScheduler legacy = {0};
    assert(cdj_dsp_scheduler_valid(&legacy));
    assert(!cdj_dsp_scheduler_request(&legacy));
    assert(!cdj_dsp_scheduler_begin(&legacy));
    assert(!cdj_dsp_scheduler_end(&legacy, 0, false, false));

    CdjDspScheduler corrupt = scheduler;
    corrupt.pending = 1;
    assert(!cdj_dsp_scheduler_valid(&corrupt));
    corrupt = scheduler;
    corrupt.slice_steps = 1;
    assert(!cdj_dsp_scheduler_valid(&corrupt));
    corrupt = scheduler;
    corrupt.reserved = 1;
    assert(!cdj_dsp_scheduler_valid(&corrupt));

    CdjDspScheduler overflow;
    cdj_dsp_scheduler_reset(&overflow);
    overflow.activation_id = UINT64_MAX;
    overflow.slice_id = UINT64_MAX - 1;
    assert(cdj_dsp_scheduler_valid(&overflow));
    before = overflow;
    assert(!cdj_dsp_scheduler_request(&overflow));
    assert(!memcmp(&overflow, &before, sizeof(overflow)));

    return 0;
}
