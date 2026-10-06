/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Differential test of the C674x packet cache and fast path.
 *
 * Two identical systems run random programs in lockstep, one with the cache
 * and fast path (mode 2) and one with neither (mode 0).  After every step the
 * whole CPU and the whole memory must be byte-identical, faults included.
 * The programs store into their own code (self-modifying code), the harness
 * rewrites code between steps (an HPI/EDMA upload), the fetch-block hook
 * withdraws and restores a code window (a remap), and the bus fails a store
 * commit or a load's E3 read on chosen addresses (retirement faults after
 * the fast path has committed).  A second set of programs runs SPLOOP
 * loops of random bodies (loads, stores, NOP n, parallel packets, every II),
 * whose loop-buffer cycles take execute_fast in mode 2.  A few directed
 * cases check the same properties with known outcomes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cdj_c674x.h"

#define BASE 0x1000u
#define SIZE 0x1000u
#define CODE_END 0x1400u
#define BAD_COMMIT 0x1ff0u   /* store check passes, commit fails */
#define FLAKY_READ 0x1fe0u   /* readable only when the tick count % 3 != 0 */

typedef struct {
    uint8_t ram[SIZE];
    unsigned ticks;
    bool hide;               /* fetch-block hook refuses 0x1100..0x11ff */
    bool swap;               /* 0x1100..0x11ff maps to alt[] instead */
    uint8_t alt[0x100];
} System;

/* The fetch epoch (cdj_c674x_set_fetch_epoch): moved whenever hide or swap
 * changes what a fetch block maps to. */
static uint64_t test_epoch;

static uint8_t *sys_at(System *s, uint32_t address)
{
    if (s->swap && address >= 0x1100 && address < 0x1200)
        return s->alt + (address - 0x1100);
    return s->ram + (address - BASE);
}

static bool sys_read(void *opaque, uint32_t address, uint32_t *value)
{
    System *s = opaque;
    if (address < BASE || address > BASE + SIZE - 4 || (address & 3)) return false;
    if (address == FLAKY_READ && s->ticks % 3 == 0) return false;
    memcpy(value, sys_at(s, address), 4);
    return true;
}

static bool sys_write(void *opaque, uint32_t address, uint64_t value,
                      unsigned size, bool commit)
{
    System *s = opaque;
    if (address < BASE || (uint64_t)address + size > BASE + SIZE ||
        (address & (size - 1)))
        return false;
    if (commit) {
        if (address == BAD_COMMIT) return false;
        if (s->swap && address < 0x1200 && address + size > 0x1100 &&
            (address < 0x1100 || address + size > 0x1200)) {
            for (unsigned i = 0; i < size; ++i)
                *sys_at(s, address + i) = value >> (8 * i);
        } else memcpy(sys_at(s, address), &value, size);
    }
    return true;
}

static const uint8_t *sys_block(void *opaque, uint32_t block)
{
    System *s = opaque;
    if (block < BASE || block >= BASE + SIZE) return NULL;
    if (s->hide && block >= 0x1100 && block < 0x1200) return NULL;
    /* Not plain memory: the read callback refuses it now and then. */
    if (block == (FLAKY_READ & ~31u)) return NULL;
    return sys_at(s, block);
}

static void sys_tick(void *opaque)
{
    System *s = opaque;
    ++s->ticks;
    memcpy(s->ram + SIZE - 4, &s->ticks, 4);
}

static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static unsigned pick_dst(void)
{
    unsigned r;
    do r = rnd() & 15; while (r == 10);   /* A10/B10 are the data bases */
    return r;
}

static uint32_t predicate(void)
{
    static const uint32_t creg[] = {1, 2, 3, 4, 5, 6};
    if (rnd() % 4) return 0;
    return creg[rnd() % 6] << 29 | (rnd() & 1) << 28;
}

/* One random 32-bit instruction at pc (parallel bit added by the caller). */
static uint32_t random_instruction(uint32_t pc)
{
    unsigned s = rnd() & 1, dst = pick_dst(), a = rnd() & 15, b = rnd() & 15;
    unsigned x = rnd() & 1;
    static const uint32_t l_ops[] = {0x78, 0xf8, 0xf78, 0xff8, 0xdf8, 0xa78,
                                     0x8f8, 0xaf8, 0x58};
    switch (rnd() % 12) {
    case 0: return predicate() | dst << 23 | (rnd() & 0xffff) << 7 | 0x28 | s << 1;
    case 1: return predicate() | dst << 23 | (rnd() & 0xffff) << 7 | 0x68 | s << 1;
    case 2: case 3:
        return predicate() | dst << 23 | b << 18 | a << 13 | x << 12 |
               l_ops[rnd() % 9] | s << 1;
    case 4:                               /* SHL .S immediate */
        return predicate() | dst << 23 | b << 18 | (rnd() & 31) << 13 |
               0xca0 | s << 1;
    case 5: case 6: {                     /* LD/ST *+A10/B10[cst] */
        static const unsigned ops[] = {6, 7, 4, 5, 2, 3};
        unsigned y = rnd() & 1, op = ops[rnd() % 6];
        unsigned off = rnd() % 8 == 0 ? 31 : rnd() & 15;
        return predicate() | dst << 23 | 10u << 18 | off << 13 | 1u << 9 |
               y << 7 | op << 4 | 4 | s << 1;
    }
    case 7: {                             /* B .S1/.S2 into the code */
        int32_t target = (int32_t)(BASE + (rnd() % ((CODE_END - BASE) / 32)) * 32);
        int32_t disp = (target - (int32_t)(pc & ~31u)) / 4;
        return predicate() | ((uint32_t)disp & 0x1fffff) << 7 | 0x10 | s << 1;
    }
    case 8: {                             /* BNOP disp,n */
        int32_t target = (int32_t)(BASE + (rnd() % ((CODE_END - BASE) / 32)) * 32);
        int32_t disp = (target - (int32_t)(pc & ~31u)) / 4;
        return predicate() | ((uint32_t)disp & 0xfff) << 16 | (rnd() % 6) << 13 |
               0x120 | s << 1;
    }
    case 9: return (rnd() % 6) << 13;     /* NOP n */
    case 10: {                            /* MVC to AMR (slow path) or read */
        if (rnd() & 1) return 0u << 23 | 3u << 18 | 0x3a2;   /* MVC B3,AMR */
        return dst << 23 | 1u << 18 | 0x3e2;                 /* MVC CSR,Bdst */
    }
    default:                              /* ADDK */
        return predicate() | dst << 23 | (rnd() & 0xffff) << 7 | 0x50 | s << 1;
    }
}

/* Loop-body extras: the operations the NXS audio kernels pipeline - MPYSP,
 * ADDSP/SUBSP (every encoding), and loads/stores of every addressing mode
 * and width (LDNDW/STNDW, LDDW/STDW, LDNW/STNW included) through A4-A7 and
 * B4-B7, which init_cpu points into data RAM, some of them circular. */
static unsigned pick_body_dst(void)
{
    unsigned r;
    do r = rnd() & 15; while (r == 10 || (r >= 4 && r <= 7));
    return r;
}

/* Kernel-shaped bodies: only unpredicated SP and memory operations (and
 * NOPs), the shape steady-state kernels compile. */
static bool kernel_body, direct_extras;

/* Direct-trace extras: CMPSP (FAUCR in place), 16x16 and half-by-word
 * multiplies (delayed results through the generic arm path), ADDA/SUBA
 * .D, the long ADDAB/H/W B14/B15 form and ADDKPC (multicycle). */
static uint32_t random_extra(void)
{
    unsigned s = rnd() & 1, x = rnd() & 1, dst = pick_body_dst();
    unsigned a = rnd() & 15, b = rnd() & 15;
    switch (rnd() % 6) {
    case 0:
        return predicate() | dst << 23 | b << 18 | a << 13 | x << 12 |
               (0x38 + rnd() % 3) << 6 | 0x20 | s << 1;        /* CMPxxSP */
    case 1: {
        static const unsigned ops[] = {0x19, 0x18, 0x01, 0x09, 0x0f, 0x03};
        return predicate() | dst << 23 | b << 18 | a << 13 | x << 12 |
               ops[rnd() % 6] << 7 | s << 1;                   /* MPY16 */
    }
    case 2: {
        static const unsigned ops[] = {0x0e, 0x10};
        return predicate() | dst << 23 | b << 18 | a << 13 | x << 12 |
               ops[rnd() % 2] << 6 | 0x30 | s << 1;            /* MPYIH/IL */
    }
    case 3:
        return predicate() | dst << 23 | (4 + rnd() % 4) << 18 |
               (rnd() & 7) << 13 | (0x30 + rnd() % 14) << 7 | 0x40 |
               s << 1;                                          /* ADDA .D */
    case 4:
        return 1u << 28 | dst << 23 | (rnd() & 0x7fff) << 8 |
               (rnd() & 1) << 7 | (rnd() % 3 == 0 ? 0x3c : 0x7c) |
               s << 1;                                          /* ADDAW */
    default:
        return predicate() | dst << 23 | (rnd() & 127) << 16 |
               (rnd() % 6) << 13 | 0x162 | 2;                   /* ADDKPC */
    }
}

static uint32_t random_body(uint32_t pc)
{
    if (direct_extras && rnd() % 4 == 0) return random_extra();
    if (kernel_body) {
        if (rnd() % 4 == 0) return rnd() % 3 ? 0 : (rnd() % 4) << 13;
    } else if (rnd() % 2) return random_instruction(pc);
    unsigned s = rnd() & 1, x = rnd() & 1, dst = pick_body_dst();
    unsigned a = rnd() & 15, b = rnd() & 15;
    switch (rnd() % 4) {
    case 0:
        return (kernel_body ? 0 : predicate()) | dst << 23 | b << 18 |
               a << 13 | x << 12 | 0xe00 | s << 1;             /* MPYSP */
    case 1: {
        static const uint32_t enc[] = {0x218, 0xe18, 0x238, 0x2b8, 0xe38,
                                       0xeb8};
        return (kernel_body ? 0 : predicate()) | dst << 23 | b << 18 |
               a << 13 | x << 12 | enc[rnd() % 6] | s << 1;    /* ADD/SUBSP */
    }
    default: {
        static const unsigned modes[] = {0, 1, 8, 9, 10, 11, 1, 9, 11, 5, 13};
        unsigned mode = modes[rnd() % 11];
        unsigned offset = (mode & 4) ? 1 + rnd() % 2 : rnd() % 8; /* A1/A2.. */
        bool extended = rnd() % 3 == 0;
        unsigned op = rnd() & 7;
        if (extended && op < 2) op += 2;
        if ((extended && op != 3 && op != 5) && (dst & 1)) dst &= ~1u;
        return (kernel_body ? 0 : predicate()) | dst << 23 |
               (4 + rnd() % 4) << 18 |
               offset << 13 | mode << 9 | (extended ? 0x100u : 0) |
               (rnd() & 1) << 7 | op << 4 | 4 | s << 1;
    }
    }
}

static void build(System *s)
{
    memset(s->ram, 0, sizeof s->ram);
    for (uint32_t pc = BASE; pc < CODE_END; pc += 4) {
        uint32_t w = random_instruction(pc);
        if ((pc & 31) != 28 && rnd() % 4 == 0) w |= 1;
        memcpy(s->ram + (pc - BASE), &w, 4);
    }
    for (uint32_t i = CODE_END - BASE; i < SIZE - 4; i += 4) {
        uint32_t v = rnd();
        memcpy(s->ram + i, &v, 4);
    }
}

/* MVK n,B3; MVC B3,ILC; NOP 4; SPLOOP ii; body; SPKERNEL; post; B BASE. */
static void build_loop(System *s)
{
    build(s);
    uint32_t code[CODE_END - BASE], *w = code;
    unsigned n = 1 + (rnd() % 4 ? rnd() % 12 : rnd() % 200), len = 1 + rnd() % 20;
    unsigned ii = 1 + rnd() % (len < 14 ? len + 1 : 14);
    *w++ = 3u << 23 | n << 7 | 0x28 | 2;           /* MVK.S2 n,B3 */
    *w++ = 13u << 23 | 3u << 18 | 0x3a2;            /* MVC.S2 B3,ILC */
    *w++ = 3u << 13;                                /* NOP 4 */
    *w++ = (ii - 1) << 23 | 0x38000;                /* SPLOOP ii */
    for (unsigned i = 0; i < len; ++i) {
        uint32_t insn;
        do insn = random_body(BASE + 4 * (uint32_t)(w - code));
        while ((insn & 0x7c) == 0x10 || (insn & 0x1ffc) == 0x120 ||
               (insn & 0xffe) == 0x3a2 || (insn & 0xffe) == 0x3e2);
        if ((insn & 0x1ffff) == 0 && rnd() % 3) insn = 0;   /* mostly NOP 1 */
        if (i + 1 < len && ((BASE + 4 * (uint32_t)(w - code)) & 31) != 28 &&
            rnd() % 3 == 0)
            insn |= 1;
        *w++ = insn;
    }
    *w++ = 0x34000;                                 /* SPKERNEL 0,0 */
    for (unsigned i = rnd() % 4; i; --i) {
        uint32_t insn;
        do insn = random_instruction(BASE + 4 * (uint32_t)(w - code));
        while ((insn & 0x7c) == 0x10 || (insn & 0x1ffc) == 0x120 ||
               (insn & 0xffe) == 0x3a2);
        *w++ = insn;
    }
    uint32_t pc = BASE + 4 * (uint32_t)(w - code);
    *w++ = ((uint32_t)(-(int32_t)((pc & ~31u) - BASE) / 4) & 0x1fffff) << 7 |
           0x10;                                    /* B.S1 BASE */
    *w++ = 4u << 13;                                /* NOP 5 */
    memcpy(s->ram, code, 4 * (size_t)(w - code));
}

static bool loop_bases;

static void init_cpu(CdjC674x *cpu, System *s, uint32_t a10, uint32_t b10)
{
    cdj_c674x_reset(cpu, BASE);
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned r = 0; r < 32; ++r) cpu->r[side][r] = rnd();
    cpu->r[0][10] = a10;
    cpu->r[1][10] = b10;
    cpu->r[1][3] = 0;   /* MVC B3,AMR leaves every register linear */
    if (loop_bases) {
        /* Data pointers for random_body: A4-A7/B4-B7 into 0x1800-0x1bff,
         * small index registers A1/A2/B1/B2, and circular addressing for
         * some of the pointers (BK0 64..512 bytes, BK1 32..1024). */
        for (unsigned side = 0; side < 2; ++side) {
            for (unsigned r = 4; r <= 7; ++r)
                cpu->r[side][r] = 0x1800 + (rnd() % 0x80) * 8;
            cpu->r[side][1] = rnd() % 4;
            cpu->r[side][2] = rnd() % 4;
        }
        uint32_t amr = (5 + rnd() % 4) << 16 | (4 + rnd() % 6) << 21;
        for (unsigned field = 0; field < 8; ++field)
            if (rnd() % 3) amr |= (1 + rnd() % 2) << (field * 2);
        cpu->control[0] = amr;
        /* FADCR/FMCR rounding modes for both units (bits 10:9, 26:25). */
        cpu->control[18] = (rnd() & 3) << 9 | (rnd() & 3) << 25;
        cpu->control[20] = (rnd() & 3) << 9 | (rnd() & 3) << 25;
    }
    cpu->cycle_tick = sys_tick;
    cpu->cycle_opaque = s;
}

static void same(const CdjC674x *a, const System *sa, const CdjC674x *b,
                 const System *sb, unsigned seed, unsigned step)
{
    CdjC674x x = *a, y = *b;
    x.cycle_opaque = y.cycle_opaque = NULL;
    bool fault = a->fault != b->fault ||
                 (a->fault && strcmp(a->fault, b->fault));
    x.fault = y.fault = NULL;
    if (fault || memcmp(&x, &y, sizeof x) || memcmp(sa, sb, sizeof *sa)) {
        fprintf(stderr, "seed %u step %u: cached and uncached diverge "
                "(fault %s / %s, pc %08x / %08x)\n", seed, step,
                a->fault ? a->fault : "-", b->fault ? b->fault : "-",
                a->pc, b->pc);
        abort();
    }
}

static unsigned packets, faults_seen, loop_steps;

static void lockstep(unsigned seed, bool loops)
{
    static System sa, sb;
    static CdjC674x a, b;
    rng_state = seed * 2654435761u + 1;
    if (loops) build_loop(&sa);
    else build(&sa);
    sa.ticks = 0;
    sa.hide = false;
    ++test_epoch;
    sb = sa;
    uint32_t state = rng_state;
    /* Half the programs point B10 into their own code: self-modifying code. */
    uint32_t b10 = seed & 1 ? BASE + (rnd() % 0x380) * 4 : 0x1900;
    uint32_t a10 = rnd() % 8 == 0 ? FLAKY_READ - 16 * 4 :
                   rnd() % 8 == 0 ? BAD_COMMIT - 8 * 4 : 0x1800;
    state = rng_state;
    loop_bases = loops;
    init_cpu(&a, &sa, a10, b10);
    rng_state = state;
    init_cpu(&b, &sb, a10, b10);
    same(&a, &sa, &b, &sb, seed, 0);
    for (unsigned step = 1; step <= 3000; ++step) {
        if (!loops && rnd() % 97 == 0) {  /* host upload into code */
            uint32_t pc = BASE + (rnd() % ((CODE_END - BASE) / 4)) * 4;
            uint32_t w = random_instruction(pc);
            memcpy(sa.ram + (pc - BASE), &w, 4);
            memcpy(sb.ram + (pc - BASE), &w, 4);
        }
        if (rnd() % 89 == 0) {
            sa.hide = sb.hide = !sa.hide;
            ++test_epoch;
        }
        cdj_c674x_set_packet_cache(2);
        uint64_t before = a.packets;
        loop_steps += a.loop_active;
        bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
        cdj_c674x_set_packet_cache(0);
        bool rb = cdj_c674x_step(&b, sys_read, sys_write, &sb);
        assert(ra == rb);
        same(&a, &sa, &b, &sb, seed, step);
        packets += a.packets != before;
        if (!ra) {
            ++faults_seen;
            return;
        }
    }
}

/* Directed: a store into the next packet's code is seen when it executes. */
static void self_modifying(void)
{
    static System s;
    CdjC674x cpu;
    memset(&s, 0, sizeof s);
    uint32_t code[] = {
        2u << 23 | 0x1234u << 7 | 0x28,                      /* MVK.S1 0x1234,A2 */
        3u << 23 | 10u << 18 | 3u << 13 | 1u << 9 | 7u << 4 | 4, /* STW A3,*+A10[3] */
        4u << 13,                                            /* NOP 5 */
        0x10,                                                /* B.S1 0x1000 */
        4u << 13,                                            /* NOP 5 */
    };
    memcpy(s.ram, code, sizeof code);
    cdj_c674x_set_packet_cache(2);
    cdj_c674x_reset(&cpu, BASE);
    cpu.r[0][10] = BASE;                  /* *+A10[3] is the B at 0x100c */
    cpu.r[0][3] = code[3];                /* first pass stores the B itself */
    for (unsigned i = 0; i < 5; ++i)
        assert(cdj_c674x_step(&cpu, sys_read, sys_write, &s));
    assert(cpu.pc == BASE);               /* looped: every packet now cached */
    cpu.r[0][3] = 7u << 23 | 0x55u << 7 | 0x28;   /* MVK.S1 0x55,A7 */
    for (unsigned i = 0; i < 5; ++i)
        assert(cdj_c674x_step(&cpu, sys_read, sys_write, &s));
    /* The cached branch was overwritten by the DSP's own store. */
    assert(cpu.r[0][7] == 0x55 && cpu.pc == 0x1014);
    /* Second pass over unchanged code is served from the cache; a host
     * rewrite of the first word must still be seen. */
    uint32_t mvk2 = 2u << 23 | 0x4321u << 7 | 0x28;
    memcpy(s.ram, &mvk2, 4);
    cpu.pc = BASE;
    assert(cdj_c674x_step(&cpu, sys_read, sys_write, &s));
    assert(cpu.r[0][2] == 0x4321);
}

/* JIT lockstep: system A runs loop-buffer cycles through cdj_c674x_run
 * (compiled), system B steps the interpreter with the cache off.  B takes
 * its step inside A's between callback, right after A's packet, so the two
 * are compared after every packet; both then get the same random interrupt
 * request (GIE and IER are set, so loops drain and vector).  Random run
 * limits and between() refusals cover every cdj_c674x_run exit. */
typedef struct {
    CdjC674x *a, *b;
    System *sa, *sb;
    unsigned seed, step;
} JitPair;

static unsigned jit_packets, jit_runs, jit_between_exits, jit_stops,
                jit_armed, quiet_checks, loud_checks;

/* The board horizon (cdj_c674x.h): after a presentation without a request,
 * half the time A may skip between() for a few packets (or until a random
 * break PC); B then catches up those steps, each followed by the no-op
 * presentation the skip stood for, and the two are compared after the
 * last.  Seeds with horizon_off run without one. */
static CdjC674xHorizon test_horizon;
static unsigned horizon_skips;
/* What bounded the horizon: a request the board will present when the
 * packet count reaches the bound, or at the break PC.  B's catch-up
 * presents it wherever B reaches it, A only where its between() runs, so a
 * core that skipped past the bound diverges. */
static uint64_t event_packets;
static uint32_t event_pc, event_mask;

static uint32_t horizon_event(const CdjC674x *cpu)
{
    if (!event_mask ||
        !(cpu->packets == event_packets || (event_pc && cpu->pc == event_pc)))
        return 0;
    uint32_t mask = event_mask;
    event_mask = 0;
    return mask;
}

static void catch_up(JitPair *p)
{
    unsigned skipped = test_horizon.skipped;
    test_horizon.skipped = 0;
    horizon_skips += skipped;
    for (unsigned k = 0; k < skipped; ++k) {
        cdj_c674x_set_packet_cache(0);
        assert(cdj_c674x_step(p->b, sys_read, sys_write, p->sb));
        cdj_c674x_set_packet_cache(2);
        ++p->step;
        uint32_t mask = horizon_event(p->b);
        assert(cdj_c674x_interrupt(p->b, mask));
        if (mask) event_mask = mask;    /* A's turn to look for it */
    }
}

static void horizon_open(JitPair *p, uint32_t pending)
{
    test_horizon.until = 0;
    test_horizon.break_pc = 0;
    event_mask = 0;
    if (pending || rnd() % 2) return;
    test_horizon.until = p->a->packets + 1 + rnd() % 12;
    if (rnd() % 4 == 0) test_horizon.break_pc = BASE + (rnd() % 64) * 4;
    if (rnd() % 2) {
        event_packets = test_horizon.until;
        event_pc = test_horizon.break_pc;
        event_mask = 1u << (4 + rnd() % 12);
    }
}

static void present(JitPair *p)
{
    /* Interrupt recognition never reads the queues while a loop is active,
     * which is the only time steady execution lasts across between(). */
    uint32_t pending = rnd() % 23 == 0 ? (1u << (4 + rnd() % 12)) : 0;
    pending |= horizon_event(p->a);
    uint64_t armed = UINT64_C(1) << 62;     /* loop interrupt armed */
    bool was = p->a->control_ready[31] & armed;
    /* cdj_c674x_interrupt_quiet: a no-op presentation, exactly. */
    static CdjC674x probe;
    cdj_c674x_view(p->a, &probe);
    if (cdj_c674x_interrupt_quiet(&probe)) {
        static CdjC674x after;
        after = probe;
        assert(cdj_c674x_interrupt(&after, 0) &&
               !memcmp(&after, &probe, sizeof probe));
        ++quiet_checks;
    } else ++loud_checks;
    bool ra = cdj_c674x_interrupt(p->a, pending);
    bool rb = cdj_c674x_interrupt(p->b, pending);
    assert(ra == rb);
    horizon_open(p, pending);
    jit_armed += !was && (p->a->control_ready[31] & armed);
    static CdjC674x view;
    cdj_c674x_view(p->a, &view);
    same(&view, p->sa, p->b, p->sb, p->seed, p->step);
}

static void step_b(JitPair *p, bool expect)
{
    catch_up(p);
    cdj_c674x_set_packet_cache(0);
    bool rb = cdj_c674x_step(p->b, sys_read, sys_write, p->sb);
    cdj_c674x_set_packet_cache(2);
    assert(rb == expect);
    ++p->step;
    /* Inside a run A may be in a steady-state kernel, whose queue arrays
     * are rebuilt only when it ends: compare the rebuilt view. */
    static CdjC674x view;
    cdj_c674x_view(p->a, &view);
    same(&view, p->sa, p->b, p->sb, p->seed, p->step);
}

/* A run that ends with CDJ_C674X_RUN_BETWEEN after a skipped between()
 * leaves B behind: the skip stood for that between(). */
static void catch_up_compare(JitPair *p)
{
    if (!test_horizon.skipped) return;
    catch_up(p);
    static CdjC674x view;
    cdj_c674x_view(p->a, &view);
    same(&view, p->sa, p->b, p->sb, p->seed, p->step);
}

static bool jit_between(void *opaque)
{
    JitPair *p = opaque;
    ++jit_packets;
    step_b(p, true);
    if (rnd() % 61 == 0) return false;
    present(p);
    return true;
}

static void jit_lockstep(unsigned seed)
{
    static System sa, sb;
    static CdjC674x a, b;
    rng_state = seed * 2654435761u + 7;
    kernel_body = seed % 3 == 1;
    build_loop(&sa);
    kernel_body = false;
    /* Functional timing: interrupt entry then sizes its pipe-down from the
     * queues, which between() reads right after a loop ends. */
    cdj_c674x_loop_set_functional_timing(seed % 4 == 3);
    sa.ticks = 0;
    sa.hide = false;
    ++test_epoch;
    sb = sa;
    uint32_t b10 = 0x1900;
    uint32_t a10 = rnd() % 8 == 0 ? FLAKY_READ - 16 * 4 :
                   rnd() % 8 == 0 ? BAD_COMMIT - 8 * 4 : 0x1800;
    uint32_t state = rng_state;
    loop_bases = true;
    init_cpu(&a, &sa, a10, b10);
    rng_state = state;
    init_cpu(&b, &sb, a10, b10);
    if (seed & 1) {                       /* interrupts recognized */
        a.control[1] |= 1; b.control[1] |= 1;          /* GIE */
        a.control[4] = b.control[4] = 0xfff3;          /* IER */
        a.control[5] = b.control[5] = BASE;            /* ISTP */
    }
    JitPair p = {&a, &b, &sa, &sb, seed, 0};
    bool pre_done = false;
    while (p.step < 4000) {
        if (!pre_done) present(&p);
        pre_done = false;
        if (a.loop_active && a.packets == b.packets) {
            unsigned status, limit = 1 + rnd() % 64;
            unsigned n = cdj_c674x_run(&a, sys_read, sys_write, &sa, limit,
                                       jit_between, &p, &status);
            jit_runs += n != 0;
            if (status == CDJ_C674X_RUN_FAULT) {
                step_b(&p, false);
                ++faults_seen;
                return;
            }
            if (status == CDJ_C674X_RUN_BETWEEN) {
                catch_up_compare(&p);
                ++jit_between_exits;
                pre_done = true;
                continue;
            }
            if (status == CDJ_C674X_RUN_STOPPED) {
                ++jit_stops;
                continue;
            }
            if (n) {
                ++jit_packets;
                step_b(&p, true);
                continue;
            }
        }
        bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
        step_b(&p, ra);
        if (!ra) {
            ++faults_seen;
            return;
        }
    }
}

/* Direct-trace lockstep: random direct programs (build()'s instruction mix
 * with half the slots replaced by random_body's MPYSP, ADDSP/SUBSP and
 * every load/store form through A4-A7/B4-B7) run through cdj_c674x_run
 * (system A) against the uncached interpreter (B), compared after every
 * packet, with random interrupts, run limits and between() refusals, host
 * code uploads, a withdrawn code window and failing bus operations. */
static void build_direct(System *s)
{
    build(s);
    direct_extras = true;
    for (uint32_t pc = BASE; pc < CODE_END; pc += 4) {
        uint32_t w;
        memcpy(&w, s->ram + (pc - BASE), 4);
        /* Keep the data pointers A4-A7/B4-B7 mostly intact, so programs
         * run long enough to matter (a few still clobber them). */
        unsigned dst = (w >> 23) & 31;
        if ((w & 0x7c) != 0x10 && (w & 0x1ffc) != 0x120 && dst >= 4 &&
            dst <= 7 && rnd() % 8)
            w = (w & ~(31u << 23)) | (8u + (dst & 1)) << 23;
        memcpy(s->ram + (pc - BASE), &w, 4);
        if (rnd() % 2) continue;
        uint32_t body;
        do body = random_body(pc);
        while ((body & 0x7c) == 0x10 || (body & 0x1ffc) == 0x120);
        w = (body & ~1u) | (w & 1);
        memcpy(s->ram + (pc - BASE), &w, 4);
    }
    direct_extras = false;
}

static unsigned dt_runs;

static void dt_lockstep(unsigned seed)
{
    static System sa, sb;
    static CdjC674x a, b;
    rng_state = seed * 2654435761u + 11;
    build_direct(&sa);
    /* Other code behind the swappable window. */
    for (unsigned i = 0; i < sizeof sa.alt; i += 4) {
        uint32_t w = random_instruction(0x1100 + i) | (rnd() & 1);
        memcpy(sa.alt + i, &w, 4);
    }
    sa.swap = false;
    cdj_c674x_loop_set_functional_timing(seed % 4 == 3);
    sa.ticks = 0;
    sa.hide = false;
    ++test_epoch;
    sb = sa;
    uint32_t b10 = seed & 1 ? BASE + (rnd() % 0x380) * 4 : 0x1900;
    uint32_t a10 = rnd() % 8 == 0 ? FLAKY_READ - 16 * 4 :
                   rnd() % 8 == 0 ? BAD_COMMIT - 8 * 4 : 0x1800;
    uint32_t state = rng_state;
    loop_bases = true;
    init_cpu(&a, &sa, a10, b10);
    rng_state = state;
    init_cpu(&b, &sb, a10, b10);
    if (seed & 2) {                       /* interrupts recognized */
        a.control[1] |= 1; b.control[1] |= 1;
        a.control[4] = b.control[4] = 0xfff3;
        a.control[5] = b.control[5] = BASE;
    }
    JitPair p = {&a, &b, &sa, &sb, seed, 0};
    bool pre_done = false;
    while (p.step < 3000) {
        if (!pre_done) {
            if (rnd() % 97 == 0) {        /* host upload into code */
                uint32_t pc = BASE + (rnd() % ((CODE_END - BASE) / 4)) * 4;
                uint32_t w = random_instruction(pc);
                memcpy(sa.ram + (pc - BASE), &w, 4);
                memcpy(sb.ram + (pc - BASE), &w, 4);
            }
            if (rnd() % 89 == 0) {
                sa.hide = sb.hide = !sa.hide;
                ++test_epoch;
            }
            if (rnd() % 83 == 0) {
                sa.swap = sb.swap = !sa.swap;
                ++test_epoch;
            }
            present(&p);
        }
        pre_done = false;
        if (a.packets == b.packets) {
            unsigned status, limit = 1 + rnd() % 64;
            unsigned n = cdj_c674x_run(&a, sys_read, sys_write, &sa, limit,
                                       jit_between, &p, &status);
            dt_runs += n != 0;
            if (status == CDJ_C674X_RUN_FAULT) {
                step_b(&p, false);
                ++faults_seen;
                return;
            }
            if (status == CDJ_C674X_RUN_BETWEEN) {
                catch_up_compare(&p);
                pre_done = true;
                continue;
            }
            if (status == CDJ_C674X_RUN_STOPPED) continue;
            if (n) {
                ++jit_packets;
                step_b(&p, true);
                continue;
            }
        }
        bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
        step_b(&p, ra);
        if (!ra) {
            ++faults_seen;
            return;
        }
    }
}

/* Directed direct-trace declines after state the plan changes in place:
 * a delayed-result append (MPY), FAUCR (CMPGTSP on NaN) and ILC are each
 * followed in their packet by a load that faults at issue (unaligned), so
 * the trace must put everything back before the interpreter faults. */
/* Fill the stack the next call will use, so a slot restored from an
 * unrecorded undo entry cannot happen to hold the right bytes. */
static __attribute__((noinline)) void stack_poison(void)
{
    volatile uint8_t junk[64 * 1024];
    for (size_t i = 0; i < sizeof junk; ++i) junk[i] = 0xa5;
}

static void dt_directed(void)
{
    static System sa, sb;
    static CdjC674x a, b;
    const uint32_t nan = 0x7fc00000u;
    const uint32_t first[] = {
        8u << 23 | 2u << 18 | 3u << 13 | 0x19u << 7 | 1,          /* MPY .M1 */
        8u << 23 | 2u << 18 | 3u << 13 | 0x39u << 6 | 0x20 | 1,   /* CMPGTSP */
    };
    for (unsigned k = 0; k < 2; ++k) {
        memset(&sa, 0, sizeof sa);
        uint32_t code[8] = {
            first[k],
            9u << 23 | 10u << 18 | 1u << 13 | 1u << 9 | 6u << 4 | 4,  /* LDW */
            4u << 13,                                             /* NOP 5 */
        };
        memcpy(sa.ram, code, sizeof code);
        sb = sa;
        for (unsigned side = 0; side < 2; ++side) {
            CdjC674x *cpu = side ? &b : &a;
            cdj_c674x_reset(cpu, BASE);
            cpu->r[0][2] = cpu->r[0][3] = nan;
            cpu->r[0][10] = 0x1801;                  /* unaligned base */
            cpu->control[19] = 0x00000005u;
            cpu->cycle_tick = sys_tick;
            cpu->cycle_opaque = side ? &sb : &sa;
            /* A queued entry in the slot the append will use: its bytes
             * must survive the decline. */
            cpu->loads[0] = (CdjC674xLoad){.due = 1000, .dst = 7,
                                           .address = 0xabcd, .size = 4};
        }
        /* Fill the packet cache with this code (the plan comes from it). */
        static System sc;
        static CdjC674x c;
        sc = sa;
        c = a;
        c.cycle_opaque = &sc;
        c.loads[0].address = 0x5555;    /* other bytes on the stack */
        assert(!cdj_c674x_step(&c, sys_read, sys_write, &sc));
        CdjC674xJitStats before, after;
        cdj_c674x_jit_stats(&before);
        stack_poison();
        unsigned status;
        unsigned n = cdj_c674x_run(&a, sys_read, sys_write, &sa, 1, NULL,
                                   NULL, &status);
        cdj_c674x_jit_stats(&after);
        assert(!n && !status && after.direct_plans == before.direct_plans + 1);
        bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
        cdj_c674x_set_packet_cache(0);
        bool rb = cdj_c674x_step(&b, sys_read, sys_write, &sb);
        cdj_c674x_set_packet_cache(2);
        assert(!ra && !rb);
        same(&a, &sa, &b, &sb, 0, k);
    }
}

static bool always(void *opaque) { (void)opaque; return true; }

/* Directed: a CMPEQDP's delayed FAUCR effect lands in the cycle a CMPGTSP
 * writes FAUCR status (both on NaN): the interpreter refuses the second
 * packet, so the trace must decline it. */
static void dt_directed_faucr(void)
{
    static System sa, sb, sc;
    static CdjC674x a, b, c;
    memset(&sa, 0, sizeof sa);
    uint32_t code[8] = {
        8u << 23 | 4u << 18 | 2u << 13 | 0xa20,                  /* CMPEQDP */
        9u << 23 | 2u << 18 | 3u << 13 | 0x39u << 6 | 0x20,      /* CMPGTSP */
        4u << 13,                                                 /* NOP 5 */
    };
    memcpy(sa.ram, code, sizeof code);
    sb = sc = sa;
    CdjC674x *cpus[3] = {&a, &b, &c};
    System *systems[3] = {&sa, &sb, &sc};
    for (unsigned i = 0; i < 3; ++i) {
        cdj_c674x_reset(cpus[i], BASE);
        for (unsigned r = 2; r <= 5; ++r) cpus[i]->r[0][r] = 0x7ff80000u;
        cpus[i]->cycle_tick = sys_tick;
        cpus[i]->cycle_opaque = systems[i];
    }
    /* Fill the packet cache with both packets. */
    assert(cdj_c674x_step(&c, sys_read, sys_write, &sc));
    assert(!cdj_c674x_step(&c, sys_read, sys_write, &sc));
    unsigned status;
    unsigned n = cdj_c674x_run(&a, sys_read, sys_write, &sa, 2, always, NULL,
                               &status);
    assert(n == 1 && status == CDJ_C674X_RUN_BETWEEN);
    bool ra = cdj_c674x_step(&a, sys_read, sys_write, &sa);
    cdj_c674x_set_packet_cache(0);
    bool rb = cdj_c674x_step(&b, sys_read, sys_write, &sb) &&
              cdj_c674x_step(&b, sys_read, sys_write, &sb);
    cdj_c674x_set_packet_cache(2);
    assert(!ra && !rb && !strcmp(a.fault, "delayed FP-status write conflict"));
    same(&a, &sa, &b, &sb, 0, 3);
}

int main(void)
{
    cdj_c674x_set_fetch_block(sys_read, sys_block);
    cdj_c674x_set_fetch_epoch(&test_epoch);
    self_modifying();
    for (unsigned seed = 1; seed <= 3000; ++seed) lockstep(seed, false);
    printf("packet cache lockstep: 3000 programs, %u packets, %u faults\n",
           packets, faults_seen);
    assert(packets > 1000000 && faults_seen > 100);
    packets = faults_seen = 0;
    for (unsigned seed = 1; seed <= 3000; ++seed) lockstep(seed, true);
    printf("SPLOOP lockstep: 3000 programs, %u packets, %u loop-buffer steps,"
           " %u faults\n", packets, loop_steps, faults_seen);
    assert(loop_steps > 300000 && faults_seen > 100);
    faults_seen = 0;
    cdj_c674x_set_jit(1);
    for (unsigned seed = 1; seed <= 3000; ++seed) {
        cdj_c674x_set_horizon(seed % 5 ? &test_horizon : NULL);
        assert(!test_horizon.skipped);
        test_horizon.until = 0;
        jit_lockstep(seed);
    }
    CdjC674xJitStats stats;
    cdj_c674x_jit_stats(&stats);
    cdj_c674x_loop_set_functional_timing(false);
    printf("JIT lockstep: 3000 programs, %u compiled packets in %u runs "
           "(%u between exits, %u stops; %llu native, %llu generic, "
           "%llu steady), %u loop interrupts, %u faults\n",
           jit_packets, jit_runs, jit_between_exits, jit_stops,
           (unsigned long long)stats.native,
           (unsigned long long)stats.generic,
           (unsigned long long)stats.steady, jit_armed, faults_seen);
    assert(jit_packets > 300000 && jit_armed > 300 && faults_seen > 100 &&
           stats.native > 300000 && stats.generic > 10000 &&
           stats.steady > 100000);
    jit_packets = faults_seen = 0;
    dt_directed();
    dt_directed_faucr();
    for (unsigned seed = 1; seed <= 24000; ++seed) {
        cdj_c674x_set_horizon(seed % 5 ? &test_horizon : NULL);
        assert(!test_horizon.skipped);
        test_horizon.until = 0;
        dt_lockstep(seed);
    }
    cdj_c674x_set_horizon(NULL);
    cdj_c674x_loop_set_functional_timing(false);
    CdjC674xJitStats after;
    cdj_c674x_jit_stats(&after);
    printf("direct-trace lockstep: 24000 programs, %llu traced packets in "
           "%llu runs (%u with a packet), %llu plans, %llu untraceable, %u faults\n",
           (unsigned long long)(after.direct - stats.direct),
           (unsigned long long)(after.direct_runs - stats.direct_runs), dt_runs,
           (unsigned long long)(after.direct_plans - stats.direct_plans),
           (unsigned long long)(after.direct_untraceable -
                                stats.direct_untraceable), faults_seen);
    assert(after.direct - stats.direct > 3000000 && faults_seen > 1000);
    printf("interrupt presentations: %u quiet (checked no-op), %u not\n",
           quiet_checks, loud_checks);
    assert(quiet_checks > 1000000 && loud_checks > 10000);
    printf("board horizon: %u between() calls skipped\n", horizon_skips);
    assert(horizon_skips > 1000000);
    return 0;
}
