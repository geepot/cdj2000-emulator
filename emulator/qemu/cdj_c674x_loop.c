/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cdj_c674x_loop.h"

bool cdj_c674x_loop_init(CdjC674xLoop *loop, unsigned ii, uint32_t iterations)
{
    if (!ii || ii > 14) return false;
    memset(loop, 0, sizeof(*loop));
    loop->ii = ii;
    loop->iterations = iterations;
    return true;
}

bool cdj_c674x_loop_load(CdjC674xLoop *loop, const uint32_t *tags,
                        unsigned count, bool finish, unsigned delay)
{
    if (!loop->ii || loop->sealed || loop->cycle >= 48 || count > 8 ||
        (count && !tags) || loop->length > loop->cycle) return false;
    unsigned index = loop->cycle;
    if (count) memcpy(loop->tags[index], tags, count * sizeof(*tags));
    loop->count[index] = count;
    loop->length = index + 1;
    if (finish) {
        loop->sealed = true;
        if (loop->predicate_loop) {
            loop->post_cycle = loop->end_cycle = UINT64_MAX;
            return true;
        }
        /* Post-loop fetching cannot precede the final loading boundary. */
        uint64_t loading_end = ((loop->length + loop->ii - 1) / loop->ii) * loop->ii;
        loop->post_cycle = (uint64_t)loop->iterations * loop->ii + delay;
        if (loop->post_cycle < loading_end) loop->post_cycle = loading_end;
        loop->end_cycle = loop->iterations ?
            (uint64_t)(loop->iterations - 1) * loop->ii + loop->length : loading_end;
        if (loop->post_cycle > loop->end_cycle) loop->post_cycle = loop->end_cycle;
    }
    return true;
}

bool cdj_c674x_loop_issue(CdjC674xLoop *loop, uint32_t tags[8], unsigned *count,
                         bool *post_fetch, bool *drained)
{
    uint32_t result[8];
    unsigned n = 0;
    if (!loop->ii || (!loop->sealed && loop->cycle >= 48)) return false;
    for (unsigned origin = 0; origin < loop->length; ++origin) {
        if (origin > loop->cycle) continue;
        uint64_t age = loop->cycle - origin;
        if (age % loop->ii || (!loop->predicate_loop && age / loop->ii >= loop->iterations)) continue;
        if (n + loop->count[origin] > 8) return false;
        memcpy(result + n, loop->tags[origin], loop->count[origin] * sizeof(*result));
        n += loop->count[origin];
    }
    if (n) memcpy(tags, result, n * sizeof(*tags));
    *count = n;
    *post_fetch = loop->sealed && loop->cycle >= loop->post_cycle;
    *drained = loop->sealed && loop->cycle >= loop->end_cycle;
    ++loop->cycle;
    return true;
}
