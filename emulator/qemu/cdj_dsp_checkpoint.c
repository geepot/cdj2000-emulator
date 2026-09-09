/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_dsp_checkpoint.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECKPOINT_ENDIAN 0x01020304u
#define CHECKPOINT_MAGIC "CDJDSP1\0"
#define CHECKPOINT_COMPONENTS 9u

typedef struct {
    char magic[8];
    uint32_t schema, endian, header_size, state_size;
    uint32_t component_size[CHECKPOINT_COMPONENTS];
    uint32_t l2_size, sdram_size, page_size, page_count, present_pages;
    uint64_t payload_size, payload_checksum;
} CheckpointHeader;

static void fail(char *error, size_t size, const char *message)
{
    if (error && size) snprintf(error, size, "%s", message);
}

static uint64_t checksum(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    sizes[0] = sizeof(CdjC674x);
    sizes[1] = sizeof(CdjC6747Syscfg);
    sizes[2] = sizeof(CdjC6747Psc);
    sizes[3] = sizeof(CdjC6747Mcasp);
    sizes[4] = sizeof(CdjC6747Gpio);
    sizes[5] = sizeof(CdjC6747I2c);
    sizes[6] = sizeof(CdjC6747Pll);
    sizes[7] = sizeof(CdjC6747Hpi);
    sizes[8] = sizeof(CdjC6747Emifb);
}

void cdj_dsp_checkpoint_prepare(CdjDspCheckpointState *state,
                                const char *reason)
{
    const char *fault = state->cpu.fault;
    state->had_fault = fault != NULL;
    snprintf(state->fault, sizeof(state->fault), "%s", fault ? fault : "");
    snprintf(state->stop_reason, sizeof(state->stop_reason), "%s",
             reason ? reason : "");
    state->cpu.cycle_tick = NULL;
    state->cpu.cycle_opaque = NULL;
    state->cpu.fault = NULL;
}

static bool state_valid(const CdjDspCheckpointState *state)
{
    return state->boot_phase <= 7 && state->reset_released <= 1 &&
           state->dsp_started <= 1 && state->dsp_halted <= 1 &&
           state->had_fault <= 1 && state->cpu.cycle_tick == NULL &&
           state->cpu.cycle_opaque == NULL && state->cpu.fault == NULL &&
           state->cpu.branch_count <= 5 && state->cpu.store_count <= 24 &&
           state->cpu.load_count <= 40 && state->cpu.loop.length <= 48 &&
           state->stop_reason[sizeof(state->stop_reason) - 1] == '\0' &&
           state->fault[sizeof(state->fault) - 1] == '\0';
}

bool cdj_dsp_checkpoint_write(const char *path,
                              const CdjDspCheckpointState *input,
                              const uint8_t *l2, size_t l2_size,
                              const uint8_t *sdram, size_t sdram_size,
                              char *error, size_t error_size)
{
    CdjDspCheckpointState state = *input;
    if (!path || !l2 || !sdram || l2_size != CDJ_DSP_L2_SIZE ||
        sdram_size != CDJ_DSP_SDRAM_SIZE || !state_valid(&state)) {
        fail(error, error_size, "invalid checkpoint state or memory size");
        return false;
    }
    const uint32_t page_count = sdram_size / CDJ_DSP_CHECKPOINT_PAGE_SIZE;
    const size_t bitmap_size = (page_count + 7u) / 8u;
    uint8_t *bitmap = calloc(1, bitmap_size);
    if (!bitmap) {
        fail(error, error_size, "cannot allocate checkpoint page bitmap");
        return false;
    }
    uint32_t present = 0;
    for (uint32_t page = 0; page < page_count; ++page) {
        const uint8_t *data = sdram + (size_t)page * CDJ_DSP_CHECKPOINT_PAGE_SIZE;
        bool nonzero = false;
        for (size_t i = 0; i < CDJ_DSP_CHECKPOINT_PAGE_SIZE; ++i) nonzero |= data[i] != 0;
        if (nonzero) {
            bitmap[page / 8] |= 1u << (page % 8);
            ++present;
        }
    }
    CheckpointHeader header = {0};
    memcpy(header.magic, CHECKPOINT_MAGIC, sizeof(header.magic));
    header.schema = CDJ_DSP_CHECKPOINT_SCHEMA;
    header.endian = CHECKPOINT_ENDIAN;
    header.header_size = sizeof(header);
    header.state_size = sizeof(state);
    component_sizes(header.component_size);
    header.l2_size = l2_size;
    header.sdram_size = sdram_size;
    header.page_size = CDJ_DSP_CHECKPOINT_PAGE_SIZE;
    header.page_count = page_count;
    header.present_pages = present;
    header.payload_size = sizeof(state) + l2_size + bitmap_size +
                          (uint64_t)present * CDJ_DSP_CHECKPOINT_PAGE_SIZE;
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = checksum(hash, &state, sizeof(state));
    hash = checksum(hash, l2, l2_size);
    hash = checksum(hash, bitmap, bitmap_size);
    for (uint32_t page = 0; page < page_count; ++page) {
        if (bitmap[page / 8] & (1u << (page % 8)))
            hash = checksum(hash, sdram + (size_t)page * header.page_size,
                            header.page_size);
    }
    header.payload_checksum = hash;
    FILE *file = fopen(path, "wb");
    if (!file) {
        if (error && error_size)
            snprintf(error, error_size, "cannot create checkpoint: %s", strerror(errno));
        free(bitmap);
        return false;
    }
    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
              fwrite(&state, 1, sizeof(state), file) == sizeof(state) &&
              fwrite(l2, 1, l2_size, file) == l2_size &&
              fwrite(bitmap, 1, bitmap_size, file) == bitmap_size;
    for (uint32_t page = 0; ok && page < page_count; ++page) {
        if (bitmap[page / 8] & (1u << (page % 8))) {
            const uint8_t *data = sdram + (size_t)page * header.page_size;
            ok = fwrite(data, 1, header.page_size, file) == header.page_size;
        }
    }
    ok = ok && fflush(file) == 0 && fclose(file) == 0;
    free(bitmap);
    if (!ok) {
        remove(path);
        fail(error, error_size, "incomplete checkpoint write");
    }
    return ok;
}

bool cdj_dsp_checkpoint_read(const char *path,
                             CdjDspCheckpointState *state,
                             uint8_t *l2, size_t l2_size,
                             uint8_t *sdram, size_t sdram_size,
                             char *error, size_t error_size)
{
    if (!path || !state || !l2 || !sdram) {
        fail(error, error_size, "missing checkpoint destination");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (error && error_size)
            snprintf(error, error_size, "cannot open checkpoint: %s", strerror(errno));
        return false;
    }
    CheckpointHeader header;
    uint32_t expected[CHECKPOINT_COMPONENTS];
    component_sizes(expected);
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
              memcmp(header.magic, CHECKPOINT_MAGIC, sizeof(header.magic)) == 0 &&
              header.schema == CDJ_DSP_CHECKPOINT_SCHEMA &&
              header.endian == CHECKPOINT_ENDIAN &&
              header.header_size == sizeof(header) &&
              header.state_size == sizeof(*state) &&
              memcmp(header.component_size, expected, sizeof(expected)) == 0 &&
              header.l2_size == l2_size && header.sdram_size == sdram_size &&
              header.page_size == CDJ_DSP_CHECKPOINT_PAGE_SIZE &&
              header.page_count == sdram_size / header.page_size;
    const size_t bitmap_size = ok ? (header.page_count + 7u) / 8u : 0;
    uint64_t expected_payload = sizeof(*state) + l2_size + bitmap_size +
        (uint64_t)header.present_pages * CDJ_DSP_CHECKPOINT_PAGE_SIZE;
    ok = ok && header.present_pages <= header.page_count &&
         header.payload_size == expected_payload;
    uint8_t *bitmap = ok ? malloc(bitmap_size) : NULL;
    ok = ok && bitmap && fread(state, 1, sizeof(*state), file) == sizeof(*state) &&
         fread(l2, 1, l2_size, file) == l2_size &&
         fread(bitmap, 1, bitmap_size, file) == bitmap_size;
    if (ok) memset(sdram, 0, sdram_size);
    uint32_t present = 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    if (ok) {
        hash = checksum(hash, state, sizeof(*state));
        hash = checksum(hash, l2, l2_size);
        hash = checksum(hash, bitmap, bitmap_size);
    }
    for (uint32_t page = 0; ok && page < header.page_count; ++page) {
        if (bitmap[page / 8] & (1u << (page % 8))) {
            uint8_t *data = sdram + (size_t)page * header.page_size;
            ok = fread(data, 1, header.page_size, file) == header.page_size;
            if (ok) hash = checksum(hash, data, header.page_size);
            ++present;
        }
    }
    ok = ok && present == header.present_pages && hash == header.payload_checksum &&
         fgetc(file) == EOF && !ferror(file) && state_valid(state);
    fclose(file);
    free(bitmap);
    if (!ok) fail(error, error_size, "incompatible, corrupt, or incomplete checkpoint");
    return ok;
}
