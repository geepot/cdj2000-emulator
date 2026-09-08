/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_H
#define CDJ_C674X_H
#include <stdbool.h>
#include <stdint.h>
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
    uint32_t r[2][32], control[32], pc;
    uint64_t cycles, packets, branch_due;
    uint64_t control_ready[32];
    uint32_t branch_target, fault_pc, fault_word;
    const char *fault;
    CdjC674xStore stores[24];
    unsigned store_count;
    CdjC674xLoad loads[40];
    unsigned load_count;
} CdjC674x;
/* Read callbacks currently describe stable, side-effect-free RAM only. */
typedef bool (*CdjC674xRead)(void *, uint32_t, uint32_t *);
/* commit=false checks a RAM write without effects. A successful check must
 * guarantee a later commit succeeds; callbacks must write the whole transfer.
 * Device/MMIO stores require a future bus transaction interface. */
typedef bool (*CdjC674xWrite)(void *, uint32_t, uint64_t, unsigned, bool commit);
typedef struct {
    uint32_t word, pc, header;
    bool compact;
} CdjC674xInstruction;
typedef struct {
    CdjC674xInstruction instructions[8];
    unsigned count;
    uint32_t next_pc;
} CdjC674xPacket;
/* Fetch and execution are separate so loop-buffer instructions retain their
 * original PC/header and share one architectural commit with overlaid code. */
bool cdj_c674x_fetch(CdjC674x *, CdjC674xRead, void *, CdjC674xPacket *);
bool cdj_c674x_execute(CdjC674x *, const CdjC674xPacket *, CdjC674xRead,
                      CdjC674xWrite, void *);
void cdj_c674x_reset(CdjC674x *cpu, uint32_t entry);
bool cdj_c674x_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque);
#endif
