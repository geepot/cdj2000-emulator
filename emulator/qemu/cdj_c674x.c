/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cdj_c674x.h"

static int32_t sx(uint32_t value, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)((value ^ sign) - sign);
}

/* Side-effect-free RAM reads; nonaligned words may span two bus words. */
static bool read_scalar(CdjC674xRead read, void *opaque, uint32_t address,
                        unsigned size, uint64_t *value)
{
    if ((uint64_t)address + size > UINT64_C(0x100000000)) return false;
    *value = 0;
    for (unsigned done = 0; done < size;) {
        uint32_t word, current = address + done;
        if (!read(opaque, current & ~3u, &word)) return false;
        unsigned lane = current & 3, n = 4 - lane;
        if (n > size - done) n = size - done;
        word >>= lane * 8;
        if (n < 4) word &= (1u << (n * 8)) - 1;
        *value |= (uint64_t)word << (done * 8);
        done += n;
    }
    return true;
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

bool cdj_c674x_fetch(CdjC674x *cpu, CdjC674xRead read, void *opaque,
                     CdjC674xPacket *packet)
{
    CdjC674xPacket result = {0};
    uint32_t next = cpu->pc;
    unsigned count = 0;
    bool parallel = true;
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
        result.instructions[count++] = (CdjC674xInstruction){
            .pc = next, .word = word, .compact = short_word, .header = mixed ? header : 0
        };
        next += short_word ? 2 : 4;
        if (mixed && (next & 31) == 28) next += 4;
    } while (parallel);
    result.count = count;
    result.next_pc = next;
    *packet = result;
    return true;
}

bool cdj_c674x_execute(CdjC674x *cpu, const CdjC674xPacket *packet,
                       CdjC674xRead read, CdjC674xWrite write, void *opaque)
{
    unsigned elapsed = 1, memory_count = 0;
    bool nonaligned_memory = false;
    CdjC674x out = *cpu;
    bool written[2][32] = {{false}}, controls[32] = {false};
    if (cpu->fault) return false;
    if (packet->count > 8) return stop(cpu, cpu->pc, 0, "execute packet exceeds eight instructions");
    for (unsigned i = 0; i < packet->count; ++i) {
        const CdjC674xInstruction *insn = &packet->instructions[i];
        uint32_t w = insn->word, pc = insn->pc, value = 0;
        unsigned side = (w >> 1) & 1, dst = (w >> 23) & 31;
        unsigned a = (w >> 13) & 31, b = (w >> 18) & 31;
        unsigned cross = side ^ ((w >> 12) & 1);
        if (insn->compact) {
            /* Figure F-31: compact MVC to ILC. SPLOOP observes a four-cycle
             * availability latency (section 7.4.3), tracked separately. */
            if ((w & 0xfc7f) == 0xd86f) {
                unsigned src = ((w >> 7) & 7) + ((insn->header & 0x80000) ? 16 : 0);
                if (controls[13]) return stop(cpu, pc, w, "parallel control write conflict");
                out.control[13] = cpu->r[1][src];
                out.control_ready[13] = cpu->cycles + 4;
                controls[13] = true;
                continue;
            }
            /* Figures F-17/18/20/21: compact BNOP uses halfword offsets.
             * Predicate controls the branch, never the inserted NOPs. */
            if ((insn->header & 0x8000) &&
                ((w & 0x3e) == 0x0a || (w & 0x2e) == 0x2a)) {
                bool unsigned_offset = (w & 0xc000) == 0xc000;
                unsigned n = unsigned_offset ? 5 : w >> 13;
                int32_t displacement = unsigned_offset ? (int32_t)((w >> 6) & 255)
                                                       : sx((w >> 6) & 127, 7);
                bool enabled = !(w & 0x20) ||
                    ((cpu->r[w & 1][0] != 0) ^ ((w >> 4) & 1));
                if (n > 0 && elapsed > 1)
                    return stop(cpu, pc, w, "multiple multicycle instructions");
                if (n + 1 > elapsed) elapsed = n + 1;
                if (enabled) {
                    if (out.branch_due) return stop(cpu, pc, w, "overlapping branches not implemented");
                    out.branch_target = (pc & ~31u) + (uint32_t)(displacement * 2);
                    out.branch_due = cpu->cycles + 6;
                }
                continue;
            }
            /* SPRUFE8B Figure C-21: compact stack pushes. Sources and
             * address are sampled in E1; B15 updates now, RAM in E3. */
            if ((w & 0x487f) == 0x0077) {
                ++memory_count;
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
                unsigned rs = (insn->header & (1u << 19)) ? 16 : 0;
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
            if ((w & 0x040e) != 0 || (insn->header & (1u << 14)))
                return stop(cpu, pc, w, "compact instruction not implemented");
            unsigned rs = (insn->header & (1u << 19)) ? 16 : 0;
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
        } else if ((w & 0x10c) == 0x04 || (w & 0x17c) == 0x134 || (w & 0x17c) == 0x154 ||
                   (w & 0x17c) == 0x124 || (w & 0x17c) == 0x174 ||
                   (w & 0x17c) == 0x164 || (w & 0x17c) == 0x144) {
            /* Scalar memory: address E1, RAM access E3, load destination E5. */
            unsigned op = (w >> 4) & 7;
            bool extended = (w & 0x100) != 0;
            bool pair = extended && (op == 2 || op == 4 || op == 6 || op == 7);
            bool nonaligned = extended && op != 4 && op != 6;
            unsigned size = pair ? 8 : nonaligned ? 4 : op >= 6 ? 4 : (op == 0 || op == 4 || op == 5) ? 2 : 1;
            bool is_store = extended ? (op == 4 || op == 5 || op == 7) : (op == 3 || op == 5 || op == 7);
            unsigned scale = size;
            if (pair && nonaligned) {
                scale = (w & (1u << 23)) ? 8 : 1;
                dst &= ~1u;
            } else if (pair && (dst & 1)) {
                return stop(cpu, pc, w, "invalid doubleword register pair");
            }
            unsigned bank = (w >> 7) & 1, mode = (w >> 9) & 15;
            reg_write = false;
            if (!(mode & 8) && (mode & 2))
                return stop(cpu, pc, w, "reserved memory addressing mode");
            if (enabled) {
                ++memory_count;
                nonaligned_memory |= nonaligned;
                if (b >= 4 && b <= 7 && cpu->control[0])
                    return stop(cpu, pc, w, "circular memory addressing not implemented");
                uint32_t offset = ((mode & 4) ? cpu->r[bank][a] : a) * scale;
                uint32_t base = cpu->r[bank][b];
                uint32_t updated = (mode & 1) ? base + offset : base - offset;
                uint32_t address = ((mode & 10) == 10) ? base : updated;
                uint64_t dummy, store_value = cpu->r[side][dst];
                if (pair) store_value |= (uint64_t)cpu->r[side][dst + 1] << 32;
                if ((!nonaligned && (address & (size - 1))) ||
                    (is_store ? (!write || !write(opaque, address, store_value, size, false))
                              : !read_scalar(read, opaque, address, size, &dummy)))
                    return stop(cpu, pc, w, "unaligned or unmapped scalar memory access");
                if (is_store) {
                    if (out.store_count == 24) return stop(cpu, pc, w, "store queue full");
                    out.stores[out.store_count++] = (CdjC674xStore){
                        .due = cpu->cycles + 3, .address = address,
                        .value = store_value, .size = size
                    };
                } else {
                    if (out.load_count == 40) return stop(cpu, pc, w, "load queue full");
                    for (unsigned j = 0; j < out.load_count; ++j)
                        if (out.loads[j].due == cpu->cycles + 5 &&
                            out.loads[j].bank == side &&
                            out.loads[j].dst < dst + (pair ? 2 : 1) &&
                            dst < out.loads[j].dst + (out.loads[j].size == 8 ? 2 : 1))
                            return stop(cpu, pc, w, "parallel load write conflict");
                    out.loads[out.load_count++] = (CdjC674xLoad){
                        .due = cpu->cycles + 5, .address = address, .bank = side, .dst = dst,
                        .size = size, .sign_extend = !extended && (op == 2 || op == 4)
                    };
                }
                if (mode & 8) {
                    if (written[bank][b]) return stop(cpu, pc, w, "parallel register write conflict");
                    out.r[bank][b] = updated; written[bank][b] = true;
                }
            }
            /* PROT inserts four NOPs, including for a false predicate. */
            if (!is_store && (insn->header & (1u << 20))) {
                if (elapsed > 1) return stop(cpu, pc, w, "multiple multicycle instructions");
                elapsed = 5;
            }
        } else if ((w & 0x7c) == 0x28) {
            value = sx((w >> 7) & 0xffff, 16);
        } else if ((w & 0x7c) == 0x68) {
            value = (cpu->r[side][dst] & 0xffff) | (((w >> 7) & 0xffff) << 16);
        } else if ((w & 0x7c1ffc) == 0x40) {
            value = sx(a, 5); /* MVK .D */
        } else if ((w & 0x3effc) == 0xa358) {
            value = sx(b, 5); /* MVK .L */
        } else if ((w & 0xffc) == 0x7a0 || (w & 0xffc) == 0xf58 || (w & 0xffc) == 0x9f0) {
            value = (uint32_t)sx(a, 5) & cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x7e0 || (w & 0xffc) == 0xf78 || (w & 0xffc) == 0x9b0) {
            value = cpu->r[side][a] & cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x58 || (w & 0xffc) == 0x1a0) {
            value = (uint32_t)sx(a, 5) + cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x78 || (w & 0xffc) == 0x1e0) {
            value = cpu->r[side][a] + cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xd8 || (w & 0xffc) == 0x5a0) {
            value = (uint32_t)sx(a, 5) - cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xf8 || (w & 0xffc) == 0x5e0) {
            value = cpu->r[side][a] - cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x8d8) {
            value = sx(a, 5) > (int32_t)cpu->r[cross][b];
        } else if ((w & 0xffc) == 0x8f8) {
            value = (int32_t)cpu->r[side][a] > (int32_t)cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xfd8) {
            value = (uint32_t)sx(a, 5) | cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xff8) {
            value = cpu->r[side][a] | cpu->r[cross][b];
        } else if ((w & 0xfbc) == 0x9a0 || (w & 0xfbc) == 0xda0 ||
                   (w & 0xfbc) == 0xca0) {
            /* Scalar .S shifts; register counts use only six low bits. */
            unsigned n = (w & 0x40) ? (cpu->r[side][a] & 63) : a;
            uint32_t source = cpu->r[cross][b];
            unsigned op = w & 0xfbc;
            if (op == 0xca0) value = n >= 32 ? 0 : source << n;
            else if (op == 0x9a0) value = n >= 32 ? 0 : source >> n;
            else if (n >= 32) value = (source & 0x80000000u) ? UINT32_MAX : 0;
            else {
                value = source >> n;
                if (n && (source & 0x80000000u)) value |= UINT32_MAX << (32 - n);
            }
        } else if ((w & 0xffc) == 0xa58) {
            value = (uint32_t)sx(a, 5) == cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xa78) {
            value = cpu->r[side][a] == cpu->r[cross][b];
        } else if ((w & 0xffe) == 0x3a2 && a == 0) {
            /* FADCR/FAUCR/FMCR storage only; FP operations are not decoded yet. */
            if (dst != 13 && dst != 14 && (dst < 18 || dst > 20)) return stop(cpu, pc, w, "control register write not implemented");
            control_write = true; reg_write = false; value = cpu->r[cross][b];
        } else if ((w & 0x7c) == 0x10) {
            reg_write = false;
            if (enabled) {
                if (out.branch_due) return stop(cpu, pc, w, "overlapping branches not implemented");
                out.branch_target = (pc & ~31u) + (uint32_t)(sx((w >> 7) & 0x1fffff, 21) * 4);
                out.branch_due = cpu->cycles + 6;
            }
        } else if ((w & 0x0f830ffe) == 0x00800362) {
            unsigned n = (w >> 13) & 7;
            reg_write = false;
            if (n && elapsed > 1) return stop(cpu, pc, w, "multiple multicycle instructions");
            if (n + 1 > elapsed) elapsed = n + 1;
            if (enabled) {
                if (out.branch_due) return stop(cpu, pc, w, "overlapping branches not implemented");
                out.branch_target = cpu->r[cross][b];
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
            if (dst == 13 || dst == 14) out.control_ready[dst] = cpu->cycles + 4;
        }
    }
    if (nonaligned_memory && memory_count > 1)
        return stop(cpu, cpu->pc, 0, "parallel access with nonaligned memory instruction");
    /* Same-cycle overlapping RAM reads/writes need bus arbitration that
     * this core does not yet model. Do not choose an invented ordering. */
    for (unsigned j = 0; j < out.load_count; ++j)
        for (unsigned k = 0; k < out.store_count; ++k)
            if (out.loads[j].due - 2 == out.stores[k].due &&
                (uint64_t)out.loads[j].address < (uint64_t)out.stores[k].address + out.stores[k].size &&
                (uint64_t)out.stores[k].address < (uint64_t)out.loads[j].address + out.loads[j].size)
                return stop(cpu, cpu->pc, 0, "simultaneous overlapping RAM accesses not implemented");
    /* Reject E5/E1 register collisions before any RAM transaction commits. */
    for (unsigned j = 0; j < out.load_count; ++j)
        if (out.loads[j].due == cpu->cycles + 1 &&
            (written[out.loads[j].bank][out.loads[j].dst] ||
             (out.loads[j].size == 8 && written[out.loads[j].bank][out.loads[j].dst + 1])))
            return stop(cpu, cpu->pc, 0, "load result write conflict");
    if (packet->single_cycle && elapsed > 1) {
        out.idle_cycles = elapsed - 1;
        elapsed = 1;
    }
    out.pc = packet->next_pc;
    for (unsigned i = 0; i < elapsed; ++i) {
        ++out.cycles;
        for (unsigned j = 0; j < out.store_count;) {
            CdjC674xStore *store = &out.stores[j];
            if (store->due > out.cycles) { ++j; continue; }
            if (!write || !write(opaque, store->address, store->value, store->size, true))
                return stop(cpu, cpu->pc, 0, "RAM store callback broke commit guarantee");
            memmove(store, store + 1, (--out.store_count - j) * sizeof(*store));
        }
        for (unsigned j = 0; j < out.load_count;) {
            CdjC674xLoad *load = &out.loads[j];
            if (load->due == out.cycles + 2) {
                uint64_t data;
                if (!read_scalar(read, opaque, load->address, load->size, &data))
                    return stop(cpu, cpu->pc, 0, "RAM load mapping changed during execution");
                if (load->sign_extend) data = sx(data, load->size * 8);
                load->value = data;
            }
            if (load->due > out.cycles) { ++j; continue; }
            out.r[load->bank][load->dst] = load->value;
            if (load->size == 8) out.r[load->bank][load->dst + 1] = load->value >> 32;
            memmove(load, load + 1, (--out.load_count - j) * sizeof(*load));
        }
        if (out.branch_due && out.cycles == out.branch_due) {
            out.pc = out.branch_target; out.branch_due = 0; out.idle_cycles = 0; break;
        }
    }
    ++out.packets;
    *cpu = out;
    return true;
}

static bool loop_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque)
{
    CdjC674x out = *cpu;
    CdjC674xPacket combined = {.next_pc = cpu->pc, .single_cycle = true};
    bool loading = !out.loop.sealed;
    bool post = out.loop.sealed && out.loop.cycle >= out.loop.post_cycle;
    if (loading && !out.loop_wait) {
        CdjC674xPacket source;
        if (!cdj_c674x_fetch(&out, read, opaque, &source))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        if (++out.loop_packets > 14) return stop(cpu, cpu->pc, 0, "loop buffer packet capacity exceeded");
        bool finish = false;
        unsigned delay = 0, count = 0;
        uint32_t tags[8];
        for (unsigned i = 0; i < source.count; ++i) {
            CdjC674xInstruction insn = source.instructions[i];
            uint32_t w = insn.word;
            if (!insn.compact && (w & 0xf03ffffc) == 0x34000) {
                if (i != 0) return stop(cpu, insn.pc, w, "SPKERNEL must start packet");
                unsigned cbits = 0, stage = 0, field = (w >> 22) & 63;
                while ((1u << cbits) < out.loop.ii) ++cbits;
                for (unsigned j = 5; j >= cbits && j < 6; --j)
                    stage |= ((field >> j) & 1) << (5 - j);
                unsigned cycle = field & ((1u << cbits) - 1);
                if (cycle >= out.loop.ii) return stop(cpu, insn.pc, w, "invalid SPKERNEL cycle");
                delay = stage * out.loop.ii + cycle;
                finish = true;
                continue;
            }
            if (!insn.compact && (w & 0xfffe1ffeu) == 0) {
                unsigned n = ((w >> 13) & 15) + 1;
                if (n > 9 || (n > 1 && (finish || out.loop_wait)))
                    return stop(cpu, insn.pc, w, "invalid loop NOP packet");
                if (n > 1) out.loop_wait = n - 1;
                continue;
            }
            /* This initial integration accepts single-cycle body operations.
             * More loop control forms must not be mistaken for ordinary code. */
            if ((!insn.compact && ((w & 0x1ffe) == 0x162 ||
                  (w & 0x0f830ffe) == 0x00800362 || (w & 0x7c) == 0x10 ||
                  (w & 0x0f83effe) == 0x362)) ||
                (insn.header & (1u << 20)) ||
                (insn.compact && (insn.header & 0x8000)))
                return stop(cpu, insn.pc, w, "loop body control or protected instruction not implemented");
            if (out.loop_tags == 112) return stop(cpu, insn.pc, w, "loop instruction capacity exceeded");
            tags[count++] = out.loop_tags;
            out.loop_instructions[out.loop_tags++] = insn;
        }
        if (!cdj_c674x_loop_load(&out.loop, tags, count, finish, delay))
            return stop(cpu, cpu->pc, 0, "invalid loop buffer load");
        combined.next_pc = source.next_pc;
    } else if (loading) {
        --out.loop_wait;
        if (!cdj_c674x_loop_load(&out.loop, NULL, 0, false, 0))
            return stop(cpu, cpu->pc, 0, "loop dynamic length exceeded");
    }
    uint32_t tags[8]; unsigned count; bool scheduler_post, drained;
    if (!cdj_c674x_loop_issue(&out.loop, tags, &count, &scheduler_post, &drained))
        return stop(cpu, cpu->pc, 0, "loop issue capacity exceeded");
    for (unsigned i = 0; i < count; ++i)
        combined.instructions[combined.count++] = out.loop_instructions[tags[i]];
    if (post && !out.idle_cycles) {
        CdjC674xPacket source;
        if (!cdj_c674x_fetch(&out, read, opaque, &source))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        if (combined.count + source.count > 8) return stop(cpu, cpu->pc, 0, "loop/post packet capacity exceeded");
        for (unsigned i = 0; i < source.count; ++i)
            combined.instructions[combined.count++] = source.instructions[i];
        combined.next_pc = source.next_pc;
    } else if (out.idle_cycles) {
        --out.idle_cycles;
    }
    if (!cdj_c674x_execute(&out, &combined, read, write, opaque))
        return stop(cpu, out.fault_pc, out.fault_word, out.fault);
    uint64_t launched = 1 + out.loop.cycle / out.loop.ii;
    out.control[13] = launched < out.loop.iterations ? out.loop.iterations - launched : 0;
    if (drained && scheduler_post) out.loop_active = false;
    *cpu = out;
    return true;
}

bool cdj_c674x_step(CdjC674x *cpu, CdjC674xRead read, CdjC674xWrite write, void *opaque)
{
    if (cpu->fault) return false;
    if (cpu->loop_active) return loop_step(cpu, read, write, opaque);
    if (cpu->idle_cycles) {
        CdjC674x out = *cpu;
        CdjC674xPacket idle = {.next_pc = cpu->pc, .single_cycle = true};
        --out.idle_cycles;
        if (!cdj_c674x_execute(&out, &idle, read, write, opaque))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        *cpu = out;
        return true;
    }
    CdjC674xPacket packet;
    if (!cdj_c674x_fetch(cpu, read, opaque, &packet)) return false;
    if (!packet.instructions[0].compact && (packet.instructions[0].word & 0x007ffffc) == 0x38000) {
        uint32_t w = packet.instructions[0].word;
        if (w >> 28) return stop(cpu, cpu->pc, w, "nested SPLOOP not implemented");
        if (cpu->cycles < cpu->control_ready[13]) return stop(cpu, cpu->pc, w, "ILC not yet available");
        if (cpu->branch_due) return stop(cpu, cpu->pc, w, "SPLOOP in branch delay not implemented");
        CdjC674x out = *cpu;
        if (!cdj_c674x_loop_init(&out.loop, ((w >> 23) & 31) + 1, cpu->control[13]))
            return stop(cpu, cpu->pc, w, "invalid SPLOOP interval");
        memmove(packet.instructions, packet.instructions + 1, (--packet.count) * sizeof(packet.instructions[0]));
        if (!cdj_c674x_execute(&out, &packet, read, write, opaque))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        out.loop_active = true; out.loop_wait = out.loop_tags = out.loop_packets = 0;
        if (out.control[13]) --out.control[13];
        *cpu = out;
        return true;
    }
    return cdj_c674x_execute(cpu, &packet, read, write, opaque);
}
