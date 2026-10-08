/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C6747_ROM_H
#define CDJ_C6747_ROM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The C6747 DSP L2 ROM: 1024 kB at global 0x11700000 (local 0x00700000,
 * SPRUH91D printed page 80).  Stock's AAC decoder reads TI tables from it.
 * The contents are TI's and are never in this repository: the board loads
 * them at run time from a user-supplied file (CDJ_DSP_ROM, tools/cdj_dsp/
 * build_dsp_rom.py builds a table image from the user's own TI download).
 * Read-only: there is no store path. */
#define CDJ_C6747_ROM_BASE  0x11700000u
#define CDJ_C6747_ROM_SIZE  0x00100000u
#define CDJ_C6747_ROM_LOCAL 0x00700000u   /* local alias, global - 0x11000000 */

typedef struct {
    uint8_t *data;          /* CDJ_C6747_ROM_SIZE bytes, or NULL (unmapped) */
} CdjC6747Rom;

/* Load `path` (exactly CDJ_C6747_ROM_SIZE bytes) and require its SHA-256 to be
 * `expected_hex`, or, when that is NULL, one of the pinned images (the
 * CDJ_DSP_ROM_SHA256 environment variable, then the built-in list).  On
 * failure the ROM stays unmapped and `error` says why. */
bool cdj_c6747_rom_load(CdjC6747Rom *rom, const char *path,
                        const char *expected_hex, char *error, size_t size);
/* CDJ_DSP_ROM from the environment; prints a clear warning on stderr when it
 * is unset (the ROM stays unmapped, the historical behaviour) or fails to
 * load.  Returns true when the ROM is mapped. */
bool cdj_c6747_rom_init_env(CdjC6747Rom *rom);
void cdj_c6747_rom_free(CdjC6747Rom *rom);
/* Aligned word read at a global or local-alias address; false when unmapped
 * or outside the ROM. */
bool cdj_c6747_rom_read(const CdjC6747Rom *rom, uint32_t address,
                        uint32_t *value);
/* SHA-256 of a buffer as 64 lowercase hex characters plus NUL. */
void cdj_c6747_sha256_hex(const uint8_t *data, size_t size, char out[65]);

#endif
