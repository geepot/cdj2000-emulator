/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stddef.h>
#include "cdj_c674x.h"
#include "cdj_c6747_pll.h"
static CdjC6747Pll pll;
static void tick(void *opaque) { cdj_c6747_pll_tick(opaque); }
static bool read_bus(void *unused, uint32_t a, uint32_t *v)
{ (void)unused; return cdj_c6747_pll_read(&pll, a, v); }
static bool write_bus(void *unused, uint32_t a, uint64_t v, unsigned size, bool commit)
{ (void)unused; return cdj_c6747_pll_write(&pll, a, v, size, commit); }
int main(void)
{
    for (unsigned split = 0; split < 2; ++split) {
        CdjC674x cpu;
        cdj_c6747_pll_reset(&pll); cdj_c674x_reset(&cpu, 0x1000);
        cpu.cycle_tick = tick; cpu.cycle_opaque = &pll;
        cpu.r[0][5] = 0x01c11138; cpu.r[0][3] = 1;
        CdjC674xPacket packet = {.count = 1, .next_pc = 0x1004,
            .instructions = {{.pc = 0x1000, .word = 0x01940274}}}; /* STW A3,*A5 */
        assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
        assert(!pll.go_remaining && cpu.store_count == 1);
        packet.instructions[0].word = 0;
        assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
        assert(!pll.go_remaining);
        assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
        assert(cpu.cycles == 3 && pll.go_remaining == 8); /* E3 starts GO */
        if (split) {
            for (unsigned i = 0; i < 8; ++i) {
                assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
                assert(pll.go_remaining == 7 - i);
            }
        } else {
            packet.instructions[0].word = 7u << 13; /* NOP 8, one API call */
            assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
        }
        assert(cpu.cycles == 11 && !pll.go_remaining);
        /* E3 read observes completion inside a protected load's NOPs. */
        assert(cdj_c6747_pll_write(&pll, 0x01c11138, 1, 4, true));
        for (unsigned i = 0; i < 5; ++i) cdj_c6747_pll_tick(&pll);
        cpu.r[0][5] = 0x01c1113c;
        packet.instructions[0].word = 0x01940264;
        packet.instructions[0].header = 1u << 20;
        assert(cdj_c674x_execute(&cpu, &packet, read_bus, write_bus, NULL));
        assert(cpu.cycles == 16 && cpu.r[0][3] == 4 && !pll.go_remaining);
    }
    return 0;
}
