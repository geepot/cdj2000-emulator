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

bool cdj_c674x_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque)
{
    uint32_t words[8], pcs[8], next = cpu->pc;
    unsigned count = 0, elapsed = 1;
    bool compact[8], parallel = true;
    uint32_t headers[8];
    CdjC674x out = *cpu;
    bool written[2][32] = {{false}}, controls[32] = {false};
    if (cpu->fault) return false;
    /* Validate the entire packet before committing any architectural state. */
    do {
        uint32_t header, word;
        if (next & 1) return stop(cpu, next, 0, "unaligned instruction fetch");
        if (!read(opaque, (next & ~31u) + 28, &header))
            return stop(cpu, next, 0, "unmapped instruction fetch");
        bool mixed = (header >> 28) == 14;
        /* The header occupies no execution slot. */
        if (mixed && (next & 31) == 28) {
            next += 4;
            continue;
        }
        if (mixed && (next & 31) == 30)
            return stop(cpu, next, header, "instruction points into compact header");
        unsigned slot = (next & 31) / 4;
        bool short_word = mixed && ((header >> (21 + slot)) & 1);
        if (!short_word && (next & 3))
            return stop(cpu, next, 0, "unaligned full instruction fetch");
        if (!read(opaque, next & ~3u, &word))
            return stop(cpu, next, 0, "unmapped instruction fetch");
        if (count == 8) return stop(cpu, next, word, "execute packet exceeds eight instructions");
        if (short_word) word = (word >> ((next & 2) * 8)) & 0xffff;
        parallel = short_word ? ((header >> ((next & 31) / 2)) & 1) : (word & 1);
        pcs[count] = next; words[count] = word;
        compact[count] = short_word; headers[count++] = mixed ? header : 0;
        next += short_word ? 2 : 4;
        if (mixed && (next & 31) == 28) next += 4;
    } while (parallel);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t w = words[i], pc = pcs[i], value = 0;
        unsigned side = (w >> 1) & 1, dst = (w >> 23) & 31;
        unsigned a = (w >> 13) & 31, b = (w >> 18) & 31;
        unsigned cross = side ^ ((w >> 12) & 1);
        if (compact[i]) {
            /* SPRUFE8B Figure C-21: compact stack pushes. Sources and
             * address are sampled in E1; B15 updates now, RAM in E3. */
            if ((w & 0x487f) == 0x0077) {
                unsigned bank = (w >> 12) & 1, src = (w >> 7) & 15;
                unsigned size = (w & 0x8000) ? 8 : 4;
                uint32_t address = cpu->r[1][15];
                uint64_t data = cpu->r[bank][src];
                if ((address & (size - 1)) || (size == 8 && (src & 1)))
                    return stop(cpu, pc, w, "unaligned stack store or invalid register pair");
                if (size == 8) data |= (uint64_t)cpu->r[bank][src + 1] << 32;
                if (!write || !write(opaque, address, data, size, false))
                    return stop(cpu, pc, w, "unmapped stack store");
                if (written[1][15]) return stop(cpu, pc, w, "parallel register write conflict");
                if (out.store_count == 24) return stop(cpu, pc, w, "store queue full");
                out.stores[out.store_count++] = (CdjC674xStore){
                    .due = cpu->cycles + 3, .value = data, .address = address, .size = size
                };
                out.r[1][15] = address - size * (((w >> 13) & 1) + 1);
                written[1][15] = true;
                continue;
            }
            /* Figures G-1/G-2: one operand is a full 5-bit register,
             * the other uses the header-selected register subset. */
            if ((w & 0x0026) == 0x0006 && ((w >> 3) & 3) != 3) {
                unsigned rs = (headers[i] & (1u << 19)) ? 16 : 0;
                unsigned ms = ((w >> 10) & 3) << 3;
                side = w & 1;
                cross = side ^ ((w >> 12) & 1);
                dst = (w >> 13) & 7;
                b = (w >> 7) & 7;
                if (w & 0x40) { dst += ms; b += rs; }
                else { dst += rs; b += ms; }
                if (written[side][dst]) return stop(cpu, pc, w, "parallel register write conflict");
                out.r[side][dst] = cpu->r[cross][b]; written[side][dst] = true;
                continue;
            }
            /* SPRUFE8B Figure D-4: compact .L ADD/SUB. */
            if ((w & 0x040e) != 0 || (headers[i] & (1u << 14)))
                return stop(cpu, pc, w, "compact instruction not implemented");
            unsigned rs = (headers[i] & (1u << 19)) ? 16 : 0;
            side = w & 1;
            dst = ((w >> 4) & 7) + rs;
            a = ((w >> 13) & 7) + rs;
            b = ((w >> 7) & 7) + rs;
            cross = side ^ ((w >> 12) & 1);
            value = (w & 0x0800) ? cpu->r[side][a] - cpu->r[cross][b]
                                  : cpu->r[side][a] + cpu->r[cross][b];
            if (written[side][dst]) return stop(cpu, pc, w, "parallel register write conflict");
            out.r[side][dst] = value; written[side][dst] = true;
            continue;
        }
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
        } else if ((w & 0xffc) == 0xa58) {
            value = (uint32_t)sx(a, 5) == cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xa78) {
            value = cpu->r[side][a] == cpu->r[cross][b];
        } else if ((w & 0xffe) == 0x3a2 && a == 0) {
            /* FADCR/FAUCR/FMCR storage only; FP operations are not decoded yet. */
            if (dst < 18 || dst > 20) return stop(cpu, pc, w, "control register write not implemented");
            control_write = true; reg_write = false; value = cpu->r[cross][b];
        } else if ((w & 0x7c) == 0x10) {
            reg_write = false;
            if (enabled) {
                if (out.branch_due) return stop(cpu, pc, w, "overlapping branches not implemented");
                out.branch_target = (pc & ~31u) + (uint32_t)(sx((w >> 7) & 0x1fffff, 21) * 4);
                out.branch_due = cpu->cycles + 6;
            }
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
        for (unsigned j = 0; j < out.store_count;) {
            CdjC674xStore *store = &out.stores[j];
            if (store->due > out.cycles) { ++j; continue; }
            if (!write || !write(opaque, store->address, store->value, store->size, true))
                return stop(cpu, cpu->pc, 0, "RAM store callback broke commit guarantee");
            memmove(store, store + 1, (--out.store_count - j) * sizeof(*store));
        }
        if (out.branch_due && out.cycles == out.branch_due) {
            out.pc = out.branch_target; out.branch_due = 0; break;
        }
    }
    ++out.packets;
    *cpu = out;
    return true;
}
