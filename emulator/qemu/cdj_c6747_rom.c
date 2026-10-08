/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cdj_c6747_rom.h"

/* SHA-256 images this board accepts without CDJ_DSP_ROM_SHA256: the table
 * stand-in tools/cdj_dsp/build_dsp_rom.py builds from TI's MP4AACDEC 1.01
 * objects (a hash is not the contents). */
static const char *const known_sha256[] = {
    "44d5f7783d1ccd898b84a399b01c89889341390f1cb6233404f97c7cee3a8c7f",
    NULL,
};

static uint32_t rotr(uint32_t x, unsigned n) { return x >> n | x << (32 - n); }

void cdj_c6747_sha256_hex(const uint8_t *data, size_t size, char out[65])
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint64_t total = (uint64_t)size + 1 + 8;
    size_t blocks = (size_t)((total + 63) / 64);
    for (size_t b = 0; b < blocks; ++b) {
        uint8_t blk[64];
        for (unsigned i = 0; i < 64; ++i) {
            uint64_t pos = (uint64_t)b * 64 + i;
            uint64_t end = (uint64_t)blocks * 64;
            if (pos < size) blk[i] = data[pos];
            else if (pos == size) blk[i] = 0x80;
            else if (pos >= end - 8) blk[i] = (uint8_t)(((uint64_t)size * 8) >> (8 * (end - 1 - pos)));
            else blk[i] = 0;
        }
        uint32_t w[64];
        for (unsigned i = 0; i < 16; ++i)
            w[i] = (uint32_t)blk[4*i] << 24 | blk[4*i+1] << 16 | blk[4*i+2] << 8 | blk[4*i+3];
        for (unsigned i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],bb=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (unsigned i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rotr(e,6)^rotr(e,11)^rotr(e,25)) + ((e&f)^(~e&g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a,2)^rotr(a,13)^rotr(a,22)) + ((a&bb)^(a&c)^(bb&c));
            hh=g; g=f; f=e; e=d+t1; d=c; c=bb; bb=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    for (unsigned i = 0; i < 8; ++i) snprintf(out + 8 * i, 9, "%08x", h[i]);
}

static void fail(char *error, size_t size, const char *what, const char *path)
{
    if (error && size) snprintf(error, size, "%s: %s", path, what);
}

bool cdj_c6747_rom_load(CdjC6747Rom *rom, const char *path,
                        const char *expected_hex, char *error, size_t size)
{
    cdj_c6747_rom_free(rom);
    FILE *f = fopen(path, "rb");
    if (!f) { fail(error, size, "cannot open", path); return false; }
    uint8_t *data = malloc(CDJ_C6747_ROM_SIZE + 1);
    size_t got = data ? fread(data, 1, CDJ_C6747_ROM_SIZE + 1, f) : 0;
    fclose(f);
    if (!data || got != CDJ_C6747_ROM_SIZE) {
        free(data);
        fail(error, size, "not exactly 1048576 bytes", path);
        return false;
    }
    char hex[65];
    cdj_c6747_sha256_hex(data, got, hex);
    bool ok = false;
    const char *override = expected_hex ? expected_hex : getenv("CDJ_DSP_ROM_SHA256");
    if (override && *override) {
        ok = !strcmp(hex, override);
    } else {
        for (unsigned i = 0; known_sha256[i]; ++i)
            if (!strcmp(hex, known_sha256[i])) ok = true;
    }
    if (!ok) {
        free(data);
        if (error && size)
            snprintf(error, size, "%s: sha256 %s is not a pinned image "
                     "(set CDJ_DSP_ROM_SHA256 to accept it)", path, hex);
        return false;
    }
    rom->data = data;
    return true;
}

bool cdj_c6747_rom_init_env(CdjC6747Rom *rom)
{
    const char *path = getenv("CDJ_DSP_ROM");
    char error[512];
    rom->data = NULL;
    if (!path || !*path) {
        fprintf(stderr, "cdj-dsp: warning: no DSP L2 ROM (--dsp-rom / CDJ_DSP_ROM): "
                "0x11700000-0x117fffff stays unmapped, so stock AAC decode will "
                "fault on its table reads (tools/cdj_dsp/build_dsp_rom.py builds one)\n");
        return false;
    }
    if (!cdj_c6747_rom_load(rom, path, NULL, error, sizeof error)) {
        fprintf(stderr, "cdj-dsp: warning: DSP L2 ROM not mapped: %s\n", error);
        return false;
    }
    return true;
}

void cdj_c6747_rom_free(CdjC6747Rom *rom)
{
    free(rom->data);
    rom->data = NULL;
}

bool cdj_c6747_rom_read(const CdjC6747Rom *rom, uint32_t address, uint32_t *value)
{
    if (!rom->data || (address & 3)) return false;
    if (address >= CDJ_C6747_ROM_LOCAL && address < CDJ_C6747_ROM_LOCAL + CDJ_C6747_ROM_SIZE)
        address += CDJ_C6747_ROM_BASE - CDJ_C6747_ROM_LOCAL;
    if (address < CDJ_C6747_ROM_BASE || address - CDJ_C6747_ROM_BASE >= CDJ_C6747_ROM_SIZE)
        return false;
    const uint8_t *p = rom->data + (address - CDJ_C6747_ROM_BASE);
    *value = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    return true;
}
