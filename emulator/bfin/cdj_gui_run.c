/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cdj-gui-run: the CDJ-2000NXS GUI board (BF531) on the vendored Blackfin
 * core, GUI-only -- no MAIN link yet.
 *
 * Based on hw/cdj/bfin/bfinrun.c from Stijn Jacobs' cdj-nxs2-qemu
 * (https://github.com/Stijn-Jacobs/cdj-nxs2-qemu, commit 08d5cb1).
 * Changed 2026-10-05: our inputs (UPD, update body, bare LDR or the 2 MiB
 * flash image bin/cdj-run's board file uses, plus an optional separate
 * flash), 64 MiB SDRAM, frames published as bin/cdj-run publishes them
 * (P6, the PPI's 480x255 capture, rgb555le expanded (v << 3) | (v >> 2),
 * written through a temporary file and a rename, unchanged frames skipped),
 * the PF0 ready toggle, a virtual-seconds budget and a stats line.
 *
 *   cdj-gui-run [-s seconds | -n cycles] [-o screen.ppm] [-f flash.bin]
 *               [-g mask] [-x lo:hi] [-q] BOOT
 *
 * -x reports how many 256-byte code lines in [lo, hi) ran (see
 * bfin_code_lines_run), e.g. a mod's extension range.
 *
 * BOOT is a C2KGUI.UPD (0x20-byte title), its body, a bare LDR stream, or a
 * 2 MiB flash image: firmware/nxs/gui-flash-image.bin (stream at 0, the
 * image bin/cdj-run's board file maps) or a dump with the body at 0x10000.
 * Without -f the flash holds BOOT itself -- a UPD, body or LDR at 0x10000,
 * as the GUI's updater programs it.
 *
 * Exit status: 0 the budget ran out, 1 an unimplemented instruction (PC and
 * words on stderr) or an idle with nothing left to wake it, 2 bad input.
 */
#include "bf531.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FLASH_APP 0x10000u

typedef struct runner {
    const char *out;
    char *tmp;
    uint8_t *rgb, *published;
    uint16_t *raw;              /* the last frame's pixels, as scanned */
    size_t rgb_size;
    unsigned w, h;
    uint64_t published_n;
} runner;

static void on_frame(void *opaque, const uint16_t *px, unsigned w, unsigned h)
{
    runner *r = opaque;
    size_t n = (size_t)w * h, size = n * 3;
    FILE *f;

    if (size != r->rgb_size) {
        r->rgb = realloc(r->rgb, size);
        r->published = realloc(r->published, size);
        r->raw = realloc(r->raw, n * 2);
        r->rgb_size = size;
        r->published_n = 0;
    } else if (!memcmp(r->raw, px, n * 2)) {
        return;                 /* the PPI rescans an unchanged picture */
    }
    memcpy(r->raw, px, n * 2);
    for (size_t k = 0; k < n; k++) {
        unsigned p = px[k], c[3] = { (p >> 10) & 31, (p >> 5) & 31, p & 31 };

        for (int j = 0; j < 3; j++) {
            r->rgb[k * 3 + j] = (c[j] << 3) | (c[j] >> 2);
        }
    }
    if (r->published_n && w == r->w && h == r->h &&
        !memcmp(r->rgb, r->published, size)) {
        return;
    }
    memcpy(r->published, r->rgb, size);
    r->w = w;
    r->h = h;
    r->published_n++;
    if (!(f = fopen(r->tmp, "wb"))) {
        perror(r->tmp);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    fwrite(r->rgb, 1, size, f);
    if (fclose(f) == 0 && rename(r->tmp, r->out) != 0) {
        perror(r->out);
    }
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long size;

    if (!f) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 &&
        fseek(f, 0, SEEK_SET) == 0 && (buf = malloc(size)) &&
        fread(buf, 1, size, f) == (size_t)size) {
        *len = size;
    } else {
        free(buf);
        buf = NULL;
        fprintf(stderr, "%s: cannot read\n", path);
    }
    fclose(f);
    return buf;
}

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    runner r = { .out = "screen.ppm" };
    bf531_host host = { .opaque = &r, .frame = on_frame };
    uint64_t cycles = 60 * BF531_CCLK_HZ;
    const char *flash_path = NULL;
    uint32_t xlo = 0, xhi = 0;
    char *end;
    uint16_t toggle = 1;
    FILE *log = stderr;
    uint8_t *boot, *flash, *fimg = NULL;
    const uint8_t *ldr;
    size_t boot_len, ldr_len, fimg_len = 0;
    uint32_t flash_size;
    bfin_stop stop;
    double t0;
    bf531 *s;
    int opt;

    while ((opt = getopt(argc, argv, "s:n:o:f:g:x:q")) != -1) {
        switch (opt) {
        case 's': cycles = (uint64_t)(strtod(optarg, NULL) * BF531_CCLK_HZ); break;
        case 'n': cycles = strtoull(optarg, NULL, 0); break;
        case 'o': r.out = optarg; break;
        case 'f': flash_path = optarg; break;
        case 'g': toggle = strtoul(optarg, NULL, 0); break;
        case 'x':
            xlo = strtoul(optarg, &end, 0);
            xhi = *end == ':' ? strtoul(end + 1, NULL, 0) : xlo + 1;
            break;
        case 'q': log = NULL; break;
        default:
            fprintf(stderr, "usage: %s [-s seconds | -n cycles] [-o screen.ppm] "
                    "[-f flash.bin] [-g mask] [-x lo:hi] [-q] BOOT\n", argv[0]);
            return 2;
        }
    }
    if (optind + 1 != argc || !(boot = read_file(argv[optind], &boot_len))) {
        fprintf(stderr, "%s: one boot image (UPD, body, LDR or flash) needed\n", argv[0]);
        return 2;
    }
    if (flash_path && !(fimg = read_file(flash_path, &fimg_len))) {
        return 2;
    }
    r.tmp = malloc(strlen(r.out) + 5);
    strcpy(r.tmp, r.out);
    strcat(r.tmp, ".tmp");

    s = bf531_new(64u << 20, &host, log);
    flash = bf531_flash(s, &flash_size);
    ldr = boot;
    ldr_len = boot_len;
    if (boot_len == flash_size) {
        /* A flash image: firmware.py's physical_flash_image puts the stream
         * at 0, the GUI's updater (a BFIN_CFI_DUMP) at 0x10000. */
        uint32_t first;

        memcpy(&first, boot, 4);
        if (first == 0xFFFFFFFFu) {
            ldr += FLASH_APP;
            ldr_len -= FLASH_APP;
        }
    } else if (boot_len > 0x20 && !memcmp(boot, "CDJ-", 4)) {   /* a UPD */
        ldr += 0x20;
        ldr_len -= 0x20;
    }
    if (fimg) {
        if (fimg_len > flash_size) {
            fprintf(stderr, "%s: larger than the 2 MiB flash\n", flash_path);
            return 2;
        }
        memcpy(flash, fimg, fimg_len);
    } else if (boot_len == flash_size) {
        memcpy(flash, boot, flash_size);
    } else if (ldr_len <= flash_size - FLASH_APP) {
        memcpy(flash + FLASH_APP, ldr, ldr_len);
    }
    if (bf531_boot_ldr(s, ldr, ldr_len)) {
        fprintf(stderr, "%s: not an LDR boot stream\n", argv[optind]);
        return 2;
    }
    bf531_set_ready_toggle(s, toggle);

    t0 = now();
    stop = bf531_run(s, cycles);
    double wall = now() - t0;

    bfin_core *c = bf531_core(s);
    fprintf(stderr, "STATS cycles=%" PRIu64 " virtual_s=%.3f wall=%.3f speed=%.1fx "
            "frames=%" PRIu64 " published=%" PRIu64 " pc=0x%08x\n",
            bfin_cycles(c), bfin_cycles(c) / (double)BF531_CCLK_HZ, wall,
            bfin_cycles(c) / (double)BF531_CCLK_HZ / (wall > 0 ? wall : 1e-9),
            bf531_frames(s), r.published_n, bfin_get_pc(c));
    if (xhi > xlo) {
        fprintf(stderr, "RANGE 0x%08x:0x%08x lines_run=%u\n", xlo, xhi,
                bfin_code_lines_run(c, xlo, xhi));
    }
    if (stop == BFIN_STOP_UNDEF) {
        /* bfin_trap_insn: iw0 << 16 | iw1, and a 64-bit bundle's two more
         * words below those. (bfinrun printed a 16-bit form's padding word
         * instead of the instruction.) */
        uint64_t insn = bfin_trap_insn(c);
        unsigned top = insn >> 32 ? 48 : 16;
        unsigned len = bfin_insn_len(insn >> top);

        fprintf(stderr, "unimplemented at 0x%08x:", bfin_trap_pc(c));
        for (unsigned n = 0; n < len / 2; n++) {
            fprintf(stderr, " %04x", (unsigned)(insn >> (top - 16 * n)) & 0xFFFF);
        }
        fputc('\n', stderr);
    } else if (stop == BFIN_STOP_IDLE) {
        fprintf(stderr, "idle with no event pending\n");
    } else if (stop == BFIN_STOP_BREAK) {
        fprintf(stderr, "break at 0x%08x\n", bfin_get_pc(c));
    }
    bf531_free(s);
    free(boot);
    free(fimg);
    return stop != BFIN_STOP_BUDGET;
}
