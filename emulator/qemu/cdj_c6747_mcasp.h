/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_MCASP_H
#define CDJ_C6747_MCASP_H
#include <stdbool.h>
#include <stdint.h>
/* SPRUH91D 24.1.3-8: configuration latches only. No physical pin routing,
 * external input, serializer, clock, FIFO, DMA or interrupt emulation yet. */
typedef struct {
    uint32_t pfunc[3], pdir[3], pdout[3];
} CdjC6747Mcasp;
void cdj_c6747_mcasp_reset(CdjC6747Mcasp *s);
bool cdj_c6747_mcasp_read(const CdjC6747Mcasp *s, uint32_t address,
                         uint32_t *value);
bool cdj_c6747_mcasp_write(CdjC6747Mcasp *s, uint32_t address,
                          uint64_t value, unsigned size, bool commit);
#endif
