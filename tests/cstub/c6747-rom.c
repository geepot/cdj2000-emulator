/* SPDX-License-Identifier: GPL-2.0-or-later */
/* C6747 L2 ROM: user-supplied, hash-checked, read-only.  argv[1] is a scratch
 * directory. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cdj_c6747_rom.h"

static void put(const char *path, size_t size, uint8_t fill)
{
    FILE *f = fopen(path, "wb");
    for (size_t i = 0; i < size; ++i) fputc((uint8_t)(fill + i), f);
    fclose(f);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char hex[65], path[600], error[600];
    cdj_c6747_sha256_hex((const uint8_t *)"abc", 3, hex);
    assert(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    cdj_c6747_sha256_hex((const uint8_t *)"", 0, hex);
    assert(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    uint8_t two_blocks[56]; memset(two_blocks, 'a', sizeof two_blocks);   /* pads into a 2nd block */
    cdj_c6747_sha256_hex(two_blocks, sizeof two_blocks, hex);
    assert(!strcmp(hex, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));

    /* Absent: unmapped, the historical behaviour (plus a warning). */
    CdjC6747Rom rom = {0};
    unsetenv("CDJ_DSP_ROM"); unsetenv("CDJ_DSP_ROM_SHA256");
    uint32_t v = 0xdeadbeef;
    assert(!cdj_c6747_rom_init_env(&rom) && !rom.data);
    assert(!cdj_c6747_rom_read(&rom, CDJ_C6747_ROM_BASE, &v) && v == 0xdeadbeef);

    /* Wrong size and unpinned hash are refused. */
    snprintf(path, sizeof path, "%s/rom.bin", argv[1]);
    put(path, 1000, 0);
    assert(!cdj_c6747_rom_load(&rom, path, NULL, error, sizeof error) && !rom.data);
    put(path, CDJ_C6747_ROM_SIZE, 1);
    assert(!cdj_c6747_rom_load(&rom, path, NULL, error, sizeof error) && !rom.data);
    assert(strstr(error, "not a pinned image"));

    /* A pinned hash maps it. */
    uint8_t *image = malloc(CDJ_C6747_ROM_SIZE);
    for (size_t i = 0; i < CDJ_C6747_ROM_SIZE; ++i) image[i] = (uint8_t)(1 + i);
    cdj_c6747_sha256_hex(image, CDJ_C6747_ROM_SIZE, hex);
    free(image);
    assert(cdj_c6747_rom_load(&rom, path, hex, error, sizeof error) && rom.data);
    assert(!cdj_c6747_rom_load(&rom, path, "00", error, sizeof error) && !rom.data);
    setenv("CDJ_DSP_ROM", path, 1); setenv("CDJ_DSP_ROM_SHA256", hex, 1);
    assert(cdj_c6747_rom_init_env(&rom));

    /* Little-endian words in the global window and its local alias; nothing
     * outside, nothing unaligned. */
    assert(cdj_c6747_rom_read(&rom, CDJ_C6747_ROM_BASE, &v) && v == 0x04030201u);
    assert(cdj_c6747_rom_read(&rom, CDJ_C6747_ROM_BASE + 0x1234 * 4, &v));
    uint32_t alias;
    assert(cdj_c6747_rom_read(&rom, CDJ_C6747_ROM_LOCAL + 0x1234 * 4, &alias) && alias == v);
    assert(cdj_c6747_rom_read(&rom, CDJ_C6747_ROM_BASE + CDJ_C6747_ROM_SIZE - 4, &v));
    assert(!cdj_c6747_rom_read(&rom, CDJ_C6747_ROM_BASE - 4, &v));
    assert(!cdj_c6747_rom_read(&rom, CDJ_C6747_ROM_BASE + CDJ_C6747_ROM_SIZE, &v));
    assert(!cdj_c6747_rom_read(&rom, CDJ_C6747_ROM_BASE + 2, &v));
    cdj_c6747_rom_free(&rom);
    puts("ok");
    return 0;
}
