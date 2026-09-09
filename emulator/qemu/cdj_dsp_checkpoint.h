/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_DSP_CHECKPOINT_H
#define CDJ_DSP_CHECKPOINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdj_c674x.h"
#include "cdj_c6747_emifb.h"
#include "cdj_c6747_gpio.h"
#include "cdj_c6747_hpi.h"
#include "cdj_c6747_i2c.h"
#include "cdj_c6747_mcasp.h"
#include "cdj_c6747_pll.h"
#include "cdj_c6747_psc.h"
#include "cdj_c6747_syscfg.h"

#define CDJ_DSP_CHECKPOINT_SCHEMA 2u
#define CDJ_DSP_L2_SIZE 0x40000u
#define CDJ_DSP_SHARED_RAM_SIZE 0x20000u
#define CDJ_DSP_SDRAM_SIZE 0x02000000u
#define CDJ_DSP_COOPERATIVE_BUDGET 1000000u
#define CDJ_DSP_CHECKPOINT_PAGE_SIZE 4096u
#define CDJ_DSP_CHECKPOINT_REASON_SIZE 64u
#define CDJ_DSP_CHECKPOINT_FAULT_SIZE 96u

/* Checkpoints are intentionally ABI-bound: they store the portable interpreter
 * structs verbatim after clearing process-local pointers. The file header
 * records every structure size and native byte order, and readers reject any
 * mismatch. This preserves all pipeline/loop fields without pretending the
 * format is portable across incompatible builds. Source and firmware SHA-256
 * provenance is supplied by the Python run manifest. Schema 2 adds the fixed
 * 128 KiB C6747 shared-RAM image between L2 and sparse EMIFB SDRAM. Schema-1
 * inputs remain readable with shared RAM explicitly restored as zero. */
typedef struct {
    uint32_t hpi_address, boot_phase;
    uint64_t words, event_sequence, checkpoint_sequence;
    uint8_t reset_released, dsp_started, dsp_halted, had_fault;
    char stop_reason[CDJ_DSP_CHECKPOINT_REASON_SIZE];
    char fault[CDJ_DSP_CHECKPOINT_FAULT_SIZE];
    CdjC674x cpu;
    CdjC6747Syscfg syscfg;
    CdjC6747Psc psc;
    CdjC6747Mcasp mcasp;
    CdjC6747Gpio gpio;
    CdjC6747I2c i2c;
    CdjC6747Pll pll;
    CdjC6747Hpi hpi;
    CdjC6747Emifb emifb;
} CdjDspCheckpointState;

void cdj_dsp_checkpoint_prepare(CdjDspCheckpointState *state,
                                const char *reason);
bool cdj_dsp_checkpoint_write(const char *path,
                              const CdjDspCheckpointState *state,
                              const uint8_t *l2, size_t l2_size,
                              const uint8_t *shared_ram, size_t shared_ram_size,
                              const uint8_t *sdram, size_t sdram_size,
                              char *error, size_t error_size);
bool cdj_dsp_checkpoint_read(const char *path,
                             CdjDspCheckpointState *state,
                             uint8_t *l2, size_t l2_size,
                             uint8_t *shared_ram, size_t shared_ram_size,
                             uint8_t *sdram, size_t sdram_size,
                             char *error, size_t error_size);

#endif
