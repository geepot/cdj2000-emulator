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
} System;

static bool sys_read(void *opaque, uint32_t address, uint32_t *value)
{
    System *s = opaque;
    if (address < BASE || address > BASE + SIZE - 4 || (address & 3)) return false;
    if (address == FLAKY_READ && s->ticks % 3 == 0) return false;
    memcpy(value, s->ram + (address - BASE), 4);
    return true;
}

static bool sys_write(void *opaque, uint32_t address, uint64_t value,
                      unsigned size, bool commit)
{
    System *s = opaque;
    if (address < BASE || address + size > BASE + SIZE || (address & (size - 1)))
        return false;
    if (commit) {
        if (address == BAD_COMMIT) return false;
        memcpy(s->ram + (address - BASE), &value, size);
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
    return s->ram + (block - BASE);
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
        do insn = random_instruction(BASE + 4 * (uint32_t)(w - code));
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

static void init_cpu(CdjC674x *cpu, System *s, uint32_t a10, uint32_t b10)
{
    cdj_c674x_reset(cpu, BASE);
    for (unsigned side = 0; side < 2; ++side)
        for (unsigned r = 0; r < 32; ++r) cpu->r[side][r] = rnd();
    cpu->r[0][10] = a10;
    cpu->r[1][10] = b10;
    cpu->r[1][3] = 0;   /* MVC B3,AMR leaves every register linear */
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
    sb = sa;
    uint32_t state = rng_state;
    /* Half the programs point B10 into their own code: self-modifying code. */
    uint32_t b10 = seed & 1 ? BASE + (rnd() % 0x380) * 4 : 0x1900;
    uint32_t a10 = rnd() % 8 == 0 ? FLAKY_READ - 16 * 4 :
                   rnd() % 8 == 0 ? BAD_COMMIT - 8 * 4 : 0x1800;
    state = rng_state;
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
        if (rnd() % 89 == 0) sa.hide = sb.hide = !sa.hide;
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
                jit_armed;

static void present(JitPair *p)
{
    uint32_t pending = rnd() % 23 == 0 ? (1u << (4 + rnd() % 12)) : 0;
    uint64_t armed = UINT64_C(1) << 62;     /* loop interrupt armed */
    bool was = p->a->control_ready[31] & armed;
    bool ra = cdj_c674x_interrupt(p->a, pending);
    bool rb = cdj_c674x_interrupt(p->b, pending);
    assert(ra == rb);
    jit_armed += !was && (p->a->control_ready[31] & armed);
    same(p->a, p->sa, p->b, p->sb, p->seed, p->step);
}

static void step_b(JitPair *p, bool expect)
{
    cdj_c674x_set_packet_cache(0);
    bool rb = cdj_c674x_step(p->b, sys_read, sys_write, p->sb);
    cdj_c674x_set_packet_cache(2);
    assert(rb == expect);
    ++p->step;
    same(p->a, p->sa, p->b, p->sb, p->seed, p->step);
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
    build_loop(&sa);
    sa.ticks = 0;
    sa.hide = false;
    sb = sa;
    uint32_t b10 = 0x1900;
    uint32_t a10 = rnd() % 8 == 0 ? FLAKY_READ - 16 * 4 :
                   rnd() % 8 == 0 ? BAD_COMMIT - 8 * 4 : 0x1800;
    uint32_t state = rng_state;
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

int main(void)
{
    cdj_c674x_set_fetch_block(sys_read, sys_block);
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
    for (unsigned seed = 1; seed <= 3000; ++seed) jit_lockstep(seed);
    printf("JIT lockstep: 3000 programs, %u compiled packets in %u runs "
           "(%u between exits, %u stops), %u loop interrupts, %u faults\n",
           jit_packets, jit_runs, jit_between_exits, jit_stops, jit_armed,
           faults_seen);
    assert(jit_packets > 300000 && jit_armed > 300 && faults_seen > 100);
    return 0;
}
