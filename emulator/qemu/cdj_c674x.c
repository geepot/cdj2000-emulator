/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cdj_c674x.h"

static int32_t sx(uint32_t value, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);
    return (int32_t)((value ^ sign) - sign);
}

/* Return the number of cycles inserted by a NOP encoding, or zero when the
 * instruction is not a NOP. The compact H-9 N3 field encodes count - 1;
 * SPRUFE8B labels the operand N3, but TI dis6x and GNU binutils agree on the
 * +1 translation used here. */
static unsigned nop_cycles(const CdjC674xInstruction *insn)
{
    if (insn->compact)
        return (insn->word & 0x1fffu) == 0x0c6eu ? (insn->word >> 13) + 1 : 0;
    if ((insn->word & 0xfffe1ffeu) == 0)
        return ((insn->word >> 13) & 15) + 1;
    return 0;
}

/* SPMASK unit bits are L1,L2,S1,S2,D1,D2,M1,M2. GNU's opcode table
 * corrects the full-width opcode in SPRUFE8B; compact is Figure H-8. */
static bool spmask_decode(const CdjC674xInstruction *insn, unsigned *mask)
{
    uint32_t w = insn->word;
    if (insn->compact && (w & 0x3c7e) == 0x2c66) {
        *mask = (w & 1) | ((w >> 6) & 14) | ((w >> 10) & 48);
        return true;
    }
    if (!insn->compact && (w & 0xfc03fffe) == 0x30000) {
        *mask = (w >> 18) & 255;
        return true;
    }
    return false;
}

/* Format-level unit classification (SPRUFE8B appendices C-G). Zero means
 * unknown: never infer that an unknown operation is safe to mask or replay.
 * This classifies units, not opcode validity; execution still validates ISA. */
static unsigned instruction_unit(const CdjC674xInstruction *insn)
{
    uint32_t w = insn->word;
    unsigned side = insn->compact ? w & 1 : (w >> 1) & 1;
    if (!insn->compact) {
        if ((w & 0x0c) == 12) return 32u; /* Long offsets always use .D2. */
        if ((w & 0x1c) == 0x18) return 1u << side;
        if ((w & 0x0c) == 4 || (w & 0x0c) == 12 ||
            (w & 0x7c) == 0x40 || (w & 0xc3c) == 0x830) return 16u << side;
        if ((w & 0x3c) == 0x20 || (w & 0x3c) == 0x28 ||
            (w & 0x3c) == 8 || (w & 0x7c) == 0x10 ||
            (w & 0x7c) == 0x50 || (w & 0xc3c) == 0xc30) return 4u << side;
        if ((w & 0x7c) == 0 || (w & 0x83c) == 0x30) return 64u << side;
        return 0;
    }
    if (((w & 0x26) == 6 || (w & 0x1c66) == 0x0866 ||
         (w & 0x1c66) == 0x1866) && ((w >> 3) & 3) != 3)
        return 1u << (2 * ((w >> 3) & 3) + side);
    if ((w & 6) == 4 || (w & 0x087f) == 0x0077 ||
        (w & 0x047e) == 0x0036 || (w & 0x047e) == 0x0436 ||
        (w & 0x1c7e) == 0x0c76) return 16u << side;
    if ((w & 0x040e) == 0 || (w & 0x040e) == 0x400 ||
        (w & 0x040e) == 0x408 || (w & 0x047e) == 0x426 ||
        (w & 0x047e) == 0x26) return 1u << side;
    if ((w & 0x001e) == 0x001e) return 64u << side;
    if ((w & 0x040e) == 0xa || (w & 0x040e) == 0x40a ||
        (w & 0x001e) == 0x12 || (w & 0x001e) == 2 ||
        (w & 0x047e) == 0x62 || (w & 0x047e) == 0x462 ||
        (w & 0x047e) == 0x2e || (w & 0x047e) == 0x42e ||
        (w & 0x187e) == 0x6e || (w & 0x1c7e) == 0x186e) return 4u << side;
    return 0;
}

typedef struct { CdjC674x *cpu; unsigned mask; bool unknown; } LoopMask;
static bool compact_branch(const CdjC674xInstruction *i)
{
    unsigned w = i->word;
    return i->compact && ((w & 0x187f) == 0x006f ||
        ((i->header & 0x8000) && ((w & 0x3e) == 0x0a ||
         (w & 0x3e) == 0x1a || (w & 0x2e) == 0x2a)));
}
static bool protected_load(const CdjC674xInstruction *i)
{
    if (!(i->header & (1u << 20))) return false;
    uint32_t w = i->word;
    if (i->compact) return (w & 6) == 4 && (w & 8);
    if ((w & 0x0c) == 12) {
        unsigned op = (w >> 4) & 7;
        return op != 3 && op != 5 && op != 7;
    }
    if ((w & 0x10c) != 4 && (w & 0x17c) != 0x134 &&
        (w & 0x17c) != 0x154 && (w & 0x17c) != 0x124 &&
        (w & 0x17c) != 0x174 && (w & 0x17c) != 0x164 &&
        (w & 0x17c) != 0x144) return false;
    unsigned op = (w >> 4) & 7;
    return op != 5 && op != 7 && op != ((w & 0x100) ? 4u : 3u);
}
static bool loop_allow(void *opaque, uint32_t tag)
{
    LoopMask *context = opaque;
    if (!context->mask) return true;
    unsigned unit = instruction_unit(&context->cpu->loop_instructions[tag]);
    if (!unit) context->unknown = true;
    return !(unit & context->mask);
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

/* One taken branch may enter E1 each cycle; all six pipeline positions can
 * be occupied. Preserve later branches when an older branch redirects fetch. */
static bool queue_branch(CdjC674x *out, uint64_t due, uint32_t target)
{
    if (!out->branch_due) {
        out->branch_due = due; out->branch_target = target;
        return true;
    }
    if (out->branch_due == due || out->branch_count == 5 ||
        (out->branch_count && out->branch_queue[out->branch_count - 1].due == due)) return false;
    out->branch_queue[out->branch_count].due = due;
    out->branch_queue[out->branch_count++].target = target;
    return true;
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
        bool compact = insn->compact;
        unsigned ignored_mask;
        if (spmask_decode(insn, &ignored_mask)) {
            if (i) return stop(cpu, pc, w, "SPMASK must start packet");
            continue; /* Idle loop buffer: SPMASK is a NOP, section 7.15. */
        }
        unsigned nop = nop_cycles(insn);
        if (nop) {
            if (nop > 9) return stop(cpu, pc, insn->word, "reserved NOP count");
            if (nop > 1 && elapsed > 1)
                return stop(cpu, pc, insn->word, "multiple multicycle instructions");
            if (nop > elapsed) elapsed = nop;
            continue;
        }
        /* Figures C-8 through C-15: lower the compact .D memory families to
         * the existing E1/E3/E5 scalar pipeline.  Pointer registers are
         * always A/B4-7 and ignore RS; data and register-offset operands use
         * RS.  Dinc is scaled post-increment, Ddec scaled pre-decrement.
         * The nonaligned doubleword offset is unscaled for Doff/Dind but
         * scaled for Dinc/Ddec, as called out by Figures C-9/11/13/15. */
        bool compact_doff = compact && (w & 0x0406) == 0x0004;
        bool compact_dind = compact && (w & 0x0c06) == 0x0404;
        bool compact_dinc = compact && (w & 0xcc06) == 0x0c04;
        bool compact_ddec = compact && (w & 0xcc06) == 0x4c04;
        if (compact_doff || compact_dind || compact_dinc || compact_ddec) {
            unsigned dsz = (insn->header >> 16) & 7;
            bool load = (w & 8) != 0, secondary = (w & 0x200) != 0;
            unsigned reg = ((w >> 4) & 7) + ((insn->header & 0x80000) ? 16 : 0);
            unsigned op, extended = 0;
            bool nonaligned_pair = false;
            if (!secondary && (dsz & 4)) {
                bool nonaligned = (w & 0x10) != 0;
                reg &= ~1u;
                op = load ? (nonaligned ? 2 : 6) : (nonaligned ? 7 : 4);
                extended = 0x100;
                nonaligned_pair = nonaligned;
            } else if (!secondary) op = load ? 6 : 7;
            else {
                static const unsigned loads[8] = {1, 2, 0, 4, 6, 2, 3, 4};
                static const unsigned stores[8] = {3, 3, 5, 5, 7, 3, 5, 5};
                op = load ? loads[dsz] : stores[dsz];
                if (dsz == 6) extended = 0x100;
            }
            unsigned offset, mode;
            if (compact_doff) {
                offset = ((w >> 13) & 7) | (((w >> 11) & 1) << 3);
                mode = 1;              /* positive constant offset */
            } else if (compact_dind) {
                offset = ((w >> 13) & 7) +
                         ((insn->header & 0x80000) ? 16 : 0);
                mode = 5;              /* positive register offset */
            } else {
                offset = ((w >> 13) & 1) + 1;
                mode = compact_dinc ? 11 : 8; /* postincrement / predecrement */
            }
            unsigned scaled_nonaligned = nonaligned_pair &&
                (compact_dinc || compact_ddec) ? 1u << 23 : 0;
            w = reg << 23 | (4 + ((w >> 7) & 3)) << 18 | offset << 13 |
                mode << 9 | scaled_nonaligned | extended | (w & 1) << 7 |
                op << 4 | 4 | ((w >> 12) & 1) << 1;
            compact = false;
        }
        unsigned side = (w >> 1) & 1, dst = (w >> 23) & 31;
        unsigned a = (w >> 13) & 31, b = (w >> 18) & 31;
        unsigned cross = side ^ ((w >> 12) & 1);
        bool callp = (!compact && (w & 0xf000007c) == 0x10000010) ||
                     (compact && (insn->header & 0x8000) && (w & 0x3e) == 0x1a);
        if (callp) {
            side = compact ? w & 1 : (w >> 1) & 1;
            /* CALLP retains a word-scaled PC-relative displacement in its
             * compact Scs10 form (SPRUFE8B CALLP description / Figure F-19).
             * Compact BNOP is the branch family that uses halfword scaling. */
            int32_t offset = compact ? sx(w >> 6, 10) * 4
                                          : sx((w >> 7) & 0x1fffff, 21) * 4;
            if (out.branch_due || elapsed > 1)
                return stop(cpu, pc, insn->word, "CALLP with pending branch or multicycle instruction");
            for (unsigned j = 0; j < packet->count; ++j) {
                if (j == i) continue;
                CdjC674xInstruction other = packet->instructions[j];
                if ((!other.compact && ((other.word & 0x7c) == 0x10 ||
                     (other.word & 0x1ffe) == 0x162 || (other.word & 0xffe) == 0x362 ||
                     (other.word & 0x1ffc) == 0x120)) ||
                    compact_branch(&other))
                    return stop(cpu, pc, insn->word, "CALLP with parallel control instruction");
            }
            if (written[side][3]) return stop(cpu, pc, insn->word, "parallel register write conflict");
            out.r[side][3] = packet->next_pc; written[side][3] = true;
            if (!queue_branch(&out, cpu->cycles + 6, (pc & ~31u) + (uint32_t)offset))
                return stop(cpu, pc, insn->word, "CALLP branch queue conflict");
            elapsed = 6;
            continue;
        }
        if (compact) {
            unsigned rs = (insn->header & 0x80000) ? 16 : 0;
            bool simple = true;
            side = w & 1;
            cross = side ^ ((w >> 12) & 1);
            if ((w & 0x1c66) == 0x0866 && ((w >> 3) & 3) != 3) {
                /* Figure G-3: [A0/!A0/B0/!B0] MVK 0/1 on L/S/D. */
                unsigned cc = w >> 14;
                if (!((cpu->r[cc >> 1][0] != 0) ^ (cc & 1))) continue;
                dst = ((w >> 7) & 7) + rs;
                value = (w >> 13) & 1;
            } else if ((w & 0x040e) == 0x0400) { /* Figure D-5, ADD.L immediate */
                dst = ((w >> 4) & 7) + rs;
                unsigned imm = (w >> 13) & 7;
                int32_t offset = (w & 0x800) ? (int32_t)imm - 8 : (imm ? (int32_t)imm : 8);
                value = cpu->r[cross][((w >> 7) & 7) + rs] + (uint32_t)offset;
            } else if ((w & 0x041e) == 2) { /* Figures F-27/F-28 */
                unsigned op = (w >> 5) & 3;
                unsigned src = ((w >> 7) & 7) + rs;
                uint32_t source = cpu->r[side][src];
                if (op == 3) {
                    unsigned kind = (w >> 11) & 3;
                    unsigned bits = (kind & 1) ? 8 : 16;
                    dst = ((w >> 13) & 7) + rs;
                    value = source & ((1u << bits) - 1);
                    if (!(kind & 2)) value = sx(value, bits);
                } else {
                    unsigned n = ((w >> 13) & 7) | (((w >> 11) & 3) << 3);
                    dst = op ? src : 0; /* EXTU always writes A0/B0, even RS=1. */
                    value = op == 0 ? (source >> (31 - n)) & 1 :
                            op == 1 ? source | (1u << n) : source & ~(1u << n);
                }
            } else if ((w & 0x001e) == 0x0012) { /* Figure F-24, unsigned MVK.S */
                dst = ((w >> 7) & 7) + rs;
                value = ((w >> 13) & 7) | (((w >> 11) & 3) << 3) |
                        (((w >> 5) & 3) << 5) | (((w >> 10) & 1) << 7);
            } else if ((w & 0x047e) == 0x0426) { /* Figure D-8, MVK.L */
                dst = ((w >> 7) & 7) + rs;
                value = sx(((w >> 13) & 7) | (((w >> 11) & 3) << 3), 5);
            } else if ((w & 0x147e) == 0x0026) { /* Figure D-9, CMPEQ immediate */
                dst = (w >> 11) & 1;
                value = ((w >> 13) & 7) == cpu->r[side][((w >> 7) & 7) + rs];
            } else if ((w & 0x040e) == 0x0408) { /* Figure D-7, L2c */
                dst = (w >> 4) & 1;
                uint32_t left = cpu->r[side][((w >> 13) & 7) + rs];
                uint32_t right = cpu->r[cross][((w >> 7) & 7) + rs];
                unsigned op = ((w >> 9) & 4) | ((w >> 5) & 3);
                switch (op) {
                case 0: value = left & right; break;
                case 1: value = left | right; break;
                case 2: value = left ^ right; break;
                case 3: value = left == right; break;
                case 4: value = (int32_t)left < (int32_t)right; break;
                case 5: value = (int32_t)left > (int32_t)right; break;
                case 6: value = left < right; break;
                default: value = left > right; break;
                }
            } else simple = false;
            if (simple) {
                if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                out.r[side][dst] = value; written[side][dst] = true;
                continue;
            }
            if ((w & 0x187f) == 0x006f) { /* Figure F-32, register BNOP */
                unsigned n = w >> 13;
                if (n && elapsed > 1) return stop(cpu, pc, insn->word, "multiple multicycle instructions");
                if (n + 1 > elapsed) elapsed = n + 1;
                if (!queue_branch(&out, cpu->cycles + 6, cpu->r[1][(w >> 7) & 15]))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
                continue;
            }
            /* Figure F-31: compact MVC to ILC. SPLOOP observes a four-cycle
             * availability latency (section 7.4.3), tracked separately. */
            if ((w & 0xfc7f) == 0xd86f) {
                unsigned src = ((w >> 7) & 7) + ((insn->header & 0x80000) ? 16 : 0);
                if (controls[13]) return stop(cpu, pc, insn->word, "parallel control write conflict");
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
                    return stop(cpu, pc, insn->word, "multiple multicycle instructions");
                if (n + 1 > elapsed) elapsed = n + 1;
                if (enabled) {
                    if (!queue_branch(&out, cpu->cycles + 6, (pc & ~31u) + (uint32_t)(displacement * 2)))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
                }
                continue;
            }
            /* SPRUFE8B Figure C-16: compact word transfer at a positive
             * constant offset from B15. Unlike Dpp, this form does not
             * update B15 and its three-bit register observes header RS. */
            if ((w & 0x8c07) == 0x8c05) {
                ++memory_count;
                bool load = (w & 8) != 0;
                unsigned bank = (w >> 12) & 1;
                unsigned reg = ((w >> 4) & 7) + rs;
                unsigned offset = ((w >> 13) & 3) | (((w >> 7) & 7) << 2);
                uint32_t address = cpu->r[1][15] + offset * 4;
                uint64_t dummy;
                if ((address & 3) ||
                    (load ? !read_scalar(read, opaque, address, 4, &dummy) :
                            (!write || !write(opaque, address,
                                              cpu->r[bank][reg], 4, false))))
                    return stop(cpu, pc, insn->word, "unmapped B15 stack transfer");
                if (load) {
                    if (out.load_count == 40)
                        return stop(cpu, pc, insn->word, "load queue full");
                    for (unsigned j = 0; j < out.load_count; ++j)
                        if (out.loads[j].due == cpu->cycles + 5 &&
                            out.loads[j].bank == bank && out.loads[j].dst == reg)
                            return stop(cpu, pc, insn->word,
                                        "parallel load write conflict");
                    out.loads[out.load_count++] = (CdjC674xLoad){
                        .due = cpu->cycles + 5, .address = address,
                        .bank = bank, .dst = reg, .size = 4
                    };
                } else {
                    if (out.store_count == 24)
                        return stop(cpu, pc, insn->word, "store queue full");
                    out.stores[out.store_count++] = (CdjC674xStore){
                        .due = cpu->cycles + 3, .value = cpu->r[bank][reg],
                        .address = address, .size = 4
                    };
                }
                continue;
            }
            /* SPRUFE8B Figure C-21: compact B15 stack forms. Stores use
             * post-decrement and commit in E3; loads use pre-increment and
             * write their destination in E5. Dpp ignores header RS. */
            if ((w & 0x087f) == 0x0077) {
                ++memory_count;
                unsigned bank = (w >> 12) & 1, reg = (w >> 7) & 15;
                unsigned size = (w & 0x8000) ? 8 : 4;
                bool load = (w & 0x4000) != 0;
                uint32_t old_sp = cpu->r[1][15];
                uint32_t delta = size * (((w >> 13) & 1) + 1);
                uint32_t address = load ? old_sp + delta : old_sp;
                uint64_t data = cpu->r[bank][reg], dummy;
                if ((address & (size - 1)) || (size == 8 && (reg & 1)))
                    return stop(cpu, pc, insn->word,
                                "unaligned stack access or invalid register pair");
                if (size == 8) data |= (uint64_t)cpu->r[bank][reg + 1] << 32;
                if (load ? !read_scalar(read, opaque, address, size, &dummy) :
                           (!write || !write(opaque, address, data, size, false)))
                    return stop(cpu, pc, insn->word, "unmapped stack access");
                if (written[1][15]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                if (load) {
                    if (out.load_count == 40)
                        return stop(cpu, pc, insn->word, "load queue full");
                    for (unsigned j = 0; j < out.load_count; ++j)
                        if (out.loads[j].due == cpu->cycles + 5 &&
                            out.loads[j].bank == bank &&
                            out.loads[j].dst < reg + (size == 8 ? 2 : 1) &&
                            reg < out.loads[j].dst + (out.loads[j].size == 8 ? 2 : 1))
                            return stop(cpu, pc, insn->word, "parallel load write conflict");
                    out.loads[out.load_count++] = (CdjC674xLoad){
                        .due = cpu->cycles + 5, .address = address, .bank = bank,
                        .dst = reg, .size = size
                    };
                } else {
                    if (out.store_count == 24)
                        return stop(cpu, pc, insn->word, "store queue full");
                    out.stores[out.store_count++] = (CdjC674xStore){
                        .due = cpu->cycles + 3, .value = data,
                        .address = address, .size = size
                    };
                }
                out.r[1][15] = load ? address : old_sp - delta;
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
                if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                out.r[side][dst] = cpu->r[cross][b]; written[side][dst] = true;
                continue;
            }
            /* SPRUFE8B Figure D-4: compact .L ADD/SUB. */
            if ((w & 0x040e) != 0 || (insn->header & (1u << 14)))
                return stop(cpu, pc, insn->word, "compact instruction not implemented");
            rs = (insn->header & (1u << 19)) ? 16 : 0;
            side = w & 1;
            dst = ((w >> 4) & 7) + rs;
            a = ((w >> 13) & 7) + rs;
            b = ((w >> 7) & 7) + rs;
            cross = side ^ ((w >> 12) & 1);
            value = (w & 0x0800) ? cpu->r[side][a] - cpu->r[cross][b]
                                  : cpu->r[side][a] + cpu->r[cross][b];
            if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
            out.r[side][dst] = value; written[side][dst] = true;
            continue;
        }
        unsigned creg = w >> 29, z = (w >> 28) & 1;
        bool enabled = true, reg_write = true, control_write = false;
        if (creg == 7 || (!creg && z)) return stop(cpu, pc, insn->word, "reserved predicate");
        if (creg) {
            static const unsigned bank[] = {0,1,1,1,0,0,0};
            static const unsigned index[] = {0,0,1,2,1,2,0};
            enabled = (cpu->r[bank[creg]][index[creg]] != 0) ^ z;
        }
        bool long_offset = (w & 0x0c) == 12;
        if (long_offset || (w & 0x10c) == 0x04 || (w & 0x17c) == 0x134 || (w & 0x17c) == 0x154 ||
                   (w & 0x17c) == 0x124 || (w & 0x17c) == 0x174 ||
                   (w & 0x17c) == 0x164 || (w & 0x17c) == 0x144) {
            /* Scalar memory: address E1, RAM access E3, load destination E5. */
            unsigned op = (w >> 4) & 7;
            bool extended = !long_offset && (w & 0x100) != 0;
            bool pair = extended && (op == 2 || op == 4 || op == 6 || op == 7);
            bool nonaligned = extended && op != 4 && op != 6;
            unsigned size = pair ? 8 : nonaligned ? 4 : op >= 6 ? 4 : (op == 0 || op == 4 || op == 5) ? 2 : 1;
            bool is_store = extended ? (op == 4 || op == 5 || op == 7) : (op == 3 || op == 5 || op == 7);
            unsigned scale = size;
            if (pair && nonaligned) {
                scale = (w & (1u << 23)) ? 8 : 1;
                dst &= ~1u;
            } else if (pair && (dst & 1)) {
                return stop(cpu, pc, insn->word, "invalid doubleword register pair");
            }
            unsigned bank = (w >> 7) & 1, mode = (w >> 9) & 15;
            if (long_offset) {
                /* Figure C-5 / 3.9.3: unsigned scaled 15-bit displacement,
                 * fixed B14/B15 base, no base update, data bank from s. */
                b = 14 + bank; bank = 1; mode = 1;
            }
            reg_write = false;
            if (!(mode & 8) && (mode & 2))
                return stop(cpu, pc, insn->word, "reserved memory addressing mode");
            if (enabled) {
                ++memory_count;
                nonaligned_memory |= nonaligned;
                if (b >= 4 && b <= 7 && cpu->control[0])
                    return stop(cpu, pc, insn->word, "circular memory addressing not implemented");
                uint32_t offset = (long_offset ? (w >> 8) & 32767 :
                                   (mode & 4) ? cpu->r[bank][a] : a) * scale;
                uint32_t base = cpu->r[bank][b];
                uint32_t updated = (mode & 1) ? base + offset : base - offset;
                uint32_t address = ((mode & 10) == 10) ? base : updated;
                uint64_t dummy, store_value = cpu->r[side][dst];
                if (pair) store_value |= (uint64_t)cpu->r[side][dst + 1] << 32;
                if ((!nonaligned && (address & (size - 1))) ||
                    (is_store ? (!write || !write(opaque, address, store_value, size, false))
                              : !read_scalar(read, opaque, address, size, &dummy)))
                    return stop(cpu, pc, insn->word, "unaligned or unmapped scalar memory access");
                if (is_store) {
                    if (out.store_count == 24) return stop(cpu, pc, insn->word, "store queue full");
                    out.stores[out.store_count++] = (CdjC674xStore){
                        .due = cpu->cycles + 3, .address = address,
                        .value = store_value, .size = size
                    };
                } else {
                    if (out.load_count == 40) return stop(cpu, pc, insn->word, "load queue full");
                    for (unsigned j = 0; j < out.load_count; ++j)
                        if (out.loads[j].due == cpu->cycles + 5 &&
                            out.loads[j].bank == side &&
                            out.loads[j].dst < dst + (pair ? 2 : 1) &&
                            dst < out.loads[j].dst + (out.loads[j].size == 8 ? 2 : 1))
                            return stop(cpu, pc, insn->word, "parallel load write conflict");
                    out.loads[out.load_count++] = (CdjC674xLoad){
                        .due = cpu->cycles + 5, .address = address, .bank = side, .dst = dst,
                        .size = size, .sign_extend = !extended && (op == 2 || op == 4)
                    };
                }
                if (mode & 8) {
                    if (written[bank][b]) return stop(cpu, pc, insn->word, "parallel register write conflict");
                    out.r[bank][b] = updated; written[bank][b] = true;
                }
            }
            /* PROT inserts four NOPs, including for a false predicate. */
            if (!is_store && (insn->header & (1u << 20))) {
                if (elapsed > 1) return stop(cpu, pc, insn->word, "multiple multicycle instructions");
                elapsed = 5;
            }
        } else if ((w & 0x7c) == 0x28) {
            value = sx((w >> 7) & 0xffff, 16);
        } else if ((w & 0x7c) == 0x68) {
            value = (cpu->r[side][dst] & 0xffff) | (((w >> 7) & 0xffff) << 16);
        } else if ((w & 0x7c) == 0x40 && ((w >> 7) & 63) >= 0x30 &&
                   ((w >> 7) & 63) <= 0x3d) {
            /* ADDAB/H/W and SUBAB/H/W: same-bank operands, unsigned
             * five-bit immediate or register offset, scaled by 1/2/4. */
            unsigned op = (w >> 7) & 63;
            if (enabled && b >= 4 && b <= 7 && cpu->control[0])
                return stop(cpu, pc, insn->word, "circular address arithmetic not implemented");
            /* ADDAD uses op 3c/3d; there is no SUBAD (TI page 117). */
            uint32_t offset = (op >= 0x3c ? op & 1 : op & 2) ? a : cpu->r[side][a];
            offset <<= (op - 0x30) / 4;
            value = (op < 0x3c && (op & 1)) ? cpu->r[side][b] - offset : cpu->r[side][b] + offset;
        } else if ((w & 0x7c) == 0x40 && ((w >> 7) & 63) >= 0x10 &&
                   ((w >> 7) & 63) <= 0x13) {
            /* ADD/SUB .D without a cross path, SPRUFE8B pp110,529.
             * The assembler operand order is src2,src1 because the D-unit
             * hardware subtracts the src1 field from the src2 field. */
            unsigned op = (w >> 7) & 63;
            uint32_t left = cpu->r[side][b];
            uint32_t right = (op & 2) ? a : cpu->r[side][a];
            value = (op & 1) ? left - right : left + right;
        } else if ((w & 0xffc) == 0xab0 || (w & 0xffc) == 0xaf0 ||
                   (w & 0xffc) == 0xb30) {
            /* Cross-path ADD/SUB .D, including ADD's signed constant form.
             * Cross SUB uses conventional src1-src2 ordering (SPRUFE8B
             * pp110-111,529-530), unlike non-cross D-unit SUB above. */
            uint32_t left = (w & 0xffc) == 0xaf0 ? (uint32_t)sx(a, 5)
                                                 : cpu->r[side][a];
            uint32_t right = cpu->r[cross][b];
            value = (w & 0xffc) == 0xb30 ? left - right : left + right;
        } else if ((w & 0x7c1ffc) == 0x40) {
            value = sx(a, 5); /* MVK .D */
        } else if ((w & 0x3effc) == 0xa358) {
            value = sx(b, 5); /* MVK .L */
        } else if ((w & 0xffc) == 0x018 || (w & 0xffc) == 0xff0 ||
                   (w & 0xffc) == 0x3d8 || (w & 0xffc) == 0x260 ||
                   (w & 0xffc) == 0x398 || (w & 0xffc) == 0x220 ||
                   (w & 0xffc) == 0x378 || (w & 0xffc) == 0x420 ||
                   (w & 0xffc) == 0xd18 || (w & 0xffc) == 0xd38) {
            /* PACK2/PACKH2/PACKHL2/PACKLH2 (.L/.S) and PACKL4/PACKH4
             * (.L), SPRUFE8B.
             * src1 supplies the upper result half and src2 the lower. */
            uint32_t left = cpu->r[side][a], right = cpu->r[cross][b];
            switch (w & 0xffc) {
            case 0x018: case 0xff0: /* PACK2 */
                value = left << 16 | (right & 0xffff); break;
            case 0x3d8: case 0x260: /* PACKH2 */
                value = (left & 0xffff0000) | (right >> 16); break;
            case 0x398: case 0x220: /* PACKHL2 */
                value = (left & 0xffff0000) | (right & 0xffff); break;
            case 0x378: case 0x420: /* PACKLH2 */
                value = left << 16 | (right >> 16); break;
            case 0xd18:             /* PACKL4 */
                value = (left & 0x00ff0000) << 8 | (left & 0xff) << 16 |
                        (right & 0x00ff0000) >> 8 | (right & 0xff); break;
            default:                /* PACKH4 */
                value = (left & 0xff000000) | (left & 0x0000ff00) << 8 |
                        (right & 0xff000000) >> 16 |
                        (right & 0x0000ff00) >> 8; break;
            }
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
        } else if ((w & 0xffc) == 0xa58 || (w & 0xffc) == 0xa78 ||
                   (w & 0xffc) == 0x8d8 || (w & 0xffc) == 0x8f8 ||
                   (w & 0xffc) == 0x9d8 || (w & 0xffc) == 0x9f8 ||
                   (w & 0xffc) == 0xad8 || (w & 0xffc) == 0xaf8 ||
                   (w & 0xffc) == 0xbd8 || (w & 0xffc) == 0xbf8) {
            /* CMPEQ/CMPGT/CMPGTU/CMPLT/CMPLTU scalar .L forms.  Even
             * opfields use a signed or unsigned five-bit constant; odd
             * opfields read src1 from the local register file. */
            unsigned encoding = w & 0xffc;
            bool immediate = !(encoding & 0x20);
            uint32_t left = immediate ? a : cpu->r[side][a];
            uint32_t right = cpu->r[cross][b];
            switch (encoding & ~0x20u) {
            case 0xa58: value = (immediate ? (uint32_t)sx(a, 5) : left) == right; break;
            case 0x8d8: value = (immediate ? sx(a, 5) : (int32_t)left) >
                                (int32_t)right; break;
            case 0x9d8: value = left > right; break;
            case 0xad8: value = (immediate ? sx(a, 5) : (int32_t)left) <
                                (int32_t)right; break;
            default:    value = left < right; break;
            }
        } else if ((w & 0xffc) == 0xfd8 || (w & 0xffc) == 0x6a0 || (w & 0xffc) == 0x8f0) {
            value = (uint32_t)sx(a, 5) | cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xff8 || (w & 0xffc) == 0x6e0 || (w & 0xffc) == 0x8b0) {
            value = cpu->r[side][a] | cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xdd8 || (w & 0xffc) == 0x2a0 || (w & 0xffc) == 0xbf0) {
            value = (uint32_t)sx(a, 5) ^ cpu->r[cross][b];
        } else if ((w & 0xffc) == 0xdf8 || (w & 0xffc) == 0x2e0 || (w & 0xffc) == 0xbb0) {
            value = cpu->r[side][a] ^ cpu->r[cross][b];
        } else if ((w & 0x3c) == 8 || (w & 0xafc) == 0xae0) {
            /* SPRUFE8B CLR/SET/EXT/EXTU: constant .S field format or
             * register counts packed as src1[9:5],src1[4:0]. EXT counts
             * describe left/right shifts, NOT low/high bit indices. */
            bool immediate = (w & 0x3c) == 8;
            unsigned op = immediate ? (w >> 6) & 3 : ((w >> 9) & 2) | ((w >> 8) & 1);
            uint32_t fields = cpu->r[side][a];
            if (!immediate && enabled && (fields & ~1023u))
                return stop(cpu, pc, insn->word, "invalid bit-field register counts");
            unsigned left = immediate ? a : (fields >> 5) & 31;
            unsigned right = immediate ? (w >> 8) & 31 : fields & 31;
            uint32_t source = cpu->r[immediate ? side : cross][b];
            if (op < 2) {
                uint32_t shifted = source << left;
                value = shifted >> right;
                if (op == 1 && right && (shifted & 0x80000000u))
                    value |= UINT32_MAX << (32 - right);
            } else {
                uint32_t mask = right < left ? 0 :
                    (UINT32_MAX << left) & (UINT32_MAX >> (31 - right));
                value = op == 2 ? source | mask : source & ~mask;
            }
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
        } else if ((w & 0xffe) == 0x3a2 && a == 0) {
            /* FADCR/FAUCR/FMCR storage only; FP operations are not decoded yet. */
            if (dst != 13 && dst != 14 && (dst < 18 || dst > 20)) return stop(cpu, pc, insn->word, "control register write not implemented");
            control_write = true; reg_write = false; value = cpu->r[cross][b];
        } else if ((w & 0x7c) == 0x10) {
            reg_write = false;
            if (enabled) {
                if (!queue_branch(&out, cpu->cycles + 6, (pc & ~31u) + (uint32_t)(sx((w >> 7) & 0x1fffff, 21) * 4)))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
            }
        } else if ((w & 0x1ffc) == 0x120) {
            /* SPRUFE8B BNOP displacement, pp165-167: NOPs are unconditional.
             * A 32-bit BNOP in a header-based fetch packet uses halfword
             * displacement units; without a header it uses words. */
            unsigned n = (w >> 13) & 7;
            reg_write = false;
            if (n && elapsed > 1) return stop(cpu, pc, insn->word, "multiple multicycle instructions");
            if (n + 1 > elapsed) elapsed = n + 1;
            if (enabled && !queue_branch(&out, cpu->cycles + 6,
                    (pc & ~31u) + (uint32_t)(sx((w >> 16) & 4095, 12) *
                                           (insn->header ? 2 : 4))))
                return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
        } else if ((w & 0x0f830ffe) == 0x00800362) {
            unsigned n = (w >> 13) & 7;
            reg_write = false;
            if (n && elapsed > 1) return stop(cpu, pc, insn->word, "multiple multicycle instructions");
            if (n + 1 > elapsed) elapsed = n + 1;
            if (enabled) {
                if (!queue_branch(&out, cpu->cycles + 6, cpu->r[cross][b]))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
            }
        } else if ((w & 0x0f83effe) == 0x362) {
            reg_write = false;
            if (enabled) {
                if (!queue_branch(&out, cpu->cycles + 6, cpu->r[((w >> 12) & 1) ^ 1][b]))
                    return stop(cpu, pc, insn->word, "parallel taken branches or branch queue overflow");
            }
        } else if ((w & 0x1ffe) == 0x162) {
            if (!side) return stop(cpu, pc, insn->word, "ADDKPC requires S2");
            value = (pc & ~31u) + (uint32_t)(sx((w >> 16) & 127, 7) * 4);
            unsigned n = 1 + ((w >> 13) & 7);
            if (enabled && n > 1 && elapsed > 1) return stop(cpu, pc, insn->word, "multiple multicycle instructions");
            if (enabled && n > elapsed) elapsed = n;
        } else {
            return stop(cpu, pc, insn->word, "instruction not implemented");
        }
        if (enabled && reg_write) {
            if (written[side][dst]) return stop(cpu, pc, insn->word, "parallel register write conflict");
            out.r[side][dst] = value; written[side][dst] = true;
        }
        if (enabled && control_write) {
            if (controls[dst]) return stop(cpu, pc, insn->word, "parallel control write conflict");
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
        if (out.cycle_tick) out.cycle_tick(out.cycle_opaque);
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
            out.pc = out.branch_target;
            out.loop_active = false; /* SPRUFE8B 7.14: taken branch idles the loop buffer. */
            if (out.branch_count) {
                out.branch_due = out.branch_queue[0].due;
                out.branch_target = out.branch_queue[0].target;
                memmove(out.branch_queue, out.branch_queue + 1,
                        --out.branch_count * sizeof(out.branch_queue[0]));
            } else out.branch_due = 0;
            out.idle_cycles = 0;
            break;
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
    CdjC674xPacket direct = {0};
    LoopMask masking = {.cpu = &out};
    bool loading = !out.loop.sealed;
    bool post = out.loop.sealed && out.loop.cycle >= out.loop.post_cycle;
    if (loading && !out.loop_wait) {
        CdjC674xPacket source;
        if (!cdj_c674x_fetch(&out, read, opaque, &source))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        /* Section 7.7.3.3 indexes storage by LBC (cycle modulo II), not
         * the number of source fetches. NOP/setup packets can exceed 14.
         * The 48-cycle, 112-tag and simultaneous-issue limits still apply. */
        ++out.loop_packets;
        bool finish = false;
        unsigned delay = 0, count = 0;
        uint32_t tags[8];
        bool has_mask = spmask_decode(&source.instructions[0], &masking.mask);
        for (unsigned i = 0; i < source.count; ++i) {
            CdjC674xInstruction insn = source.instructions[i];
            uint32_t w = insn.word;
            unsigned mask;
            if (spmask_decode(&insn, &mask)) {
                if (i) return stop(cpu, insn.pc, w, "SPMASK must start packet");
                continue;
            }
            bool full_kernel = !insn.compact &&
                (w & 0xf03ffffc) == 0x34000;
            bool compact_kernel = insn.compact &&
                (w & 0x3c7e) == 0x1c66;
            if (full_kernel || compact_kernel) {
                if (i != 0) return stop(cpu, insn.pc, w, "SPKERNEL must start packet");
                /* Figure H-7 scatters the same six-bit combined
                 * stage/cycle field across bits 0, 9:7 and 15:14. */
                unsigned field = compact_kernel ?
                    (w & 1) | (((w >> 7) & 7) << 1) |
                    (((w >> 14) & 3) << 4) : (w >> 22) & 63;
                unsigned cbits = 0, stage = 0;
                while ((1u << cbits) < out.loop.ii) ++cbits;
                for (unsigned j = 5; j >= cbits && j < 6; --j)
                    stage |= ((field >> j) & 1) << (5 - j);
                unsigned cycle = field & ((1u << cbits) - 1);
                if (cycle >= out.loop.ii) return stop(cpu, insn.pc, w, "invalid SPKERNEL cycle");
                delay = stage * out.loop.ii + cycle;
                if (out.loop.predicate_loop && delay)
                    return stop(cpu, insn.pc, w, "SPLOOPW requires zero SPKERNEL fetch delay");
                finish = true;
                continue;
            }
            unsigned n = nop_cycles(&insn);
            if (n) {
                if (n > 9 || (n > 1 && (finish || out.loop_wait)))
                    return stop(cpu, insn.pc, w, "invalid loop NOP packet");
                if (n > 1) out.loop_wait = n - 1;
                continue;
            }
            /* SPRUFE8B 3.10 and 7.7.3.3: PROT expands the program stream
             * with four empty loading cycles. Buffered instructions continue
             * issuing during those cycles, just as for explicit NOP 4.
             * Do not reinsert fetch delays when the load is replayed. This
             * expansion applies even when predicated false or SPMASKed. */
            if (protected_load(&insn)) {
                if (finish || out.loop_wait)
                    return stop(cpu, insn.pc, w, "invalid protected loop load packet");
                out.loop_wait = 4;
                insn.header &= ~(1u << 20);
            }
            if ((!insn.compact && ((w & 0x1ffe) == 0x162 || (w & 0x7c) == 0x10 ||
                                  (w & 0xffe) == 0x362 || (w & 0x1ffc) == 0x120)) ||
                compact_branch(&insn))
                return stop(cpu, insn.pc, w, "loop body control instruction not implemented");
            if (has_mask && masking.mask) {
                unsigned unit = instruction_unit(&insn);
                if (!unit) return stop(cpu, insn.pc, w, "SPMASK unit not implemented");
                if (unit & masking.mask) {
                    direct.instructions[direct.count++] = insn;
                    continue;
                }
            }
            /* Section 7.18: MVC may execute from memory when masked but
             * cannot enter the loop buffer. */
            if ((!insn.compact && (w & 0xffe) == 0x3a2) ||
                (insn.compact && (w & 0xfc7f) == 0xd86f))
                return stop(cpu, insn.pc, w, "unmasked loop MVC not permitted");
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
    if (post && !out.idle_cycles) {
        CdjC674xPacket source;
        if (!cdj_c674x_fetch(&out, read, opaque, &source))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        bool has_mask = spmask_decode(&source.instructions[0], &masking.mask);
        for (unsigned i = has_mask ? 1 : 0; i < source.count; ++i) {
            unsigned mask;
            if (spmask_decode(&source.instructions[i], &mask))
                return stop(cpu, source.instructions[i].pc, source.instructions[i].word,
                            "SPMASK must start packet");
            direct.instructions[direct.count++] = source.instructions[i];
        }
        combined.next_pc = source.next_pc;
    } else if (out.idle_cycles) {
        --out.idle_cycles;
    }
    uint32_t tags[8]; unsigned count; bool scheduler_post, drained;
    if (!cdj_c674x_loop_issue_filtered(&out.loop, tags, &count, &scheduler_post,
                                     &drained, loop_allow, &masking))
        return stop(cpu, cpu->pc, 0, "loop issue capacity exceeded");
    if (masking.unknown) return stop(cpu, cpu->pc, 0, "buffered SPMASK unit not implemented");
    if (count + direct.count > 8) return stop(cpu, cpu->pc, 0, "loop/direct packet capacity exceeded");
    for (unsigned i = 0; i < count; ++i)
        combined.instructions[combined.count++] = out.loop_instructions[tags[i]];
    for (unsigned i = 0; i < direct.count; ++i)
        combined.instructions[combined.count++] = direct.instructions[i];
    /* SPRUFE8B 7.10: sample the selected condition each cycle. At the end
     * of a stage, use its value three cycles earlier; the first three loop
     * cycles cannot terminate. No ILC/RILC access and no epilog. */
    bool end_while = out.loop.predicate_loop && out.loop.cycle >= 4 &&
        out.loop.cycle % out.loop.ii == 0 && !(out.loop_pred_history & 4);
    if (end_while && !out.loop.sealed)
        return stop(cpu, cpu->pc, 0, "SPLOOPW termination during loading not implemented");
    bool condition = (cpu->r[out.loop_pred_bank][out.loop_pred_reg] != 0) ^ out.loop_pred_invert;
    out.loop_pred_history = ((out.loop_pred_history << 1) | condition) & 7;
    if (!cdj_c674x_execute(&out, &combined, read, write, opaque))
        return stop(cpu, out.fault_pc, out.fault_word, out.fault);
    uint64_t launched = 1 + out.loop.cycle / out.loop.ii;
    if (!out.loop.predicate_loop)
        out.control[13] = launched < out.loop.iterations ? out.loop.iterations - launched : 0;
    if (end_while) out.loop_active = false;
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
    CdjC674xInstruction first = packet.instructions[0];
    bool compact_sploop = first.compact && (first.word & 0xbc7f) == 0x0c66;
    bool compact_sploopd = first.compact &&
        (((first.word & 0xbc7f) == 0x0c67) ||
         ((first.word & 0xbc7e) == 0x8c66));
    bool while_loop = !first.compact && (first.word & 0x007ffffe) == 0x3e000;
    bool full_sploop = !first.compact &&
        (first.word & 0x007ffffc) == 0x38000;
    if (compact_sploopd)
        return stop(cpu, cpu->pc, first.word,
                    "SPLOOPD delayed testing not implemented");
    if (full_sploop || compact_sploop || while_loop) {
        uint32_t w = first.word;
        unsigned pred = first.compact ? 0 : w >> 29;
        if (while_loop ? (!pred || pred == 7) : (!first.compact && (w >> 28) != 0))
            return stop(cpu, cpu->pc, w, "unsupported loop predicate");
        if (!while_loop && cpu->cycles < cpu->control_ready[13]) return stop(cpu, cpu->pc, w, "ILC not yet available");
        CdjC674x out = *cpu;
        /* SPRUFE8B Figure H-5 scatters compact ii-1 across bits 9:7 and
         * bit 14. GNU binutils format nfu_uspl independently agrees. */
        unsigned ii = first.compact ? (((w >> 7) & 7) | ((w >> 11) & 8)) + 1
                                    : ((w >> 23) & 31) + 1;
        if (!cdj_c674x_loop_init(&out.loop, ii, cpu->control[13]))
            return stop(cpu, cpu->pc, w, "invalid SPLOOP interval");
        out.loop.predicate_loop = while_loop;
        if (while_loop) {
            static const unsigned banks[] = {0,1,1,1,0,0,0};
            static const unsigned regs[] = {0,0,1,2,1,2,0};
            out.loop_pred_bank = banks[pred]; out.loop_pred_reg = regs[pred];
            out.loop_pred_invert = (w >> 28) & 1;
            out.loop_pred_history = 7;
        }
        /* Loop setup cannot share a packet with multicycle operations. */
        for (unsigned j = 1; j < packet.count; ++j) {
            CdjC674xInstruction other = packet.instructions[j];
            uint32_t v = other.word;
            unsigned mask;
            if (spmask_decode(&other, &mask))
                return stop(cpu, other.pc, v, "SPMASK cannot share loop setup packet");
            if (protected_load(&other) ||
                (nop_cycles(&other) > 1) ||
                (!other.compact && ((v & 0xffe) == 0x362 || (v & 0x7c) == 0x10 ||
                                   (v & 0x1ffc) == 0x120)) ||
                compact_branch(&other))
                return stop(cpu, other.pc, v, "multicycle loop setup packet not implemented");
        }
        memmove(packet.instructions, packet.instructions + 1, (--packet.count) * sizeof(packet.instructions[0]));
        if (!cdj_c674x_execute(&out, &packet, read, write, opaque))
            return stop(cpu, out.fault_pc, out.fault_word, out.fault);
        out.loop_active = !(cpu->branch_due && out.cycles >= cpu->branch_due); out.loop_wait = out.loop_tags = out.loop_packets = 0;
        if (!while_loop && out.control[13]) --out.control[13];
        *cpu = out;
        return true;
    }
    return cdj_c674x_execute(cpu, &packet, read, write, opaque);
}
