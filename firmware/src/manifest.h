// Universal Black Box - capture manifest (RAM copy + precomputed read runs)
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ubb_format.h"
#include "config.h"
#include "dap.h"

typedef struct {
    uint32_t addr;        // word aligned start
    uint16_t nwords;
    uint16_t first;       // first entry index in this run
    uint16_t count;       // entries covered
    uint8_t  cls;
} ubb_run_t;

typedef struct {
    bool              valid;
    uint32_t          hash;      // CRC32 of the raw manifest bytes
    ubb_manifest_hdr_t hdr;
    ubb_entry_t       entries[UBB_MAX_ENTRIES];
    ubb_region_t      regions[UBB_MAX_REGIONS];
    ubb_trigger_t     triggers[UBB_MAX_TRIGGERS];
    uint16_t          n_entries, n_regions, n_triggers;
    ubb_run_t         runs[UBB_MAX_ENTRIES];
    uint16_t          n_runs;
    uint32_t          keyframe_bytes;   // sum of entry sizes
    dap_profile_t     profile;
} ubb_manifest_t;

extern ubb_manifest_t g_manifest;

// Parse raw manifest bytes into g_manifest. Returns false if invalid.
bool manifest_load(const uint8_t *raw, uint32_t len);
// Same, into a caller-provided structure (validation without touching g_manifest).
bool manifest_parse(const uint8_t *raw, uint32_t len, ubb_manifest_t *out);
// Load a built-in fallback (no variables, RP2040 profile, passive level).
void manifest_load_default(void);
void manifest_profile_for_target(uint8_t target, dap_profile_t *p);
