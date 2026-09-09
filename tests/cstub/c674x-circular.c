/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SPRUFE8B 2.8.3, 3.9.1-3.9.2: AMR controls only A4-7/B4-7 on .D.
 * Block length is bytes, mode selects BK0/BK1, N=31 is a valid 4GiB ring. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cdj_c674x.h"

static uint8_t memory[1024];
static unsigned committed;
static bool read_word(void *opaque, uint32_t address, uint32_t *value)
{
    (void)opaque;
    if ((address & 3) || address < 0x1000 || address > 0x13fc) return false;
    *value = 0;
    for (unsigned i = 0; i < 4; ++i) *value |= (uint32_t)memory[address - 0x1000 + i] << (8 * i);
    return true;
}
static bool write_value(void *opaque, uint32_t address, uint64_t value,
                        unsigned size, bool commit)
{
    (void)opaque;
    if (address < 0x1000 || (uint64_t)address + size > 0x1400) return false;
    if (commit) {
        ++committed;
        for (unsigned i = 0; i < size; ++i) memory[address - 0x1000 + i] = value >> (8 * i);
    }
    return true;
}
static bool execute(CdjC674x *c, uint32_t word)
{
    CdjC674xPacket p = {.count = 1, .next_pc = c->pc + 4,
                       .instructions = {{.word = word, .pc = c->pc}}};
    return cdj_c674x_execute(c, &p, read_word, write_value, NULL);
}
static void issue(CdjC674x *c, uint32_t word)
{
    if (!execute(c, word)) {
        fprintf(stderr, "circular test fault %08x: %s\n", word, c->fault);
        assert(false);
    }
}
static void reset(CdjC674x *c)
{
    cdj_c674x_reset(c, 0x1000); committed = 0;
    for (unsigned i = 0; i < sizeof(memory); ++i) memory[i] = (i * 19 + (i >> 5) * 7) & 255;
}
static uint32_t amr(unsigned bank, unsigned reg, unsigned mode, unsigned bk)
{
    return mode << (bank * 8 + 2 * (reg - 4)) |
           bk << (mode == 2 ? 21 : 16);
}
static uint32_t wrap(uint32_t base, uint32_t updated, unsigned bk)
{
    uint32_t mask = (uint32_t)((UINT64_C(1) << (bk + 1)) - 1);
    return (base & ~mask) | (updated & mask);
}
static uint32_t address_op(unsigned side, unsigned base, unsigned op, unsigned field)
{ return 10u << 23 | base << 18 | field << 13 | op << 7 | 0x40 | side << 1; }

static void arithmetic(void)
{
    const uint32_t bases[] = {0, 0x12345678, 0x7ffffffe, 0x80000000, 0xffffffff};
    const uint32_t offsets[] = {0, 1, 31, 33, 0xffffffff, 0x80000000};
    for (unsigned side = 0; side < 2; ++side)
    for (unsigned reg = 4; reg <= 7; ++reg)
    for (unsigned mode = 1; mode <= 2; ++mode)
    for (unsigned bk = 0; bk < 32; ++bk)
    for (unsigned op = 0x30; op <= 0x3d; ++op)
    for (unsigned j = 0; j < sizeof(bases) / sizeof(bases[0]); ++j)
    for (unsigned k = 0; k < sizeof(offsets) / sizeof(offsets[0]); ++k) {
        CdjC674x c; reset(&c);
        c.control[0] = amr(side, reg, mode, bk);
        c.r[side][reg] = bases[j]; c.r[side][1] = offsets[k];
        /* Opposite bank must not be sampled by address arithmetic. */
        c.r[side ^ 1][reg] = ~bases[j]; c.r[side ^ 1][1] = ~offsets[k];
        bool immediate = op >= 0x3c ? op & 1 : op & 2;
        unsigned field = immediate ? offsets[k] & 31 : 1;
        uint32_t delta = (immediate ? field : offsets[k]) << ((op - 0x30) / 4);
        uint32_t linear = op < 0x3c && (op & 1) ? bases[j] - delta : bases[j] + delta;
        issue(&c, address_op(side, reg, op, field));
        assert(c.r[side][10] == wrap(bases[j], linear, bk));
        assert(c.r[side][reg] == bases[j]);
    }
    /* AMR is keyed by source/base, not destination; noneligible sources and
     * ordinary ADD .D remain linear even while all AMR modes are circular. */
    for (unsigned reg = 0; reg < 32; ++reg) {
        if (reg >= 4 && reg <= 7) continue;
        CdjC674x c; reset(&c); c.control[0] = 0x00005555;
        c.r[0][reg] = 0x111f;
        issue(&c, address_op(0, reg, 0x32, 3));
        assert(c.r[0][10] == 0x1122);
    }
    CdjC674x c; reset(&c); c.control[0] = amr(0, 4, 1, 4);
    c.r[0][4] = 0x111f;
    issue(&c, address_op(0, 4, 0x12, 3));
    assert(c.r[0][10] == 0x1122);
}

static uint32_t memop(unsigned bank, unsigned data_side, unsigned base,
                      unsigned field, unsigned mode, unsigned opcode)
{
    return 10u << 23 | base << 18 | field << 13 | mode << 9 |
           bank << 7 | opcode | data_side << 1;
}

static void aligned_memory(void)
{
    static const struct { unsigned code, size; bool store, sign; } forms[] = {
        {0x04, 2, false, false}, {0x14, 1, false, false}, {0x24, 1, false, true},
        {0x34, 1, true, false}, {0x44, 2, false, true}, {0x54, 2, true, false},
        {0x64, 4, false, false}, {0x74, 4, true, false},
        {0x164, 8, false, false}, {0x144, 8, true, false},
    };
    const unsigned modes[] = {0, 1, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
    for (unsigned bank = 0; bank < 2; ++bank)
    for (unsigned data_side = 0; data_side < 2; ++data_side)
    for (unsigned reg = 4; reg <= 7; ++reg)
    for (unsigned choice = 1; choice <= 2; ++choice)
    for (unsigned f = 0; f < sizeof(forms) / sizeof(forms[0]); ++f)
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
        CdjC674x c; reset(&c);
        unsigned bk = choice == 1 ? 4 : 5, mode = modes[m];
        c.control[0] = amr(bank, reg, choice, bk);
        uint32_t base = mode & 1 ? 0x1118 : 0x1100;
        c.r[bank][reg] = base; c.r[bank][1] = 33;
        c.r[data_side][10] = 0x89abcdef; c.r[data_side][11] = 0x12345678;
        uint32_t offset = (mode & 4 ? 33 : 9) * forms[f].size;
        uint32_t updated = wrap(base, mode & 1 ? base + offset : base - offset, bk);
        uint32_t address = (mode & 10) == 10 ? base : updated;
        uint64_t expected = 0;
        for (unsigned i = 0; i < forms[f].size; ++i)
            expected |= (uint64_t)memory[address - 0x1000 + i] << (8 * i);
        if (forms[f].sign) {
            unsigned bits = forms[f].size * 8;
            expected = (uint32_t)((expected ^ (1u << (bits - 1))) - (1u << (bits - 1)));
        }
        issue(&c, memop(bank, data_side, reg, mode & 4 ? 1 : 9, mode, forms[f].code));
        assert(c.r[bank][reg] == (mode & 8 ? updated : base));
        assert(!committed && c.r[data_side][10] == 0x89abcdef);
        for (unsigned cycle = 2; cycle <= 5; ++cycle) issue(&c, 0);
        if (forms[f].store) {
            uint64_t stored = 0;
            for (unsigned i = 0; i < forms[f].size; ++i)
                stored |= (uint64_t)memory[address - 0x1000 + i] << (8 * i);
            uint64_t mask = forms[f].size == 8 ? UINT64_MAX : (UINT64_C(1) << (8 * forms[f].size)) - 1;
            assert(committed && stored == (UINT64_C(0x1234567889abcdef) & mask));
        } else {
            assert(c.r[data_side][10] == (uint32_t)expected);
            if (forms[f].size == 8) assert(c.r[data_side][11] == expected >> 32);
        }
    }
}

static void mvc_and_reserved(void)
{
    CdjC674x c; reset(&c); assert(!c.control[0]);
    c.r[1][1] = UINT32_MAX;
    issue(&c, 1u << 18 | 0x3a2); /* MVC B1,AMR */
    assert(c.control[0] == 0x03ffffff);
    issue(&c, 2u << 23 | 0x3e2); /* MVC AMR,B2 */
    assert(c.r[1][2] == 0x03ffffff);
    for (unsigned enabled = 0; enabled < 2; ++enabled)
    for (unsigned memory_form = 0; memory_form < 2; ++memory_form) {
        reset(&c); c.control[0] = 3; c.r[0][4] = 0x1100; c.r[1][0] = enabled;
        uint32_t word = (1u << 29) | (memory_form ? memop(0, 0, 4, 1, 9, 0x64) :
                                                        address_op(0, 4, 0x32, 1));
        bool ok = execute(&c, word);
        assert(ok == !enabled);
        assert(c.r[0][4] == 0x1100 && !c.load_count && !c.store_count && !committed);
    }
    /* Changing an unrelated field to reserved mode cannot disable A4. */
    reset(&c); c.control[0] = 3u << 14; c.r[0][4] = 0x111f;
    issue(&c, address_op(0, 4, 0x32, 1)); assert(c.r[0][10] == 0x1120);
    /* Architectural normal execution inserts one AMR-use stall. Until that
     * pipeline interlock exists, adjacent eligible use must fail closed even
     * when the newly selected mode is linear; one NOP makes it safe. */
    for (unsigned nop = 0; nop < 2; ++nop) {
        reset(&c); c.r[1][1] = 0; c.r[0][4] = 0x111f;
        issue(&c, 1u << 18 | 0x3a2);
        if (nop) issue(&c, 0);
        bool ok = execute(&c, address_op(0, 4, 0x32, 1));
        assert(ok == (bool)nop);
        assert(c.r[0][10] == (nop ? 0x1120u : 0));
    }
}

static void nonaligned_memory(void)
{
    for (unsigned bank = 0; bank < 2; ++bank)
    for (unsigned data_side = 0; data_side < 2; ++data_side)
    for (unsigned pair = 0; pair < 2; ++pair)
    for (unsigned store = 0; store < 2; ++store)
    for (unsigned crossing = 0; crossing < 2; ++crossing) {
        CdjC674x c; reset(&c);
        unsigned size = pair ? 8 : 4;
        uint32_t address = crossing ? 0x111f : 0x1103;
        c.control[0] = amr(bank, 4, 1, 4); c.r[bank][4] = address;
        c.r[data_side][10] = 0x89abcdef; c.r[data_side][11] = 0x12345678;
        uint8_t original[sizeof(memory)]; memcpy(original, memory, sizeof(memory));
        unsigned code = pair ? (store ? 0x174 : 0x124) : (store ? 0x154 : 0x134);
        issue(&c, memop(bank, data_side, 4, 0, 1, code));
        assert(!committed && c.r[data_side][10] == 0x89abcdef);
        /* Change AMR after issue via genuine MVC: the pending access must
         * still use its old circle. Address E1, sample/store E3, result E5. */
        c.r[1][1] = 0;
        issue(&c, 1u << 18 | 0x3a2);
        assert(!committed && !c.control[0]);
        uint64_t expected = 0;
        if (!store) {
            for (unsigned i = 0; i < size; ++i) {
                unsigned index = wrap(address, address + i, 4) - 0x1000;
                memory[index] ^= 0x55;
                expected |= (uint64_t)memory[index] << (8 * i);
            }
        }
        issue(&c, 0); /* E3 samples the transfer. */
        assert(c.r[data_side][10] == 0x89abcdef);
        if (!store) memset(memory, 0, sizeof(memory));
        issue(&c, 0); issue(&c, 0);
        if (store) {
            uint8_t wanted[sizeof(memory)]; memcpy(wanted, original, sizeof(memory));
            const uint64_t value = UINT64_C(0x1234567889abcdef);
            for (unsigned i = 0; i < size; ++i)
                wanted[wrap(address, address + i, 4) - 0x1000] = value >> (8 * i);
            assert(committed && !memcmp(wanted, memory, sizeof(memory)));
        } else {
            assert(c.r[data_side][10] == (uint32_t)expected);
            if (pair) assert(c.r[data_side][11] == expected >> 32);
        }
    }
    /* TI3.9.2.3 says nonaligned circular buffers smaller than32 are undefined. */
    for (unsigned bk = 0; bk < 4; ++bk)
    for (unsigned store = 0; store < 2; ++store) {
        CdjC674x c; reset(&c);
        c.control[0] = amr(0, 4, 1, bk); c.r[0][4] = 0x1100;
        assert(!execute(&c, memop(0, 0, 4, 0, 1, store ? 0x154 : 0x134)));
        assert(c.fault && !c.cycles && !c.load_count && !c.store_count && !committed);
    }
    /* LDNDW/STNDW bit23 scales an offset by8, not the destination pair. */
    for (unsigned scaled = 0; scaled < 2; ++scaled)
    for (unsigned store = 0; store < 2; ++store) {
        CdjC674x c; reset(&c);
        uint32_t base = 0x111d, address = wrap(base, base + 9 * (scaled ? 8 : 1), 4);
        c.control[0] = amr(0, 4, 1, 4); c.r[0][4] = base;
        c.r[0][10] = 0x89abcdef; c.r[0][11] = 0x12345678;
        uint64_t expected = 0;
        for (unsigned i = 0; i < 8; ++i)
            expected |= (uint64_t)memory[wrap(address, address + i, 4) - 0x1000] << (8 * i);
        issue(&c, memop(0, 0, 4, 9, 9, store ? 0x174 : 0x124) | scaled << 23);
        assert(c.r[0][4] == address);
        for (unsigned i = 0; i < 4; ++i) issue(&c, 0);
        if (store) {
            uint64_t actual = 0;
            for (unsigned i = 0; i < 8; ++i)
                actual |= (uint64_t)memory[wrap(address, address + i, 4) - 0x1000] << (8 * i);
            assert(actual == UINT64_C(0x1234567889abcdef));
        } else {
            assert(c.r[0][10] == (uint32_t)expected && c.r[0][11] == expected >> 32);
        }
    }
}

static void circular_conflicts(void)
{
    /* Isolate queued-transfer collision validation, as at a restored pipeline
     * boundary. Ordinary packets already reject parallel nonaligned accesses;
     * these explicitly seeded queues exercise wrapped-byte overlap rather
     * than stopping at that earlier packet-format restriction. */
    for (unsigned wrapped_store = 0; wrapped_store < 2; ++wrapped_store)
    for (unsigned overlap = 0; overlap < 2; ++overlap) {
        CdjC674x c; reset(&c);
        uint8_t initial[sizeof(memory)]; memcpy(initial, memory, sizeof(memory));
        uint32_t linear = overlap ? 0x1100 : 0x1108;
        c.load_count = c.store_count = 1;
        c.loads[0] = (CdjC674xLoad){
            .due = 3, .address = wrapped_store ? linear : 0x111e,
            .size = wrapped_store ? 2 : 4 | (5u << 8), .bank = 0, .dst = 10,
        };
        c.stores[0] = (CdjC674xStore){
            .due = 1, .address = wrapped_store ? 0x111e : linear,
            .size = wrapped_store ? 4 | (5u << 8) : 2, .value = 0x1234abcd,
        };
        assert(execute(&c, 0) == !overlap);
        if (overlap) {
            assert(c.fault && strstr(c.fault, "overlapping RAM"));
            assert(!c.cycles && !c.packets && !committed);
            assert(c.load_count == 1 && c.store_count == 1);
            assert(!memcmp(initial, memory, sizeof(memory)));
        } else {
            assert(c.cycles == 1 && committed && c.store_count == 0);
            uint64_t expected = 0;
            for (unsigned i = 0; i < (wrapped_store ? 2u : 4u); ++i) {
                unsigned address = wrapped_store ? linear + i : wrap(0x111e, 0x111e + i, 4);
                expected |= (uint64_t)initial[address - 0x1000] << (8 * i);
            }
            issue(&c, 0); issue(&c, 0);
            assert(c.r[0][10] == expected);
        }
    }
    /* The high-byte circular-width tag must not hide a pending pair's second
     * destination from E5/E1 collision checks. Use a genuinely issued LDNDW. */
    CdjC674x c; reset(&c);
    c.control[0] = amr(0, 4, 1, 4); c.r[0][4] = 0x111f;
    c.r[0][10] = 0x11223344; c.r[0][11] = 0x55667788;
    issue(&c, memop(0, 0, 4, 0, 1, 0x124));
    assert(c.load_count == 1 && (c.loads[0].size & 255) == 8);
    for (unsigned i = 0; i < 3; ++i) issue(&c, 0);
    assert(c.cycles == 4);
    assert(!execute(&c, 11u << 23 | 1u << 7 | 0x28)); /* MVK .S1 1,A11 */
    assert(c.fault && strstr(c.fault, "delayed-result write conflict"));
    assert(c.cycles == 4 && c.load_count == 1);
    assert(c.r[0][10] == 0x11223344 && c.r[0][11] == 0x55667788);
}

static void compact_circular_memory(void)
{
    /* Figures C-8/C-10/C-12/C-14: RS selects data and register offsets,
     * never the A4-7/B4-7 pointer whose AMR mode controls wrapping. */
    for (unsigned family = 0; family < 4; ++family)
    for (unsigned bank = 0; bank < 2; ++bank)
    for (unsigned data_side = 0; data_side < 2; ++data_side)
    for (unsigned base_reg = 4; base_reg <= 7; ++base_reg)
    for (unsigned rs = 0; rs <= 16; rs += 16)
    for (unsigned load = 0; load < 2; ++load) {
        CdjC674x c; reset(&c);
        unsigned data_reg = 3 + rs;
        uint32_t base = family == 3 ? 0x1100 : 0x111c;
        c.control[0] = amr(bank, base_reg, 1, 4);
        c.r[bank][base_reg] = base;
        c.r[bank][base_reg + 16] = 0xfffffff0; /* Wrong RS pointer must fail. */
        c.r[bank][1 + rs] = 9;
        c.r[data_side][data_reg] = 0x12345678;
        unsigned delta = family < 2 ? 36 : 8;
        uint32_t updated = wrap(base, family == 3 ? base - delta : base + delta, 4);
        uint32_t address = family == 2 ? base : updated;
        uint32_t expected = 0;
        for (unsigned i = 0; i < 4; ++i)
            expected |= (uint32_t)memory[address - 0x1000 + i] << (8 * i);
        unsigned fixed = family == 0 ? 0x0004 : family == 1 ? 0x0404 :
                         family == 2 ? 0x0c04 : 0x4c04;
        uint32_t word = fixed | 1u << 13 | (family == 0 ? 1u << 11 : 0) |
                        data_side << 12 | (base_reg - 4) << 7 | 3u << 4 |
                        load << 3 | bank;
        CdjC674xPacket p = {.count = 1, .next_pc = 0x1002,
            .instructions = {{.compact = true, .pc = 0x1000, .word = word,
                              .header = 0xe0000000u | (rs ? 1u << 19 : 0)}}};
        assert(cdj_c674x_execute(&c, &p, read_word, write_value, NULL));
        assert(c.r[bank][base_reg] == (family < 2 ? base : updated));
        assert(c.r[bank][base_reg + 16] == 0xfffffff0);
        assert(c.r[data_side][data_reg] == 0x12345678 && !committed);
        for (unsigned i = 0; i < 4; ++i) issue(&c, 0);
        if (load) assert(c.r[data_side][data_reg] == expected);
        else {
            uint32_t actual = 0;
            for (unsigned i = 0; i < 4; ++i)
                actual |= (uint32_t)memory[address - 0x1000 + i] << (8 * i);
            assert(committed && actual == 0x12345678);
        }
    }
}

int main(void)
{
    arithmetic(); aligned_memory(); mvc_and_reserved(); nonaligned_memory();
    circular_conflicts();
    compact_circular_memory();
    puts("C674x AMR/circular addressing tests passed");
    return 0;
}
