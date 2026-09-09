/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_dsp_checkpoint.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECKPOINT_ENDIAN 0x01020304u
#define CHECKPOINT_MAGIC "CDJDSP4\0"
#define CHECKPOINT_SCHEMA1_MAGIC "CDJDSP1\0"
#define CHECKPOINT_SCHEMA2_MAGIC "CDJDSP2\0"
#define CHECKPOINT_SCHEMA3_MAGIC "CDJDSP3\0"
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
    /* Preserve the fixed nine-component header used by schemas 1-3. The
     * final component now describes the complete peripheral-state tail. */
    sizes[8] = sizeof(CdjC6747Emifb) + sizeof(CdjC6747Intc) +
               sizeof(CdjC6747Timer) * CDJ_C6747_TIMER_COUNT;
}

static void schema3_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    component_sizes(sizes);
    sizes[8] = sizeof(CdjC6747Emifb) + sizeof(CdjC6747Intc);
}

static void legacy_component_sizes(uint32_t sizes[CHECKPOINT_COMPONENTS])
{
    schema3_component_sizes(sizes);
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
    bool timers_valid = true;
    for (unsigned i = 0; i < CDJ_C6747_TIMER_COUNT; ++i) {
        const CdjC6747Timer *timer = &state->timers[i];
        timers_valid = timers_valid && timer->tim34_shadow_valid <= 1 &&
                       !(timer->emumgt & ~3u) &&
                       !(timer->gpintgpen & ~0x00030033u) &&
                       !(timer->gpdatgpdir & ~0x00030003u) &&
                       !(timer->tcr & ~0x04c03ffeu) &&
                       !(timer->tgcr & ~0x0000ff1fu) &&
                       !(timer->wdtcr & ~0xffffc000u) &&
                       !(timer->intctlstat & ~0x000f000fu);
    }
    return timers_valid && state->boot_phase <= 7 && state->reset_released <= 1 &&
           state->dsp_started <= 1 && state->dsp_halted <= 1 &&
           state->had_fault <= 1 && state->cpu.cycle_tick == NULL &&
           state->cpu.cycle_opaque == NULL && state->cpu.fault == NULL &&
           state->cpu.branch_count <= 5 && state->cpu.store_count <= 24 &&
           state->cpu.load_count <= 40 && state->cpu.loop.length <= 48 &&
           !(state->intc.event_flag[0] & 0xf) &&
           (state->intc.event_mask[0] & 0xf) == 0xf &&
           (state->intc.exception_mask[0] & 0xf) == 0xf &&
           !(state->intc.interrupt_mux[0] & 0x80808080u) &&
           !(state->intc.interrupt_mux[1] & 0x80808080u) &&
           !(state->intc.interrupt_mux[2] & 0x80808080u) &&
           state->stop_reason[sizeof(state->stop_reason) - 1] == '\0' &&
           state->fault[sizeof(state->fault) - 1] == '\0';
}

bool cdj_dsp_checkpoint_write(const char *path,
                              const CdjDspCheckpointState *input,
                              const uint8_t *l2, size_t l2_size,
                              const uint8_t *shared_ram, size_t shared_ram_size,
                              const uint8_t *sdram, size_t sdram_size,
                              char *error, size_t error_size)
{
    CdjDspCheckpointState state = *input;
    if (!path || !l2 || !shared_ram || !sdram ||
        l2_size != CDJ_DSP_L2_SIZE ||
        shared_ram_size != CDJ_DSP_SHARED_RAM_SIZE ||
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
    header.payload_size = sizeof(state) + l2_size + shared_ram_size + bitmap_size +
                          (uint64_t)present * CDJ_DSP_CHECKPOINT_PAGE_SIZE;
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = checksum(hash, &state, sizeof(state));
    hash = checksum(hash, l2, l2_size);
    hash = checksum(hash, shared_ram, shared_ram_size);
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
              fwrite(shared_ram, 1, shared_ram_size, file) == shared_ram_size &&
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
                             uint8_t *shared_ram, size_t shared_ram_size,
                             uint8_t *sdram, size_t sdram_size,
                             char *error, size_t error_size)
{
    if (!path || !state || !l2 || !shared_ram || !sdram ||
        l2_size != CDJ_DSP_L2_SIZE ||
        shared_ram_size != CDJ_DSP_SHARED_RAM_SIZE ||
        sdram_size != CDJ_DSP_SDRAM_SIZE) {
        fail(error, error_size, "missing checkpoint destination");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (error && error_size)
            snprintf(error, error_size, "cannot open checkpoint: %s", strerror(errno));
        return false;
    }
    CheckpointHeader header = {0};
    uint32_t expected[CHECKPOINT_COMPONENTS], schema3_expected[CHECKPOINT_COMPONENTS];
    uint32_t legacy_expected[CHECKPOINT_COMPONENTS];
    component_sizes(expected);
    schema3_component_sizes(schema3_expected);
    legacy_component_sizes(legacy_expected);
    bool header_read = fread(&header, 1, sizeof(header), file) == sizeof(header);
    bool schema1 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA1_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 1;
    bool schema2 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA2_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 2;
    bool schema3 = header_read &&
                   memcmp(header.magic, CHECKPOINT_SCHEMA3_MAGIC,
                          sizeof(header.magic)) == 0 && header.schema == 3;
    bool current = header_read &&
                   memcmp(header.magic, CHECKPOINT_MAGIC,
                          sizeof(header.magic)) == 0 &&
                   header.schema == CDJ_DSP_CHECKPOINT_SCHEMA;
    /* Appending INTC reused four bytes of the schema-2 structure's trailing
     * alignment padding, so round the prefix back to the old ABI size. */
    size_t alignment = _Alignof(CdjDspCheckpointState);
    size_t legacy_state_size = (offsetof(CdjDspCheckpointState, intc) +
                                alignment - 1) / alignment * alignment;
    size_t schema3_state_size = (offsetof(CdjDspCheckpointState, timers) +
                                 alignment - 1) / alignment * alignment;
    bool legacy = (schema1 || schema2) &&
                  header.state_size == legacy_state_size &&
                  memcmp(header.component_size, legacy_expected,
                         sizeof(legacy_expected)) == 0;
    bool old_schema3 = schema3 && header.state_size == schema3_state_size &&
                       memcmp(header.component_size, schema3_expected,
                              sizeof(schema3_expected)) == 0;
    bool old = legacy || old_schema3;
    bool ok = (old || current) &&
              header.endian == CHECKPOINT_ENDIAN &&
              header.header_size == sizeof(header) &&
              (old || (header.state_size == sizeof(*state) &&
                       memcmp(header.component_size, expected,
                              sizeof(expected)) == 0)) &&
              header.l2_size == l2_size && header.sdram_size == sdram_size &&
              header.page_size == CDJ_DSP_CHECKPOINT_PAGE_SIZE &&
              header.page_count == sdram_size / header.page_size;
    const size_t bitmap_size = ok ? (header.page_count + 7u) / 8u : 0;
    uint64_t expected_payload = header.state_size + l2_size +
        (schema1 ? 0 : shared_ram_size) + bitmap_size +
        (uint64_t)header.present_pages * CDJ_DSP_CHECKPOINT_PAGE_SIZE;
    ok = ok && header.present_pages <= header.page_count &&
         header.payload_size == expected_payload;
    uint8_t *bitmap = ok ? malloc(bitmap_size) : NULL;
    memset(state, 0, sizeof(*state));
    ok = ok && bitmap && fread(state, 1, header.state_size, file) == header.state_size &&
         fread(l2, 1, l2_size, file) == l2_size;
    if (ok && schema1) memset(shared_ram, 0, shared_ram_size);
    else if (ok) ok = fread(shared_ram, 1, shared_ram_size, file) == shared_ram_size;
    ok = ok && fread(bitmap, 1, bitmap_size, file) == bitmap_size;
    if (ok) memset(sdram, 0, sdram_size);
    uint32_t present = 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    if (ok) {
        hash = checksum(hash, state, header.state_size);
        hash = checksum(hash, l2, l2_size);
        if (!schema1) hash = checksum(hash, shared_ram, shared_ram_size);
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
         fgetc(file) == EOF && !ferror(file);
    /* Validate the old payload exactly as captured before initializing
     * appended state. This also avoids relying on old tail padding
     * having happened to contain zero bytes. */
    if (ok && legacy) cdj_c6747_intc_reset(&state->intc);
    if (ok && old) cdj_c6747_timers_reset(state->timers);
    ok = ok && state_valid(state);
    fclose(file);
    free(bitmap);
    if (!ok) fail(error, error_size, "incompatible, corrupt, or incomplete checkpoint");
    return ok;
}
