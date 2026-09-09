/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_H
#define CDJ_C674X_H
#include <stdbool.h>
#include <stdint.h>
#include "cdj_c674x_loop.h"
/* Partial interpreter. Encodings/semantics: TI SPRUFE8B, instruction entries
 * MVK, MVKH, MVC, AND, B, ADDKPC and NOP; no third-party decoder code. */
typedef struct {
    uint64_t due, value;
    uint32_t address;
    unsigned size;
} CdjC674xStore;
typedef struct {
    uint64_t due, value;
    uint32_t address;
    unsigned bank, dst, size;
    bool sign_extend;
} CdjC674xLoad;
typedef struct {
    uint32_t word, pc, header;
    bool compact;
} CdjC674xInstruction;
typedef struct {
    CdjC674xInstruction instructions[8];
    unsigned count;
    uint32_t next_pc;
    bool single_cycle;
} CdjC674xPacket;
typedef struct {
    uint32_t r[2][32], control[32], pc;
    uint64_t cycles, packets, branch_due;
    uint64_t control_ready[32];
    uint32_t branch_target, fault_pc, fault_word;
    struct { uint64_t due; uint32_t target; } branch_queue[5];
    unsigned branch_count;
    const char *fault;
    CdjC674xStore stores[24];
    unsigned store_count;
    CdjC674xLoad loads[40];
    unsigned load_count;
    bool loop_active;
    unsigned idle_cycles, loop_wait, loop_tags, loop_packets;
    unsigned loop_pred_bank, loop_pred_reg, loop_pred_history;
    bool loop_pred_invert;
    CdjC674xLoop loop;
    CdjC674xInstruction loop_instructions[112];
} CdjC674x;
/* Reads must be side-effect-free and remain mapped between E1 validation and
 * E3 sampling. Read-clear registers require a future bus transaction API. */
typedef bool (*CdjC674xRead)(void *, uint32_t, uint32_t *);
/* commit=false checks a write without effects. A successful check must
 * guarantee a later commit succeeds; callbacks must write the whole transfer.
 * Simple MMIO registers can apply effects at commit; mapping/acceptance must
 * remain stable, even if an earlier in-flight store changes register state. */
typedef bool (*CdjC674xWrite)(void *, uint32_t, uint64_t, unsigned, bool commit);
/* Fetch and execution are separate so loop-buffer instructions retain their
 * original PC/header and share one architectural commit with overlaid code. */
bool cdj_c674x_fetch(CdjC674x *, CdjC674xRead, void *, CdjC674xPacket *);
bool cdj_c674x_execute(CdjC674x *, const CdjC674xPacket *, CdjC674xRead,
                      CdjC674xWrite, void *);
void cdj_c674x_reset(CdjC674x *cpu, uint32_t entry);
bool cdj_c674x_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque);
#endif
