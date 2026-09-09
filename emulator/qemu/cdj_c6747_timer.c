/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "cdj_c6747_timer.h"

#define TIMER_REVID 0x4472020cu
#define TCR_WRITE_MASK 0x04c03ffeu
#define TGCR_WRITE_MASK 0x0000ff1fu
#define GPINTGPEN_WRITE_MASK 0x00030033u
#define GPDATGPDIR_WRITE_MASK 0x00030003u
#define INTCTL_ENABLE_MASK 0x00050005u
#define INTCTL_STATUS_MASK 0x000a000au

static CdjC6747Timer *decode(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT],
                            uint32_t address, uint32_t *offset)
{
    uint32_t base;
    unsigned index;
    if (address >= CDJ_C6747_TIMER0_BASE && address < CDJ_C6747_TIMER0_BASE + 0x1000) {
        base = CDJ_C6747_TIMER0_BASE;
        index = 0;
    } else if (address >= CDJ_C6747_TIMER1_BASE &&
               address < CDJ_C6747_TIMER1_BASE + 0x1000) {
        base = CDJ_C6747_TIMER1_BASE;
        index = 1;
    } else {
        return NULL;
    }
    *offset = address - base;
    return &timers[index];
}

void cdj_c6747_timers_reset(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT])
{
    memset(timers, 0, sizeof(*timers) * CDJ_C6747_TIMER_COUNT);
}

bool cdj_c6747_timers_read(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT],
                           uint32_t address, uint32_t *value)
{
    uint32_t offset;
    CdjC6747Timer *s = decode(timers, address, &offset);
    if (!s || !value || (offset & 3)) return false;
    switch (offset) {
    case 0x00: *value = TIMER_REVID; break;
    case 0x04: *value = s->emumgt; break;
    case 0x08: *value = s->gpintgpen; break;
    case 0x0c: *value = s->gpdatgpdir; break;
    case 0x10:
        *value = s->tim12;
        if (((s->tgcr >> 2) & 3) == 0) {
            s->tim34_shadow = s->tim34;
            s->tim34_shadow_valid = true;
        } else if ((s->tgcr & 0x14) == 0x14 && (s->tcr & (1u << 10))) {
            s->tim12 = 0;
        }
        break;
    case 0x14:
        *value = s->tim34_shadow_valid ? s->tim34_shadow : s->tim34;
        s->tim34_shadow_valid = false;
        if (((s->tgcr >> 2) & 3) == 1 && (s->tgcr & 0x10) &&
            (s->tcr & (1u << 26)))
            s->tim34 = 0;
        break;
    case 0x18: *value = s->prd12; break;
    case 0x1c: *value = s->prd34; break;
    case 0x20: *value = s->tcr; break;
    case 0x24: *value = s->tgcr; break;
    case 0x28: *value = s->wdtcr; break;
    case 0x34: *value = s->rel12; break;
    case 0x38: *value = s->rel34; break;
    case 0x3c: *value = s->cap12; break;
    case 0x40: *value = s->cap34; break;
    case 0x44: *value = s->intctlstat; break;
    default:
        if (offset < 0x60 || offset > 0x7c) return false;
        *value = s->compare[(offset - 0x60) / 4];
        break;
    }
    return true;
}

bool cdj_c6747_timers_write(CdjC6747Timer timers[CDJ_C6747_TIMER_COUNT],
                            uint32_t address, uint64_t value, unsigned size,
                            bool commit)
{
    uint32_t offset;
    CdjC6747Timer *s = decode(timers, address, &offset);
    if (!s || size != 4 || value > UINT32_MAX || (offset & 3) || offset == 0)
        return false;
    uint32_t word = value;
    switch (offset) {
    case 0x04: if (commit) s->emumgt = word & 3; break;
    case 0x08: if (commit) s->gpintgpen = word & GPINTGPEN_WRITE_MASK; break;
    case 0x0c: if (commit) s->gpdatgpdir = word & GPDATGPDIR_WRITE_MASK; break;
    case 0x10: if (commit) s->tim12 = word; break;
    case 0x14:
        if (commit) { s->tim34 = word; s->tim34_shadow_valid = false; }
        break;
    case 0x18: if (commit) s->prd12 = word; break;
    case 0x1c: if (commit) s->prd34 = word; break;
    case 0x20: if (commit) s->tcr = word & TCR_WRITE_MASK; break;
    case 0x24:
        if (commit) {
            s->tgcr = word & TGCR_WRITE_MASK;
            if (((s->tgcr >> 2) & 3) != 0) s->tim34_shadow_valid = false;
            if (!(s->tgcr & 1)) s->tim12 = 0;
            if (!(s->tgcr & 2)) {
                s->tim34 = 0;
                s->tim34_shadow_valid = false;
            }
        }
        break;
    case 0x28:
        if (commit) {
            uint32_t status = s->wdtcr & (1u << 15);
            if (word & (1u << 15)) status = 0;
            s->wdtcr = (word & 0xffff4000u) | status;
        }
        break;
    case 0x34: if (commit) s->rel12 = word; break;
    case 0x38: if (commit) s->rel34 = word; break;
    case 0x3c: if (commit) s->cap12 = word; break;
    case 0x40: if (commit) s->cap34 = word; break;
    case 0x44:
        if (commit) {
            uint32_t status = s->intctlstat & INTCTL_STATUS_MASK;
            status &= ~(word & INTCTL_STATUS_MASK);
            s->intctlstat = status | (word & INTCTL_ENABLE_MASK);
        }
        break;
    default:
        if (offset < 0x60 || offset > 0x7c) return false;
        if (commit) s->compare[(offset - 0x60) / 4] = word;
        break;
    }
    return true;
}
