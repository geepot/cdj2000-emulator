/* SPDX-License-Identifier: GPL-2.0-or-later
 * Headless replay of an NXS pre-execution UHPI L2 dump. No boot ROM or
 * external processors run here; this is a deterministic core diagnostic. */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include "cdj_c674x.h"
#include "cdj_c6747_syscfg.h"
static uint8_t ram[0x40000];
static CdjC6747Syscfg syscfg;
static uint32_t global(uint32_t a)
{ return a >= 0x00800000 && a < 0x00840000 ? a + 0x11000000 : a; }
static bool read_bus(void *unused, uint32_t a, uint32_t *v)
{
    (void)unused;
    if (cdj_c6747_syscfg_read(&syscfg, a, v)) return true;
    a = global(a);
    if ((a & 3) || a < 0x11800000 || a > 0x1183fffc) return false;
    a -= 0x11800000;
    *v = ram[a] | (uint32_t)ram[a+1] << 8 | (uint32_t)ram[a+2] << 16 | (uint32_t)ram[a+3] << 24;
    return true;
}
static bool write_bus(void *unused, uint32_t a, uint64_t v, unsigned size, bool commit)
{
    (void)unused;
    bool ok = cdj_c6747_syscfg_write(&syscfg, a, v, size, commit);
    uint32_t physical = global(a);
    if (!ok && (size == 1 || size == 2 || size == 4 || size == 8) &&
        physical >= 0x11800000 && physical <= 0x11840000 - size) {
        ok = true;
        if (commit) for (unsigned i = 0; i < size; ++i)
            ram[physical - 0x11800000 + i] = v >> (8*i);
    }
    if (commit || !ok)
        printf("{\"event\":\"%s\",\"address\":%" PRIu32 ",\"value\":%" PRIu64 ",\"size\":%u}\n",
               ok ? "write" : "rejected_write", a, v, size);
    return ok;
}
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    char *end;
    errno = 0;
    unsigned long long limit = strtoull(argv[2], &end, 10);
    if (errno || *end || !limit || argv[2][0] == '-') return 2;
    errno = 0;
    unsigned long long breakpoint = strtoull(argv[3], &end, 0);
    if (errno || *end || breakpoint > UINT32_MAX) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("dump"); return 2; }
    bool valid = fread(ram, 1, sizeof(ram), f) == sizeof(ram) && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!valid) { fputs("expected exactly 256 KiB of L2\n", stderr); return 2; }
    CdjC674x c;
    uint32_t entry;
    read_bus(NULL, 0x11800000, &entry);
    cdj_c674x_reset(&c, entry);
    const char *reason = "step_limit";
    for (unsigned long long step = 0; step < limit; ++step) {
        if (breakpoint && c.pc == breakpoint) { reason = "breakpoint"; break; }
        printf("{\"event\":\"step\",\"pc\":%" PRIu32 ",\"cycles\":%" PRIu64
               ",\"loop_active\":%s,\"branch_due\":%" PRIu64 "}\n",
               c.pc, c.cycles, c.loop_active ? "true" : "false", c.branch_due);
        if (!cdj_c674x_step(&c, read_bus, write_bus, NULL)) { reason = "fault"; break; }
    }
    /* Fault strings originate in the interpreter and contain no JSON escapes. */
    printf("{\"event\":\"stop\",\"reason\":\"%s\",\"fault\":\"%s\",\"pc\":%" PRIu32
           ",\"fault_pc\":%" PRIu32 ",\"fault_word\":%" PRIu32
           ",\"packets\":%" PRIu64 ",\"cycles\":%" PRIu64 ",\"registers\":[",
           reason, c.fault ? c.fault : "", c.pc, c.fault_pc, c.fault_word, c.packets, c.cycles);
    for (unsigned bank = 0; bank < 2; ++bank) {
        printf("%s[", bank ? "," : "");
        for (unsigned i = 0; i < 32; ++i) printf("%s%" PRIu32, i ? "," : "", c.r[bank][i]);
        printf("]");
    }
    printf("],\"pending_stores\":%u,\"pending_loads\":%u,\"syscfg_unlocked\":%s}\n",
           c.store_count, c.load_count, syscfg.unlocked ? "true" : "false");
    return ferror(stdout) ? 2 : 0;
}
