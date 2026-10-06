/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic CPU-time benchmark; not a firmware or end-to-end speed test.
 * Build from the repository root:
 * cc -O3 -I emulator/qemu tools/cdj_dsp/benchmark_core.c \
 *   emulator/qemu/cdj_c674x.c \
 *   emulator/qemu/cdj_c674x_mpy.c emulator/qemu/cdj_c674x_dotp.c emulator/qemu/cdj_c674x_packed8.c emulator/qemu/cdj_c674x_packed16.c emulator/qemu/cdj_c674x_packbits.c emulator/qemu/cdj_c674x_mpy32.c emulator/qemu/cdj_c674x_dp.c emulator/qemu/cdj_c674x_approx.c emulator/qemu/cdj_c674x_uncond.c \
 *   emulator/qemu/cdj_c674x_sp.c emulator/qemu/cdj_c674x_control.c \
 *   emulator/qemu/cdj_c674x_loop.c -lm -o /tmp/cdj-core-bench
 * Run alternating baseline/candidate binaries on an otherwise idle host.
 * The step workloads run a small loop from RAM through cdj_c674x_step with a
 * fetch-block hook, as the NXS board does; set CDJ_C674X_PACKET_CACHE=0 or
 * =decode to compare the packet cache's modes in one binary. */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include "cdj_c674x.h"

static uint8_t ram[0x200]; /* 0x1000..0x11ff: code, then data at 0x1100 */

static bool bench_read(void *opaque, uint32_t address, uint32_t *value)
{
    (void)opaque;
    if (address < 0x1000 || address > 0x11fc || (address & 3)) return false;
    memcpy(value, ram + address - 0x1000, 4);
    return true;
}

static bool bench_write(void *opaque, uint32_t address, uint64_t value,
                        unsigned size, bool commit)
{
    (void)opaque;
    if (address < 0x1100 || address + size > 0x1200 || (address & (size - 1)))
        return false;
    if (commit) memcpy(ram + address - 0x1000, &value, size);
    return true;
}

static const uint8_t *bench_block(void *opaque, uint32_t block)
{
    (void)opaque;
    return block >= 0x1000 && block < 0x1200 ? ram + block - 0x1000 : NULL;
}

static void tick(void *opaque) { ++*(unsigned *)opaque; }

static double step_loop(const uint32_t *code, unsigned words, unsigned steps,
                        uint64_t *packets)
{
    memset(ram, 0, sizeof ram);
    memcpy(ram, code, words * 4);
    CdjC674x cpu;
    unsigned ticks = 0;
    cdj_c674x_reset(&cpu, 0x1000);
    cpu.r[0][10] = 0x1100;
    cpu.cycle_tick = tick;
    cpu.cycle_opaque = &ticks;
    cdj_c674x_set_fetch_block(bench_read, bench_block);
    clock_t start = clock();
    for (unsigned i = 0; i < steps; ++i)
        if (!cdj_c674x_step(&cpu, bench_read, bench_write, NULL)) {
            fprintf(stderr, "fault %s at %08x\n", cpu.fault, cpu.fault_pc);
            return -1;
        }
    *packets = cpu.packets;
    return (double)(clock() - start) / CLOCKS_PER_SEC;
}

int main(void)
{
    CdjC674x cpu;
    cdj_c674x_reset(&cpu, 0x1000);
    CdjC674xPacket packet = {
        .count = 1, .next_pc = 0x1004,
        .instructions = {{.word = (3u << 23) | (123u << 7) | 0x28,
                          .pc = 0x1000}},
    };
    clock_t start = clock();
    for (unsigned i = 0; i < 3000000; ++i)
        if (!cdj_c674x_execute(&cpu, &packet, NULL, NULL, NULL)) return 1;
    printf("execute %.6f s; packets=%llu reg=%u state=%zu prefix=%zu\n",
           (double)(clock() - start) / CLOCKS_PER_SEC,
           (unsigned long long)cpu.packets, cpu.r[0][3], sizeof(cpu),
           offsetof(CdjC674x, loop));

    CdjC674xLoop loop;
    cdj_c674x_loop_init(&loop, 8, 10000000);
    loop.sealed = true;
    loop.length = 48;
    for (unsigned i = 0; i < 48; ++i) {
        loop.count[i] = 1;
        loop.tags[i][0] = i;
    }
    uint32_t tags[8];
    unsigned count;
    bool post, drained;
    start = clock();
    for (unsigned i = 0; i < 3000000; ++i)
        if (!cdj_c674x_loop_issue(&loop, tags, &count, &post, &drained)) return 2;
    printf("loop %.6f s; cycles=%llu count=%u\n",
           (double)(clock() - start) / CLOCKS_PER_SEC,
           (unsigned long long)loop.cycle, count);

    /* Register-only loop: MVK, ADD, MVKH, CMPEQ, B, NOP 5 (.S1/.L1). */
    static const uint32_t alu[] = {
        1u << 23 | 0x1234u << 7 | 0x28,               /* MVK.S1 0x1234,A1 */
        3u << 23 | 3u << 18 | 1u << 13 | 0x78,        /* ADD.L1 A1,A3,A3 */
        5u << 23 | 0x5678u << 7 | 0x68,               /* MVKH.S1 ...,A5 */
        6u << 23 | 3u << 18 | 1u << 13 | 0xa78,       /* CMPEQ.L1 A1,A3,A6 */
        0x10,                                         /* B.S1 0x1000 */
        4u << 13,                                     /* NOP 5 */
    };
    /* The same with a load and a store in place of MVKH and CMPEQ. */
    static const uint32_t mem[] = {
        1u << 23 | 0x1234u << 7 | 0x28,
        3u << 23 | 3u << 18 | 1u << 13 | 0x78,
        4u << 23 | 10u << 18 | 1u << 9 | 6u << 4 | 4, /* LDW *+A10[0],A4 */
        3u << 23 | 10u << 18 | 1u << 13 | 1u << 9 | 7u << 4 | 4, /* STW */
        0x10,
        4u << 13,
    };
    uint64_t packets;
    double t = step_loop(alu, 6, 30000000, &packets);
    printf("step-alu %.6f s; packets=%llu (%.1f M packets/s)\n", t,
           (unsigned long long)packets, packets / t / 1e6);
    t = step_loop(mem, 6, 30000000, &packets);
    printf("step-mem %.6f s; packets=%llu (%.1f M packets/s)\n", t,
           (unsigned long long)packets, packets / t / 1e6);
    return 0;
}
