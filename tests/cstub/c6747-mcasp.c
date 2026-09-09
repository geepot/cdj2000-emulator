/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <string.h>
#include "cdj_c6747_mcasp.h"
int main(void)
{
    CdjC6747Mcasp s, before;
    _Static_assert(sizeof(CdjC6747Mcasp) == 36,
                   "pin state is an existing checkpoint ABI");
    cdj_c6747_mcasp_reset(&s);
    const uint32_t masks[] = {0xfe00ffffu, 0xfe000fffu, 0xfe00000fu};
    for (unsigned b = 0; b < 3; ++b) {
        uint32_t base = 0x01d00000 + b * 0x4000, v;
        for (unsigned o = 0x10; o <= 0x18; o += 4) {
            assert(cdj_c6747_mcasp_read(&s, base + o, &v) && v == 0);
            before = s;
            assert(cdj_c6747_mcasp_write(&s, base + o, masks[b], 4, false));
            assert(!memcmp(&s, &before, sizeof(s)));
            assert(cdj_c6747_mcasp_write(&s, base + o, masks[b], 4, true));
            assert(cdj_c6747_mcasp_read(&s, base + o, &v) && v == masks[b]);
        }
        before = s;
        assert(cdj_c6747_mcasp_write(&s, base + 0x20, 5, 4, false));
        assert(cdj_c6747_mcasp_write(&s, base + 0x1c, 5, 4, false));
        assert(!memcmp(&s, &before, sizeof(s)));
        assert(cdj_c6747_mcasp_write(&s, base + 0x20, 5, 4, true));
        assert(cdj_c6747_mcasp_read(&s, base + 0x18, &v) && v == (masks[b] & ~5u));
        assert(cdj_c6747_mcasp_write(&s, base + 0x1c, 1, 4, true));
        assert(cdj_c6747_mcasp_write(&s, base + 0x1c, 0, 4, true));
        assert(cdj_c6747_mcasp_write(&s, base + 0x20, 0, 4, true));
        assert(cdj_c6747_mcasp_read(&s, base + 0x18, &v) && v == (masks[b] & ~4u));
        assert(cdj_c6747_mcasp_write(&s, base + 0x10, 0, 4, true));
        assert(cdj_c6747_mcasp_write(&s, base + 0x14, 0, 4, true));
        assert(cdj_c6747_mcasp_read(&s, base + 0x18, &v) && v == (masks[b] & ~4u));
        before = s;
        for (unsigned o = 0x10; o <= 0x20; o += 4) {
            assert(!cdj_c6747_mcasp_write(&s, base + o, ~masks[b], 4, true));
            assert(!cdj_c6747_mcasp_write(&s, base + o, 0, 1, true));
            assert(!cdj_c6747_mcasp_write(&s, base + o, UINT64_C(1)<<32, 4, true));
        }
        assert(!cdj_c6747_mcasp_read(&s, base + 0x1c, &v));
        assert(!cdj_c6747_mcasp_read(&s, base + 0x20, &v));
        assert(!cdj_c6747_mcasp_read(&s, base + 0x44, &v));
        assert(!cdj_c6747_mcasp_write(&s, base + 0x44, 0, 4, true));
        assert(!cdj_c6747_mcasp_read(&s, base + 0x15, &v));
        assert(!cdj_c6747_mcasp_write(&s, base + 0x15, 0, 4, true));
        assert(!memcmp(&s, &before, sizeof(s)));
        for (unsigned other = b + 1; other < 3; ++other)
            assert(s.pfunc[other] == 0 && s.pdir[other] == 0 && s.pdout[other] == 0);
    }
    cdj_c6747_mcasp_reset(&s);
    before = (CdjC6747Mcasp){0};
    assert(!memcmp(&s, &before, sizeof(s)));

    CdjC6747McaspControl control, control_before;
    cdj_c6747_mcasp_control_reset(&control);
    const unsigned serializers[] = {16, 12, 4};
    for (unsigned b = 0; b < 3; ++b) {
        uint32_t base = 0x01d00000 + b * 0x4000, v;
        const unsigned zero_registers[] = {
            0x44, 0x48, 0x4c, 0x50, 0x60, 0xa0, 0xa4, 0xa8,
            0xac, 0xb8, 0xbc, 0xc0, 0xc8, 0xcc,
        };
        for (unsigned j = 0; j < sizeof(zero_registers) /
                                  sizeof(zero_registers[0]); ++j)
            assert(cdj_c6747_mcasp_control_read(
                       &control, base + zero_registers[j], &v) && v == 0);
        assert(cdj_c6747_mcasp_control_read(&control, base + 0xb0, &v) &&
               v == 0x60);
        assert(cdj_c6747_mcasp_control_read(&control, base + 0xb4, &v) &&
               v == 0x8000);
        assert(cdj_c6747_mcasp_control_read(&control, base + 0xc4, &v) &&
               v == 0x17f);
        assert(!cdj_c6747_mcasp_control_read(&control, base + 0x04, &v));
        for (unsigned n = 0; n < serializers[b]; ++n)
            assert(cdj_c6747_mcasp_control_read(
                       &control, base + 0x180 + n * 4, &v) && v == 0);
        if (serializers[b] < 16)
            assert(!cdj_c6747_mcasp_control_read(
                       &control, base + 0x180 + serializers[b] * 4, &v));
        for (unsigned n = 0; n < 24; ++n) {
            uint32_t address = base + 0x100 + n * 4;
            assert(cdj_c6747_mcasp_control_read(&control, address, &v) &&
                   v == 0);
            assert(cdj_c6747_mcasp_control_write(
                       &control, address, 0x80000000u + n, 4, true));
            assert(cdj_c6747_mcasp_control_read(&control, address, &v) &&
                   v == 0x80000000u + n);
        }
    }

    /* Exact straight-line firmware configuration for McASP1 and McASP2.
     * Validation must be side-effect free; commit then reads back every
     * documented configuration latch. */
    struct RegisterWrite { unsigned offset; uint32_t value; } mcasp1[] = {
        {0x44, 0}, {0x60, 0}, {0xa0, 0}, {0x04, 1},
        {0xa4, 0xffffffff}, {0xa8, 0x000180f0}, {0xac, 0x113},
        {0xb0, 0xe5}, {0xb4, 0x8000}, {0xb8, 3}, {0xbc, 0},
        {0xc8, 0x00ff0008}, {0x180, 0x0d}, {0x50, 0},
        {0x4c, 0}, {0x48, 0}, {0xcc, 0},
    }, mcasp2[] = {
        {0x44, 0}, {0x60, 0}, {0xa0, 0}, {0x04, 1},
        {0xa4, 0xffffffff}, {0xa8, 0xf2}, {0xac, 0xc002},
        {0xb0, 0x62}, {0xb4, 0}, {0xb8, 0xffffffff}, {0xbc, 0},
        {0xc0, 0}, {0xc8, 0x00ff0000}, {0x18c, 1}, {0x50, 1},
        {0x4c, 0}, {0x48, 0}, {0xcc, 0},
    };
    for (unsigned which = 0; which < 2; ++which) {
        unsigned b = which + 1;
        uint32_t base = 0x01d00000 + b * 0x4000, v;
        struct RegisterWrite *sequence = which ? mcasp2 : mcasp1;
        unsigned count = which ? sizeof(mcasp2) / sizeof(mcasp2[0]) :
                                 sizeof(mcasp1) / sizeof(mcasp1[0]);
        for (unsigned j = 0; j < count; ++j) {
            control_before = control;
            assert(cdj_c6747_mcasp_control_write(
                       &control, base + sequence[j].offset,
                       sequence[j].value, 4, false));
            assert(!memcmp(&control, &control_before, sizeof(control)));
            assert(cdj_c6747_mcasp_control_write(
                       &control, base + sequence[j].offset,
                       sequence[j].value, 4, true));
            if (sequence[j].offset == 0x04) {
                assert(!cdj_c6747_mcasp_control_read(
                           &control, base + sequence[j].offset, &v));
            } else {
                assert(cdj_c6747_mcasp_control_read(
                           &control, base + sequence[j].offset, &v));
                assert(v == sequence[j].value);
            }
        }
    }

    /* SRCTL0-15 configuration uses only implemented C6747 serializers.
     * Ready bits are read-only, and inactive reset state reads zero. */
    cdj_c6747_mcasp_control_reset(&control);
    for (unsigned b = 0; b < 3; ++b) {
        uint32_t base = 0x01d00000 + b * 0x4000, v;
        for (unsigned n = 0; n < serializers[b]; ++n) {
            assert(cdj_c6747_mcasp_control_write(
                       &control, base + 0x180 + n * 4, 0x0d, 4, true));
            assert(cdj_c6747_mcasp_control_read(
                       &control, base + 0x180 + n * 4, &v) && v == 0x0d);
        }
    }

    /* McASP1 releases transmit clocks, serializers, state machine and frame
     * sync in firmware order through XGBLCTL. All aliases poll the same
     * immediately-latched GBLCTL state. XSRCLR sets XDATA/XRDY; XSMRST with
     * no modeled/preloaded XBUF sets XUNDRN and derived XERR. */
    cdj_c6747_mcasp_control_reset(&control);
    uint32_t base = 0x01d04000, v;
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180, 0x0d, 4, true));
    const uint32_t stages[] = {0x200, 0x300, 0x700, 0xf00, 0x1f00};
    for (unsigned j = 0; j < sizeof(stages) / sizeof(stages[0]); ++j) {
        assert(cdj_c6747_mcasp_control_write(
                   &control, base + 0xa0, stages[j], 4, true));
        assert(cdj_c6747_mcasp_control_read(&control, base + 0x44, &v) &&
               v == stages[j]);
        assert(cdj_c6747_mcasp_control_read(&control, base + 0x60, &v) &&
               v == stages[j]);
        assert(cdj_c6747_mcasp_control_read(&control, base + 0xa0, &v) &&
               v == stages[j]);
    }
    assert(cdj_c6747_mcasp_control_read(&control, base + 0x180, &v) &&
           v == 0x1d);
    assert(cdj_c6747_mcasp_control_read(&control, base + 0xc0, &v) &&
           v == 0x121);
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xc0, 0xffff, 4, true));
    assert(cdj_c6747_mcasp_control_read(&control, base + 0xc0, &v) && v == 0);
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xc0, 0x20, 4, true));
    assert(cdj_c6747_mcasp_control_read(&control, base + 0xc0, &v) && v == 0);

    /* Alias writes affect only their half. Clearing XSRCLR also clears the
     * modeled serializer-ready state. DITEN cannot change while XSMRST or
     * XSRCLR is released. */
    assert(!cdj_c6747_mcasp_control_write(
               &control, base + 0x50, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x60, 0x1f1f, 4, true));
    assert(cdj_c6747_mcasp_control_read(&control, base + 0x44, &v) &&
           v == 0x1f1f);
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa0, 0x1f, 4, true));
    assert(cdj_c6747_mcasp_control_read(&control, base + 0x44, &v) &&
           v == 0x1f);
    assert(cdj_c6747_mcasp_control_read(&control, base + 0x180, &v) &&
           v == 0x0d);
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x44, 0, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x50, 1, 4, true));

    /* Reserved fields/values, nonexistent serializers, non-word accesses,
     * and all unobserved values at undocumented offset 04h fail closed and
     * leave the complete control state unchanged. */
    cdj_c6747_mcasp_control_reset(&control); base = 0x01d00000;
    struct RegisterWrite invalid[] = {
        {0x04, 0}, {0x04, 2}, {0x44, 0x2000}, {0x48, 0x10},
        {0x48, 3}, {0x4c, 1}, {0x50, 2}, {0xa8, 3u << 16},
        {0xa8, 3u << 13 | 7u << 4}, {0xa8, 2u << 4},
        {0xa8, 1u << 18}, {0xac, 4}, {0xac, 0x80},
        {0xb0, 0x100}, {0xb4, 0x1000}, {0xbc, 0x40},
        {0xc0, 0x10000}, {0xc4, 1}, {0xc8, 0x10}, {0xc8, 9}, {0xcc, 1},
        {0x180, 0x10}, {0x180, 3}, {0x180, 4},
    };
    for (unsigned j = 0; j < sizeof(invalid) / sizeof(invalid[0]); ++j) {
        control_before = control;
        assert(!cdj_c6747_mcasp_control_write(
                   &control, base + invalid[j].offset,
                   invalid[j].value, 4, true));
        assert(!memcmp(&control, &control_before, sizeof(control)));
    }
    control_before = control;
    assert(!cdj_c6747_mcasp_control_write(&control, base + 0x44, 0, 1, true));
    assert(!cdj_c6747_mcasp_control_write(
               &control, base + 0x44, UINT64_C(1) << 32, 4, true));
    assert(!cdj_c6747_mcasp_control_write(&control, base + 0x45, 0, 4, true));
    assert(!cdj_c6747_mcasp_control_write(&control, 0x01d0c000, 0, 4, true));
    assert(!memcmp(&control, &control_before, sizeof(control)));
    assert(cdj_c6747_mcasp_control_valid(&control));
    control.xrdy[2] = 0x10;
    assert(!cdj_c6747_mcasp_control_valid(&control));

    /* The DMA XBUF port services active transmit serializers in increasing
     * order.  Genuine words and their acceptance order remain inspectable,
     * while XRDY/XDATA clear only as each active serializer is serviced. */
    cdj_c6747_mcasp_control_reset(&control); base = 0x01d00000;
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180 + 0 * 4, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180 + 4 * 4, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180 + 5 * 4, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180 + 7 * 4, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa0, 0x400, 4, true));
    assert(cdj_c6747_mcasp_axevt_ready(&control, 0));
    assert(control.xrdy[0] == 0xb1 && control.xdma_next[0] == 0);
    const unsigned dma_serializers[] = {0, 4, 5, 7};
    const uint32_t dma_words[] = {
        0x01234567, 0x89abcdef, 0x13579bdf, 0x2468ace0,
    };
    for (unsigned n = 0; n < 4; ++n) {
        control_before = control;
        assert(cdj_c6747_mcasp_control_write(
                   &control, base + 0x2000, dma_words[n], 4, false));
        assert(!memcmp(&control, &control_before, sizeof(control)));
        assert(cdj_c6747_mcasp_control_write(
                   &control, base + 0x2000, dma_words[n], 4, true));
        unsigned serializer = dma_serializers[n];
        assert(control.xbuf[0][serializer] == dma_words[n]);
        assert(control.xbuf_sequence[0][serializer] == n + 1);
        assert(!(control.xrdy[0] & (1u << serializer)));
        assert(cdj_c6747_mcasp_axevt_ready(&control, 0) == (n != 3));
    }
    assert(control.xbuf_writes[0] == 4 && control.xdma_next[0] == 16);
    assert(control.axevt_generation[0] == 1);
    assert(cdj_c6747_mcasp_control_valid(&control));

    /* More DMA words than active serializers set documented XDMAERR.  The
     * overrun does not invent an XBUF destination or acceptance sequence. */
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0xdeadbeef, 4, true));
    assert(control.xbuf_writes[0] == 4);
    assert(cdj_c6747_mcasp_control_read(&control, base + 0xc0, &v) &&
           v == 0x180);
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xc0, 0x80, 4, true));

    /* A second two-serializer arrangement proves that DMA order is based on
     * SRMOD, not on consecutive serializer numbers. */
    cdj_c6747_mcasp_control_reset(&control); base = 0x01d04000;
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180 + 2 * 4, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180 + 9 * 4, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa0, 0x400, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0x22222222, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0x99999999, 4, true));
    assert(control.xbuf[1][2] == 0x22222222 &&
           control.xbuf[1][9] == 0x99999999);
    assert(!cdj_c6747_mcasp_axevt_ready(&control, 1));

    /* The recovered McASP2 DIT configuration has one transmitter on AXR3.
     * DIT changes encoding, not the XBUF service rule. */
    cdj_c6747_mcasp_control_reset(&control); base = 0x01d08000;
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x18c, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x50, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa8, 0xf2, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa0, 0x400, 4, true));
    assert(cdj_c6747_mcasp_axevt_ready(&control, 2));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0x00abcdef, 4, true));
    assert(control.xbuf[2][3] == 0x00abcdef &&
           control.xbuf_sequence[2][3] == 1 && !control.xrdy[2]);

    /* XFMT.XBUSEL makes writes through the other port no-ops.  In selected
     * configuration-port mode, XBUFn services exactly the named serializer. */
    cdj_c6747_mcasp_control_reset(&control); base = 0x01d00000;
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180 + 1 * 4, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180 + 6 * 4, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa0, 0x400, 4, true));
    control_before = control;
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x204, 0x11111111, 4, true));
    assert(!memcmp(&control, &control_before, sizeof(control)));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa8, 0x78, 4, true));
    control_before = control;
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0xeeeeeeee, 4, true));
    assert(!memcmp(&control, &control_before, sizeof(control)));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x218, 0x66666666, 4, true));
    assert(control.xbuf[0][6] == 0x66666666 && control.xrdy[0] == 2);
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x204, 0x11111111, 4, true));
    assert(control.xbuf[0][1] == 0x11111111 && !control.xrdy[0]);
    assert(!cdj_c6747_mcasp_axevt_ready(&control, 0));

    /* The explicit slot primitive supplies no timing of its own.  For a
     * four-slot TDM frame, only XTDM-selected slots transfer the two genuine
     * buffered words in serializer lockstep and rearm one AXEVT. */
    cdj_c6747_mcasp_control_reset(&control); base = 0x01d00000;
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180 + 0 * 4, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180 + 4 * 4, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xac, 4u << 7, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xb8, 0x5, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa0, 0x700, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0x01020304, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0xa0b0c0d0, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa0, 0x1f00, 4, true));
    assert(control.axevt_generation[0] == 1 && !(control.xstat[0] & 1));
    bool axevt = false;
    assert(cdj_c6747_mcasp_tx_slot(&control, 0, &axevt) && axevt);
    assert(control.xslot[0] == 0 && control.tx_slot_boundaries[0] == 1);
    assert(control.xrsr[0][0] == 0x01020304 &&
           control.xrsr[0][4] == 0xa0b0c0d0);
    assert(control.xrsr_source_sequence[0][0] == 1 &&
           control.xrsr_source_sequence[0][4] == 2);
    assert(control.xrdy[0] == 0x11 && control.xdma_next[0] == 0 &&
           control.axevt_generation[0] == 2);
    assert(cdj_c6747_mcasp_control_read(&control, base + 0xc0, &v) &&
           v == 0x28);

    axevt = true;
    assert(cdj_c6747_mcasp_tx_slot(&control, 0, &axevt) && !axevt);
    assert(control.xslot[0] == 1 && control.tx_slot_boundaries[0] == 2 &&
           control.axevt_generation[0] == 2);
    assert(control.xrdy[0] == 0x11 && !(control.xstat[0] & 1));
    assert(cdj_c6747_mcasp_control_read(&control, base + 0xc0, &v) &&
           v == 0x20);

    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0x11112222, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0x33334444, 4, true));
    assert(cdj_c6747_mcasp_tx_slot(&control, 0, &axevt) && axevt);
    assert(control.xslot[0] == 2 && control.axevt_generation[0] == 3);
    assert(control.xrsr[0][0] == 0x11112222 &&
           control.xrsr[0][4] == 0x33334444);
    assert(control.xrsr_source_sequence[0][0] == 3 &&
           control.xrsr_source_sequence[0][4] == 4);
    assert(cdj_c6747_mcasp_tx_slot(&control, 0, &axevt) && !axevt);
    assert(control.xslot[0] == 3 && control.axevt_generation[0] == 3);
    assert(cdj_c6747_mcasp_tx_slot(&control, 0, &axevt) && axevt);
    assert(control.xslot[0] == 0 && control.tx_slot_boundaries[0] == 5 &&
           control.axevt_generation[0] == 4);
    assert(control.xstat[0] & 1);
    assert(control.xrsr[0][0] == 0x11112222 &&
           control.xrsr_source_sequence[0][0] == 3);
    assert(cdj_c6747_mcasp_control_valid(&control));

    /* DIT uses its architecture-defined 384-subframe counter and every
     * boundary is active.  An accepted zero is still a genuine transfer,
     * distinguished from missing data by its nonzero source sequence. */
    cdj_c6747_mcasp_control_reset(&control); base = 0x01d08000;
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x18c, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x50, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xac, 0xc002, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xb8, UINT32_MAX, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa0, 0x700, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa0, 0x1f00, 4, true));
    assert(cdj_c6747_mcasp_tx_slot(&control, 2, &axevt) && axevt);
    assert(control.xslot[2] == 0 && control.xrsr[2][3] == 0 &&
           control.xrsr_source_sequence[2][3] == 1 &&
           control.axevt_generation[2] == 2);
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 0x76543210, 4, true));
    assert(cdj_c6747_mcasp_tx_slot(&control, 2, &axevt) && axevt);
    assert(control.xslot[2] == 1 && control.tx_slot_boundaries[2] == 2 &&
           control.xrsr[2][3] == 0x76543210 &&
           control.xrsr_source_sequence[2][3] == 2 &&
           control.axevt_generation[2] == 3);
    assert(cdj_c6747_mcasp_control_read(&control, base + 0xc0, &v) &&
           v == 0x20);
    assert(cdj_c6747_mcasp_control_valid(&control));

    /* Unsupported or stopped configurations fail without changing either
     * checkpoint state or the caller's event result. */
    cdj_c6747_mcasp_control_reset(&control);
    control_before = control; axevt = true;
    assert(!cdj_c6747_mcasp_tx_slot(&control, 0, &axevt) && axevt);
    assert(!memcmp(&control, &control_before, sizeof(control)));
    assert(!cdj_c6747_mcasp_tx_slot(&control, 3, &axevt) && axevt);
    assert(!cdj_c6747_mcasp_tx_slot(&control, 0, NULL));

    base = 0x01d00000;
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0x180, 1, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xa0, 0x1f00, 4, true));
    control_before = control;
    assert(!cdj_c6747_mcasp_tx_slot(&control, 0, &axevt));
    assert(!memcmp(&control, &control_before, sizeof(control)));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xac, 2u << 7, 4, true));
    assert(cdj_c6747_mcasp_control_write(
               &control, base + 0xb8, 1, 4, true));
    control.axevt_generation[0] = UINT64_MAX;
    control_before = control;
    assert(!cdj_c6747_mcasp_tx_slot(&control, 0, &axevt));
    assert(!memcmp(&control, &control_before, sizeof(control)));

    /* Every XBUF path requires an aligned 32-bit access; buffer registers
     * are write-only in this transmit-only slice. */
    control_before = control;
    assert(!cdj_c6747_mcasp_control_write(
               &control, base + 0x2000, 1, 2, true));
    assert(!cdj_c6747_mcasp_control_write(
               &control, base + 0x2001, 1, 4, true));
    assert(!cdj_c6747_mcasp_control_write(
               &control, base + 0x204, UINT64_C(1) << 32, 4, true));
    assert(!cdj_c6747_mcasp_control_read(&control, base + 0x2000, &v));
    assert(!cdj_c6747_mcasp_control_read(&control, base + 0x204, &v));
    assert(!memcmp(&control, &control_before, sizeof(control)));
    assert(!cdj_c6747_mcasp_axevt_ready(&control, 3));
    assert(cdj_c6747_mcasp_control_valid(&control));
    control.xdma_next[0] = 17;
    assert(!cdj_c6747_mcasp_control_valid(&control));
    control.xdma_next[0] = 16;
    control.xbuf_sequence[0][1] = control.xbuf_writes[0] + 1;
    assert(!cdj_c6747_mcasp_control_valid(&control));
    cdj_c6747_mcasp_control_reset(&control);
    control.xslot[0] = 0x180;
    assert(!cdj_c6747_mcasp_control_valid(&control));
    cdj_c6747_mcasp_control_reset(&control);
    control.xrsr[0][0] = 1;
    assert(!cdj_c6747_mcasp_control_valid(&control));
    cdj_c6747_mcasp_control_reset(&control);
    control.xrsr_source_sequence[0][0] = 1;
    assert(!cdj_c6747_mcasp_control_valid(&control));
    cdj_c6747_mcasp_control_reset(&control);
    control.xrsr[2][4] = 1;
    assert(!cdj_c6747_mcasp_control_valid(&control));
    return 0;
}
