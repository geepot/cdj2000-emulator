/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cdj_c674x.h"

static int32_t sx(uint32_t value, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)((value ^ sign) - sign);
}

void cdj_c674x_reset(CdjC674x *cpu, uint32_t entry)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->pc = entry;
}

static bool stop(CdjC674x *cpu, uint32_t pc, uint32_t word, const char *why)
{
    cpu->fault = why;
    cpu->fault_pc = pc;
    cpu->fault_word = word;
    return false;
}

bool cdj_c674x_step(CdjC674x *cpu, CdjC674xRead read, void *opaque)
{
    uint32_t words[8], pcs[8], next = cpu->pc;
    unsigned count = 0, elapsed = 1;
    CdjC674x out = *cpu;
    bool written[2][32] = {{false}}, controls[32] = {false};
    if (cpu->fault) return false;
    /* Validate the entire packet before committing any architectural state. */
    do {
        uint32_t header, word;
        if (next & 3) return stop(cpu, next, 0, "unaligned instruction fetch");
        if (!read(opaque, (next & ~31u) + 28, &header) || !read(opaque, next, &word))
            return stop(cpu, next, 0, "unmapped instruction fetch");
        if ((header >> 28) == 14) return stop(cpu, next, header, "compact fetch packet not implemented");
        if (count == 8) return stop(cpu, next, word, "execute packet exceeds eight instructions");
        pcs[count] = next; words[count++] = word; next += 4;
    } while (words[count - 1] & 1);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t w = words[i], pc = pcs[i], value = 0;
        unsigned side = (w >> 1) & 1, dst = (w >> 23) & 31;
        unsigned a = (w >> 13) & 31, b = (w >> 18) & 31;
        unsigned cross = side ^ ((w >> 12) & 1);
        unsigned creg = w >> 29, z = (w >> 28) & 1;
        bool enabled = true, reg_write = true, control_write = false;
        if (creg == 7 || (!creg && z)) return stop(cpu, pc, w, "reserved predicate");
        if (creg) {
            static const unsigned bank[] = {0,1,1,1,0,0,0};
            static const unsigned index[] = {0,0,1,2,1,2,0};
            enabled = (cpu->r[bank[creg]][index[creg]] != 0) ^ z;
        }
        if ((w & 0xfffe1ffeu) == 0) {
            unsigned n = ((w >> 13) & 15) + 1;
            if (n > 9) return stop(cpu, pc, w, "reserved NOP count");
            if (n > 1 && elapsed > 1) return stop(cpu, pc, w, "multiple multicycle instructions");
            if (n > elapsed) elapsed = n;
            reg_write = false;
        } else if ((w & 0x7c) == 0x28) {
            value = sx((w >> 7) & 0xffff, 16);
        } else if ((w & 0x7c) == 0x68) {
            value = (cpu->r[side][dst] & 0xffff) | (((w >> 7) & 0xffff) << 16);
        } else if ((w & 0x3effc) == 0xa358) {
            value = sx(b, 5); /* MVK .L */
        } else if ((w & 0xffc) == 0x7a0 || (w & 0xffc) == 0xf58 || (w & 0xffc) == 0x9f0) {
            value = (uint32_t)sx(a, 5) & cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x7e0 || (w & 0xffc) == 0xf78 || (w & 0xffc) == 0x9b0) {
            value = cpu->r[side][a] & cpu->r[cross][b];
        } else if ((w & 0xffe) == 0x3a2 && a == 0) {
            /* FADCR/FAUCR/FMCR storage only; FP operations are not decoded yet. */
            if (dst < 18 || dst > 20) return stop(cpu, pc, w, "control register write not implemented");
            control_write = true; reg_write = false; value = cpu->r[cross][b];
        } else if ((w & 0x0f83effe) == 0x362) {
            reg_write = false;
            if (enabled) {
                if (out.branch_due) return stop(cpu, pc, w, "overlapping branches not implemented");
                out.branch_target = cpu->r[((w >> 12) & 1) ^ 1][b];
                out.branch_due = cpu->cycles + 6;
            }
        } else if ((w & 0x1ffe) == 0x162) {
            if (!side) return stop(cpu, pc, w, "ADDKPC requires S2");
            value = (pc & ~31u) + (uint32_t)(sx((w >> 16) & 127, 7) * 4);
            unsigned n = 1 + ((w >> 13) & 7);
            if (enabled && n > 1 && elapsed > 1) return stop(cpu, pc, w, "multiple multicycle instructions");
            if (enabled && n > elapsed) elapsed = n;
        } else {
            return stop(cpu, pc, w, "instruction not implemented");
        }
        if (enabled && reg_write) {
            if (written[side][dst]) return stop(cpu, pc, w, "parallel register write conflict");
            out.r[side][dst] = value; written[side][dst] = true;
        }
        if (enabled && control_write) {
            if (controls[dst]) return stop(cpu, pc, w, "parallel control write conflict");
            out.control[dst] = value; controls[dst] = true;
        }
    }
    out.pc = next;
    for (unsigned i = 0; i < elapsed; ++i) {
        ++out.cycles;
        if (out.branch_due && out.cycles == out.branch_due) {
            out.pc = out.branch_target; out.branch_due = 0; break;
        }
    }
    ++out.packets;
    *cpu = out;
    return true;
}
