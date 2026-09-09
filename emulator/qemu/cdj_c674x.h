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
    /* Low byte: transfer size. High byte: issue-time circular address width
     * (0=linear, 1..32 bits) for nonaligned transfers. */
    unsigned size;
} CdjC674xStore;
typedef struct {
    uint64_t due, value;
    /* Low size byte 1/2/4/8 is a memory load; high byte retains circular
     * address width as in CdjC674xStore. Size 0 is an already-computed delayed
     * scalar result and size 16 an already-computed register-pair result.
     * Sizes 32/33 are delayed IFR set/clear effects; their address field is
     * the interrupt mask and they never write a general register.
     * For size 0, address may be a floating-point status-bit OR mask and
     * sign_extend selects FADCR (false) or FMCR (true).  These sentinels reuse
     * the ABI-stable writeback queue so schema-1 checkpoints retain every
     * in-flight four-cycle result. */
    uint32_t address;
    unsigned bank, dst, size;
    bool sign_extend;
} CdjC674xLoad;
#define CDJ_C674X_DELAYED_IFR_SET 32u
#define CDJ_C674X_DELAYED_IFR_CLEAR 33u
/* No GPR write: address is the SSR unit mask, with CSR.SAT set in parallel. */
#define CDJ_C674X_DELAYED_SAT 34u
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
    /* Optional board clock edge, after advancing the cycle and before E3
     * bus effects. Runs for inserted NOPs too, never for a decode rejection.
     * Must not fail or modify CPU state. Rebind after reset/checkpoint restore.
     * Like bus commits, external effects cannot roll back a broken callback. */
    void (*cycle_tick)(void *);
    void *cycle_opaque;
    /* Delayed control-register availability. ID 31 is not a C674x control
     * register exposed by this core; its otherwise-unused slot preserves the
     * software-loop setup PC, interrupt-drain phase, and compact retained
     * buffer metadata in existing checkpoints without changing the CPU ABI. */
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
/* Present already-selected CPU interrupt requests at an execute-packet
 * boundary. Bits 4..15 correspond to INT4..INT15; all other bits are
 * rejected. Requests latch in IFR even while masked. A recognized interrupt
 * redirects the next fetch to its IST entry without advancing CPU time. */
bool cdj_c674x_interrupt(CdjC674x *cpu, uint32_t pending);
bool cdj_c674x_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque);
#endif
