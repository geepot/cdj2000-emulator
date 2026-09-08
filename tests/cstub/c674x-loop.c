/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "cdj_c674x_loop.h"
int main(void)
{
    CdjC674xLoop loop;
    uint32_t out[8], tag;
    unsigned n;
    bool post, drained;
    /* SPRUFE8B Table 7-1: eight iterations, II=1, LDW/NOP4/MV/STW.
     * Each bit marks a functional operation issued during that cycle. */
    const unsigned expected[] = {1,1,1,1,1,3,7,7,6,6,6,6,6,4};
    assert(cdj_c674x_loop_init(&loop, 1, 8));
    for (unsigned t = 0; t < 14; ++t) {
        if (t <= 6) {
            tag = t == 0 ? 1 : t == 5 ? 2 : t == 6 ? 4 : 0;
            assert(cdj_c674x_loop_load(&loop, &tag, tag ? 1 : 0, t == 6, 6));
        }
        assert(cdj_c674x_loop_issue(&loop, out, &n, &post, &drained));
        unsigned mask = 0;
        for (unsigned j = 0; j < n; ++j) mask |= out[j];
        assert(mask == expected[t] && !post && !drained);
    }
    assert(cdj_c674x_loop_issue(&loop, out, &n, &post, &drained));
    assert(n == 0 && post && drained);

    /* II=2: post-loop instructions may overlap the draining stores. */
    assert(cdj_c674x_loop_init(&loop, 2, 3));
    for (unsigned t = 0; t <= 10; ++t) {
        if (t <= 5) {
            tag = t == 0 ? 1 : t == 5 ? 2 : 0;
            assert(cdj_c674x_loop_load(&loop, &tag, tag ? 1 : 0, t == 5, 0));
        }
        assert(cdj_c674x_loop_issue(&loop, out, &n, &post, &drained));
        unsigned expected_tag = (t <= 4 && !(t & 1)) ? 1 :
                                (t >= 5 && t <= 9 && (t & 1)) ? 2 : 0;
        assert(n == (expected_tag ? 1u : 0u));
        if (n) assert(out[0] == expected_tag);
        assert(post == (t >= 6) && drained == (t >= 10));
    }

    /* Zero iterations still load the body but issue none of its operations. */
    assert(cdj_c674x_loop_init(&loop, 2, 0));
    for (unsigned t = 0; t <= 4; ++t) {
        tag = 1;
        if (t < 3) assert(cdj_c674x_loop_load(&loop, &tag, 1, t == 2, 0));
        assert(cdj_c674x_loop_issue(&loop, out, &n, &post, &drained));
        assert(!n && post == (t == 4) && drained == (t == 4));
    }
    assert(!cdj_c674x_loop_init(&loop, 0, 1));
    assert(!cdj_c674x_loop_init(&loop, 15, 1));
    assert(cdj_c674x_loop_init(&loop, 1, 2));
    uint32_t full[8] = {1,2,3,4,5,6,7,8};
    assert(cdj_c674x_loop_load(&loop, full, 8, false, 0));
    assert(cdj_c674x_loop_issue(&loop, out, &n, &post, &drained));
    assert(cdj_c674x_loop_load(&loop, full, 8, true, 0));
    CdjC674xLoop before = loop;
    assert(!cdj_c674x_loop_issue(&loop, out, &n, &post, &drained));
    assert(!memcmp(&loop, &before, sizeof(loop)));
    return 0;
}
