// Universal Black Box - capture manifest (RAM copy + precomputed read runs)
#include "manifest.h"
#include "crc.h"
#include <string.h>

ubb_manifest_t g_manifest;

void manifest_profile_for_target(uint8_t target, dap_profile_t *p)
{
    memset(p, 0, sizeof *p);
    if (target == UBB_TGT_RP2350) {
        p->adiv6 = true;
        p->targetsel[0] = 0x00040927u;  // single ADIv6 DP, instance 0
        p->targetsel[1] = 0;
        p->ap_base[0] = 0x2000;         // core 0 AHB-AP
        p->ap_base[1] = 0x4000;         // core 1 AHB-AP
        p->timerawl_addr = 0x400B0000u + 0x28;   // TIMER0.TIMERAWL
        p->chip_reset_addr = 0x40100000u + 0x28; // POWMAN.CHIP_RESET
        p->wdog_reason_addr = 0x400D8000u + 0x08; // WATCHDOG.REASON
        p->sram_base = 0x20000000u;
        p->sram_size = 520u * 1024u;
    } else {
        p->adiv6 = false;
        p->targetsel[0] = 0x01002927u;  // core 0 DP
        p->targetsel[1] = 0x11002927u;  // core 1 DP
        p->ap_base[0] = 0;              // APSEL 0 on each DP
        p->ap_base[1] = 0;
        p->timerawl_addr = 0x40054000u + 0x28;   // TIMER.TIMERAWL
        p->chip_reset_addr = 0x40064000u + 0x08; // VREG_AND_CHIP_RESET.CHIP_RESET
        p->wdog_reason_addr = 0x40058000u + 0x08; // WATCHDOG.REASON
        p->sram_base = 0x20000000u;
        p->sram_size = 264u * 1024u;
    }
}

static bool in_sram(const dap_profile_t *p, uint32_t a)
{
    return a >= p->sram_base && a < p->sram_base + p->sram_size;
}

static void build_runs(ubb_manifest_t *m)
{
    m->n_runs = 0;
    m->keyframe_bytes = 0;
    for (uint16_t i = 0; i < m->n_entries; i++) m->keyframe_bytes += m->entries[i].size;

    // Entries arrive sorted by (class, address).  Merge neighbours into one
    // block read when that reads no extra bytes outside SRAM.
    uint16_t i = 0;
    while (i < m->n_entries) {
        const ubb_entry_t *e = &m->entries[i];
        uint32_t start = e->addr & ~3u;
        uint32_t end = (e->addr + e->size + 3u) & ~3u;
        uint16_t count = 1;
        while (i + count < m->n_entries) {
            const ubb_entry_t *n = &m->entries[i + count];
            if (n->cls != e->cls) break;
            uint32_t nstart = n->addr & ~3u;
            uint32_t nend = (n->addr + n->size + 3u) & ~3u;
            if (nstart < start) break;                    // not sorted: stop merging
            bool sram = in_sram(&m->profile, end) && in_sram(&m->profile, nstart);
            uint32_t gap = nstart > end ? nstart - end : 0;
            if (gap > (sram ? 32u : 0u)) break;           // peripherals: only exact neighbours
            if (nend - start > 256u) break;               // keep runs short (atomicity window)
            if (nend > end) end = nend;
            count++;
        }
        ubb_run_t *r = &m->runs[m->n_runs++];
        r->addr = start;
        r->nwords = (uint16_t)((end - start) / 4u);
        r->first = i;
        r->count = count;
        r->cls = e->cls;
        i += count;
    }
}

bool manifest_load(const uint8_t *raw, uint32_t len)
{
    return manifest_parse(raw, len, &g_manifest);
}

bool manifest_parse(const uint8_t *raw, uint32_t len, ubb_manifest_t *m)
{
    if (len < sizeof(ubb_manifest_hdr_t)) return false;
    ubb_manifest_hdr_t h;
    memcpy(&h, raw, sizeof h);
    if (h.magic != UBB_MANIFEST_MAGIC || h.version != UBB_MANIFEST_VERSION) return false;
    if (h.header_len != sizeof h || h.total_len > len) return false;
    if (h.n_entries > UBB_MAX_ENTRIES || h.n_regions > UBB_MAX_REGIONS || h.n_triggers > UBB_MAX_TRIGGERS) return false;
    uint32_t need = sizeof h + h.n_entries * sizeof(ubb_entry_t) + h.n_regions * sizeof(ubb_region_t)
                  + h.n_triggers * sizeof(ubb_trigger_t);
    if (need > h.total_len) return false;

    // CRC over the whole manifest with the crc field zeroed
    uint8_t tmp[sizeof h];
    memcpy(tmp, raw, sizeof h);
    memset(tmp + 12, 0, 4);   // crc field offset: magic(4)+version(2)+header_len(2)+total_len(4)
    uint32_t c = crc32_update(0, tmp, sizeof h);
    c = crc32_update(c, raw + sizeof h, h.total_len - sizeof h);
    if (c != h.crc) return false;

    memset(m, 0, sizeof *m);
    m->hdr = h;
    const uint8_t *p = raw + sizeof h;
    memcpy(m->entries, p, h.n_entries * sizeof(ubb_entry_t));   p += h.n_entries * sizeof(ubb_entry_t);
    memcpy(m->regions, p, h.n_regions * sizeof(ubb_region_t));  p += h.n_regions * sizeof(ubb_region_t);
    memcpy(m->triggers, p, h.n_triggers * sizeof(ubb_trigger_t));
    m->n_entries = h.n_entries;
    m->n_regions = h.n_regions;
    m->n_triggers = h.n_triggers;
    for (uint16_t i = 0; i < m->n_entries; i++) {
        uint8_t s = m->entries[i].size;
        if (s != 1 && s != 2 && s != 4 && s != 8) return false;
        m->entries[i].id = i;
    }
    manifest_profile_for_target(h.target, &m->profile);
    build_runs(m);
    m->hash = crc32_buf(raw, h.total_len);
    if (m->hdr.fast_period_us == 0) m->hdr.fast_period_us = UBB_DEFAULT_FAST_PERIOD_US;
    if (m->hdr.slow_period_us == 0) m->hdr.slow_period_us = UBB_DEFAULT_SLOW_PERIOD_US;
    if (m->hdr.keyframe_us == 0) m->hdr.keyframe_us = UBB_DEFAULT_KEYFRAME_US;
    if (m->hdr.swd_hz == 0) m->hdr.swd_hz = UBB_SWD_DEFAULT_HZ;
    m->valid = true;
    return true;
}

void manifest_load_default(void)
{
    ubb_manifest_t *m = &g_manifest;
    memset(m, 0, sizeof *m);
    m->hdr.magic = UBB_MANIFEST_MAGIC;
    m->hdr.version = UBB_MANIFEST_VERSION;
    m->hdr.target = UBB_TGT_RP2040;
    m->hdr.level = UBB_LEVEL_ARMED;
    m->hdr.on_fault = UBB_ONFAULT_RESUME;
    m->hdr.pc_sampling = 10;
    m->hdr.swd_hz = UBB_SWD_DEFAULT_HZ;
    m->hdr.fast_period_us = UBB_DEFAULT_FAST_PERIOD_US;
    m->hdr.slow_period_us = UBB_DEFAULT_SLOW_PERIOD_US;
    m->hdr.keyframe_us = UBB_DEFAULT_KEYFRAME_US;
    m->hdr.snapshot_on_fault = 1;
    manifest_profile_for_target(UBB_TGT_RP2040, &m->profile);
    // With no ELF we can still record core state; snapshot the two default pico-sdk stacks
    // (core 0 in scratch Y 0x20041000, core 1 in scratch X 0x20040000) on a fault.
    m->regions[0].addr = 0x20040000u;
    m->regions[0].len = 0x2000u;
    m->n_regions = 1;
    m->hash = 0;
    m->valid = false;   // "no manifest uploaded" is reported to the host
    build_runs(m);
}
