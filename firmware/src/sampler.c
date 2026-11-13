// Universal Black Box - the recorder loop (core 0).
//
// This is a flight recorder, not a debugger.  The loop only *reads* the
// target while it runs, writes change-only records into the stream, and
// halts the core only to take a post-mortem picture of a fault, after which
// the core is released again (or held, if the manifest says so).
#include "sampler.h"
#include "config.h"
#include "ubb_format.h"
#include "manifest.h"
#include "stream.h"
#include "swd.h"
#include "dap.h"
#include "leds.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include <string.h>

sampler_status_t g_sampler;

// ---- requests from core 1 --------------------------------------------------
static volatile bool req_pause, parked, req_reload, req_start, req_stop, req_flush, req_clear_event;
static volatile bool req_mark;
static volatile uint32_t mark_id;
static volatile bool mark_snapshot;

void sampler_request_pause(void)
{
    req_pause = true;
    while (!parked) tight_loop_contents();
}
void sampler_resume_after_pause(void) { req_pause = false; while (parked) tight_loop_contents(); }
void sampler_request_reload(void) { req_reload = true; }
void sampler_request_mark(uint32_t id, bool with_snapshot) { mark_id = id; mark_snapshot = with_snapshot; req_mark = true; }
void sampler_request_start(void) { req_start = true; }
void sampler_request_stop(void) { req_stop = true; }
void sampler_request_flush(void) { req_flush = true; }
void sampler_clear_event(void) { req_clear_event = true; }

// ---- local state -----------------------------------------------------------
static uint8_t  last_vals[UBB_MAX_ENTRIES * 8];   // last seen raw bytes, entry i at offset off[i]
static uint16_t val_off[UBB_MAX_ENTRIES];
static bool     have_last;
static uint32_t runbuf[64];
static uint8_t  keyframe_buf[UBB_MAX_ENTRIES * 8];
static uint8_t  snapbuf[UBB_SNAPSHOT_CHUNK];
static uint8_t  trig_fired[UBB_MAX_TRIGGERS];

static bool     attached;
static bool     held;                 // core intentionally left halted
static bool     lockup_reported;
static bool     armed;
static uint32_t pc_div_counter;
static uint32_t last_core1_dhcsr;
static uint64_t next_fast, next_slow, next_core1, next_keyframe, next_linkstat, next_reconnect, next_ttime;
static uint64_t stat_window_start;
static uint32_t last_transfers;
static uint32_t dropped_since_note;
static uint64_t last_fault_resume_us;   // when we last released the core after a fault capture
static uint32_t captures_since_reset;   // fault captures since the target last reset

#define LVL   (g_manifest.hdr.level)
#define M     (&g_manifest)

// ---- record helpers ---------------------------------------------------------
static void put_or_drop(uint8_t tag, const void *p, uint32_t len);

static void ensure_space(uint32_t need)
{
    // Keep a keyframe's worth of room: a block that starts without one is
    // still decodable, but every block that *can* start with one should.
    if (stream_remaining() < need) stream_close_block();
}

static void write_keyframe(void)
{
    if (!have_last) return;
    uint32_t n = 0;
    for (uint16_t i = 0; i < M->n_entries; i++) {
        memcpy(keyframe_buf + n, last_vals + val_off[i], M->entries[i].size);
        n += M->entries[i].size;
    }
    ensure_space(n + 16);
    if (stream_put(UBB_REC_KEYFRAME, keyframe_buf, n)) stream_mark_keyframe();
    else stream_note_drop(1);
}

static void put_or_drop(uint8_t tag, const void *p, uint32_t len)
{
    if (stream_put(tag, p, len)) return;
    // did not fit: close, start a fresh block with a keyframe, retry once
    stream_close_block();
    write_keyframe();
    if (!stream_put(tag, p, len)) stream_note_drop(1);
}

static void rec_status(uint8_t code, uint32_t arg)
{
    uint8_t b[5];
    b[0] = code;
    memcpy(b + 1, &arg, 4);
    put_or_drop(UBB_REC_STATUS, b, 5);
}

static void rec_u32(uint8_t tag, uint32_t v) { put_or_drop(tag, &v, 4); }

static void rec_session(void)
{
    uint8_t b[13];
    uint32_t fw = (UBB_FW_VERSION_MAJOR << 8) | UBB_FW_VERSION_MINOR;
    uint32_t sid = 0, mh = M->hash;
    extern uint32_t g_session_id;
    sid = g_session_id;
    memcpy(b, &fw, 4); memcpy(b + 4, &sid, 4); memcpy(b + 8, &mh, 4);
    b[12] = LVL;
    put_or_drop(UBB_REC_SESSION, b, 13);
}

static void rec_linkstat(uint64_t now)
{
    uint32_t b[5];
    uint64_t dt = now - stat_window_start;
    uint32_t wps = dt ? (uint32_t)((uint64_t)(swd_stats.transfers - last_transfers) * 1000000ull / dt) : 0;
    last_transfers = swd_stats.transfers;
    stat_window_start = now;
    g_sampler.words_per_s = wps;
    b[0] = wps;
    b[1] = swd_stats.wait_acks;
    b[2] = swd_stats.fault_acks;
    b[3] = g_sampler.frames;
    b[4] = stream_overruns() + stream_dropped_records();
    put_or_drop(UBB_REC_LINKSTAT, b, 20);
}

// ---- snapshot ----------------------------------------------------------------
static uint32_t snapshot_regions(void)
{
    uint32_t total = 0;
    led_event(true);
    for (uint16_t r = 0; r < M->n_regions; r++) {
        uint32_t addr = M->regions[r].addr & ~3u;
        uint32_t len = (M->regions[r].len + 3u) & ~3u;
        while (len) {
            uint32_t n = len < UBB_SNAPSHOT_CHUNK ? len : UBB_SNAPSHOT_CHUNK;
            if (dap_mem_read_block(addr, (uint32_t *)snapbuf, n / 4u) != DAP_OK) return total;
            uint8_t h[6];
            uint16_t l16 = (uint16_t)n;
            memcpy(h, &addr, 4); memcpy(h + 4, &l16, 2);
            if (!stream_put2(UBB_REC_SNAPSHOT, h, 6, snapbuf, n)) {
                stream_close_block();
                if (!stream_put2(UBB_REC_SNAPSHOT, h, 6, snapbuf, n)) stream_note_drop(1);
            }
            addr += n; len -= n; total += n;
        }
    }
    rec_status(UBB_ST_SNAPSHOT_DONE, total);
    return total;
}

// ---- fault capture --------------------------------------------------------------
// Takes the post-mortem picture.  `already_halted` tells whether the core
// stopped by itself (vector catch / lockup) or we must stop it for a mark.
static void capture_fault(uint8_t reason, bool already_halted, bool with_snapshot)
{
    uint64_t t0 = time_us_64();
    bool we_halted = false;
    uint32_t dhcsr = 0, dfsr = 0, icsr = 0;
    uint32_t regs[17];
    uint32_t frame[8];
    memset(regs, 0, sizeof regs);
    memset(frame, 0, sizeof frame);

    if (!already_halted) {
        if (LVL < UBB_LEVEL_ARMED) {
            // Passive level: no halt allowed. Record what we can and the snapshot.
            if (with_snapshot) snapshot_regions();
            return;
        }
        if (dap_core_halt() != DAP_OK) return;
        we_halted = true;
    }
    dap_mem_read32(SCS_DHCSR, &dhcsr);
    dap_mem_read32(SCS_DFSR, &dfsr);
    dap_mem_read32(SCS_ICSR, &icsr);
    if (dap_core_regs(regs) == DAP_OK) {
        uint32_t sp = regs[13];
        if (sp >= M->profile.sram_base && sp + 32 <= M->profile.sram_base + M->profile.sram_size)
            dap_mem_read_block(sp, frame, 8);
    }
    uint8_t b[2 + 12 + 17 * 4 + 8 * 4];
    b[0] = (uint8_t)dap_current_core();
    b[1] = reason;
    memcpy(b + 2, &dhcsr, 4); memcpy(b + 6, &dfsr, 4); memcpy(b + 10, &icsr, 4);
    memcpy(b + 14, regs, sizeof regs);
    memcpy(b + 14 + sizeof regs, frame, sizeof frame);
    ensure_space(sizeof b + 16);
    put_or_drop(UBB_REC_FAULT, b, sizeof b);
    if (dfsr) dap_mem_write32(SCS_DFSR, dfsr);   // write-one-to-clear, so the next event is fresh

    if (with_snapshot) snapshot_regions();

    g_sampler.faults_captured++;
    g_sampler.event_latched = 1;
    led_event(true);

    bool hold = (M->hdr.on_fault == UBB_ONFAULT_HOLD) && reason != UBB_FAULT_MARK && reason != UBB_FAULT_TRIGGER;
    if (hold || held) {   // a core that was already held stays held
        held = true;
        g_sampler.link_state = 2;
        rec_status(UBB_ST_HELD, dhcsr);
    } else {
        dap_core_resume();
        last_fault_resume_us = time_us_64();
        rec_status(UBB_ST_RESUMED, (uint32_t)(last_fault_resume_us - t0));
    }
    (void)we_halted;
}

// Give the target its natural behaviour back: no C_DEBUGEN means BKPT and HardFault
// escalate normally (lockup -> watchdog reset) instead of freezing the core and the
// watchdog.  Fault catch is re-armed when the next reset is seen.
static void disarm_debug(void)
{
    uint32_t d = 0;
    for (int i = 0; i < 3; i++) {
        dap_mem_write32(SCS_DHCSR, DHCSR_KEY);   // C_DEBUGEN = 0, C_HALT = 0
        if (dap_mem_read32(SCS_DHCSR, &d) != DAP_OK || !(d & DHCSR_S_HALT)) break;
    }
    armed = false;
    rec_status(UBB_ST_DISARMED, d);
}

// ---- arming ----------------------------------------------------------------------
static void arm_debug(void)
{
    armed = false;
    if (LVL < UBB_LEVEL_ARMED) return;
    uint32_t demcr = DEMCR_VC_HARDERR | (M->hdr.pc_sampling ? DEMCR_DWTENA : 0);
    if (dap_mem_write32(SCS_DEMCR, demcr) != DAP_OK) return;
    if (dap_mem_write32(SCS_DHCSR, DHCSR_KEY | DHCSR_C_DEBUGEN) != DAP_OK) return;
    armed = true;
    rec_status(UBB_ST_ARMED, demcr);
}

static void rec_reset_cause(void)
{
    uint32_t b[2] = { 0, 0 };
    dap_mem_read32(M->profile.chip_reset_addr, &b[0]);
    dap_mem_read32(M->profile.wdog_reason_addr, &b[1]);
    put_or_drop(UBB_REC_RESET_CAUSE, b, 8);
}

// ---- connection --------------------------------------------------------------------
static void on_lost(uint32_t arg)
{
    if (attached) rec_status(UBB_ST_LOST, arg);
    attached = false;
    held = false;
    armed = false;
    g_sampler.link_state = 0;
    led_run(false);
    next_reconnect = time_us_64() + UBB_RECONNECT_US;
}

static bool try_connect(void)
{
    uint32_t dpidr = 0;
    int r = dap_connect(&dpidr);
    if (r != DAP_OK) return false;
    attached = true;
    held = false;
    lockup_reported = false;
    have_last = false;
    g_sampler.dpidr = dpidr;
    g_sampler.link_state = 1;
    led_run(true);
    rec_session();
    rec_status(UBB_ST_ATTACHED, dpidr);
    rec_reset_cause();
    // Reading DHCSR once clears sticky S_RESET_ST left over from before we attached.
    uint32_t d;
    if (dap_mem_read32(SCS_DHCSR, &d) == DAP_OK) g_sampler.last_dhcsr = d;
    arm_debug();
    uint64_t now = time_us_64();
    next_fast = next_slow = next_core1 = now;
    next_keyframe = now;           // first frame writes a keyframe
    next_linkstat = now + M->hdr.keyframe_us;
    next_ttime = now;
    stat_window_start = now;
    last_transfers = swd_stats.transfers;
    memset(trig_fired, 0, sizeof trig_fired);
    return true;
}

// ---- triggers -----------------------------------------------------------------------
static uint32_t entry_u32(uint16_t i)
{
    uint32_t v = 0;
    memcpy(&v, last_vals + val_off[i], M->entries[i].size > 4 ? 4 : M->entries[i].size);
    return v;
}

static void check_triggers(uint16_t id, uint32_t oldv, uint32_t newv)
{
    for (uint16_t t = 0; t < M->n_triggers; t++) {
        const ubb_trigger_t *tr = &M->triggers[t];
        if (tr->id != id) continue;
        if ((tr->flags & UBB_TRIGF_ONCE) && trig_fired[t]) continue;
        bool fire = false;
        switch (tr->op) {
        case UBB_TRIG_EQ:     fire = newv == tr->value && oldv != tr->value; break;
        case UBB_TRIG_NE:     fire = newv != tr->value && oldv == tr->value; break;
        case UBB_TRIG_GT:     fire = (int32_t)newv > (int32_t)tr->value && (int32_t)oldv <= (int32_t)tr->value; break;
        case UBB_TRIG_LT:     fire = (int32_t)newv < (int32_t)tr->value && (int32_t)oldv >= (int32_t)tr->value; break;
        case UBB_TRIG_CHANGE: fire = newv != oldv; break;
        case UBB_TRIG_BITSET: fire = (newv & tr->value) && !(oldv & tr->value); break;
        }
        if (!fire) continue;
        trig_fired[t] = 1;
        uint8_t b[1 + 5 + 8];
        b[0] = (uint8_t)t;
        uint32_t n = 1 + ubb_varint_encode(b + 1, id);
        memcpy(b + n, last_vals + val_off[id], M->entries[id].size);
        n += M->entries[id].size;
        put_or_drop(UBB_REC_TRIGGER, b, n);
        g_sampler.event_latched = 1;
        led_event(true);
        if (tr->flags & UBB_TRIGF_SNAPSHOT) capture_fault(UBB_FAULT_TRIGGER, false, true);
    }
}

// ---- frames ----------------------------------------------------------------------------
// Reads all runs of one class; emits CHANGE records (or fills last_vals silently when first).
static int sample_class(uint8_t cls, bool first)
{
    for (uint16_t r = 0; r < M->n_runs; r++) {
        const ubb_run_t *run = &M->runs[r];
        if (run->cls != cls) continue;
        int rc = dap_mem_read_block(run->addr, runbuf, run->nwords);
        if (rc != DAP_OK) return rc;
        for (uint16_t k = 0; k < run->count; k++) {
            uint16_t i = run->first + k;
            const ubb_entry_t *e = &M->entries[i];
            const uint8_t *src = (const uint8_t *)runbuf + (e->addr - run->addr);
            uint8_t *dst = last_vals + val_off[i];
            if (first || memcmp(src, dst, e->size) != 0) {
                uint32_t oldv = first ? 0 : entry_u32(i);
                memcpy(dst, src, e->size);
                if (!first) {
                    uint8_t b[5 + 8];
                    uint32_t n = ubb_varint_encode(b, i);
                    memcpy(b + n, src, e->size);
                    put_or_drop(UBB_REC_CHANGE, b, n + e->size);
                    if (M->n_triggers) check_triggers(i, oldv, entry_u32(i));
                }
            }
        }
    }
    return DAP_OK;
}

static void handle_dhcsr(uint32_t d)
{
    g_sampler.last_dhcsr = d;
    if (d & DHCSR_S_RESET_ST) {
        g_sampler.resets_seen++;
        rec_status(UBB_ST_RESET_SEEN, d);
        rec_reset_cause();
        lockup_reported = false;
        held = false;
        captures_since_reset = 0;
        last_fault_resume_us = 0;
        arm_debug();   // a reset clears the debug configuration
        if (M->hdr.snapshot_on_reset) { snapshot_regions(); g_sampler.event_latched = 1; }
        have_last = false;   // force a fresh keyframe
    }
    if (d & DHCSR_S_LOCKUP) {
        if (!lockup_reported) {
            lockup_reported = true;
            rec_status(UBB_ST_LOCKUP, d);
            capture_fault(UBB_FAULT_LOCKUP, false, M->hdr.snapshot_on_fault);
        }
        return;
    }
    lockup_reported = false;
    if ((d & DHCSR_S_HALT) && !held) {
        // The core stopped on its own: vector catch (HardFault) or a BKPT.
        uint64_t now = time_us_64();
        if (captures_since_reset >= 4 ||
            (last_fault_resume_us && now - last_fault_resume_us < 500000)) {
            // Halted again right after we released it (e.g. the fault handler is a BKPT loop),
            // or too many captures since the last reset: the picture is already taken.
            rec_status(UBB_ST_STUCK, d);
            disarm_debug();
            return;
        }
        captures_since_reset++;
        uint32_t dfsr = 0;
        dap_mem_read32(SCS_DFSR, &dfsr);
        uint8_t reason = (dfsr & (1u << 3)) ? UBB_FAULT_VC_HARDERR : UBB_FAULT_BKPT;
        rec_status(UBB_ST_HALTED, d);
        capture_fault(reason, true, M->hdr.snapshot_on_fault);
    }
}

static void fast_frame(uint64_t now)
{
    bool first = !have_last;
    uint32_t d;
    if (dap_mem_read32(SCS_DHCSR, &d) != DAP_OK) { on_lost(1); return; }
    handle_dhcsr(d);
    if (held) return;

    if (M->hdr.pc_sampling && armed) {
        if (++pc_div_counter >= M->hdr.pc_sampling) {
            pc_div_counter = 0;
            uint32_t pc;
            if (dap_mem_read32(DWT_PCSR, &pc) == DAP_OK && pc != 0xFFFFFFFFu) rec_u32(UBB_REC_PCSAMPLE, pc);
        }
    }
    if (now >= next_ttime) {
        uint32_t tt;
        if (dap_mem_read32(M->profile.timerawl_addr, &tt) == DAP_OK) rec_u32(UBB_REC_TARGET_TIME, tt);
        next_ttime = now + 100000;
    }
    if (first) {
        if (sample_class(UBB_CLASS_FAST, true) != DAP_OK || sample_class(UBB_CLASS_SLOW, true) != DAP_OK) { on_lost(2); return; }
        have_last = true;
        write_keyframe();
        next_keyframe = now + M->hdr.keyframe_us;
        next_slow = now + M->hdr.slow_period_us;
        return;
    }
    if (sample_class(UBB_CLASS_FAST, false) != DAP_OK) { on_lost(2); return; }
    g_sampler.frames++;
}

static void slow_frame(void)
{
    if (held || !have_last) return;
    if (sample_class(UBB_CLASS_SLOW, false) != DAP_OK) on_lost(3);
}

static void core1_poll(void)
{
    if (M->profile.targetsel[1] == 0 && !M->profile.adiv6) return;
    if (dap_select_core(1) != DAP_OK) { dap_select_core(0); return; }
    uint32_t d = 0;
    int rc = dap_mem_read32(SCS_DHCSR, &d);
    dap_select_core(0);
    if (rc != DAP_OK) return;
    uint32_t mask = DHCSR_S_HALT | DHCSR_S_SLEEP | DHCSR_S_LOCKUP | DHCSR_S_RESET_ST;
    if ((d & mask) != (last_core1_dhcsr & mask)) {
        last_core1_dhcsr = d;
        rec_u32(UBB_REC_CORE1, d);
    }
}

// ---- main loop ------------------------------------------------------------------------------
static void load_manifest_from_flash(void)
{
    extern bool flashlog_manifest_read(const uint8_t **ptr_out, uint32_t *len_out);
    const uint8_t *p; uint32_t len;
    bool ok = false;
    if (flashlog_manifest_read(&p, &len)) ok = manifest_load(p, len);
    if (!ok) manifest_load_default();
    stream_set_manifest_hash(g_manifest.hash);
    uint32_t off = 0;
    for (uint16_t i = 0; i < g_manifest.n_entries; i++) { val_off[i] = (uint16_t)off; off += g_manifest.entries[i].size; }
    have_last = false;
    swd_set_clock(g_manifest.hdr.swd_hz);
}

static void park_if_requested(void)
{
    if (!req_pause) return;
    stream_close_block();
    parked = true;
    while (req_pause) tight_loop_contents();
    if (req_reload) {
        req_reload = false;
        load_manifest_from_flash();
        if (attached) { rec_session(); arm_debug(); }
    }
    parked = false;
}

static void blink_run(uint64_t now)
{
    led_run(((now / 250000) & 1) == 0);
}

void sampler_main(void)
{
    load_manifest_from_flash();
    swd_init(g_manifest.hdr.swd_hz);
    dap_set_profile(&g_manifest.profile);
    g_sampler.recording = 1;
    rec_status(UBB_ST_REC_START, 0);
    next_reconnect = 0;
    if (UBB_PIN_TRIGGER >= 0) { gpio_init(UBB_PIN_TRIGGER); gpio_set_dir(UBB_PIN_TRIGGER, GPIO_IN); gpio_pull_up(UBB_PIN_TRIGGER); }
    uint64_t btn_last = 0;

    for (;;) {
        uint64_t now = time_us_64();
        park_if_requested();

        if (req_clear_event) { req_clear_event = false; g_sampler.event_latched = 0; led_event(false); }
        if (req_stop && g_sampler.recording) { req_stop = false; g_sampler.recording = 0; rec_status(UBB_ST_REC_STOP, 0); stream_close_block(); }
        if (req_start && !g_sampler.recording) { req_start = false; g_sampler.recording = 1; have_last = false; rec_status(UBB_ST_REC_START, 0); }
        if (req_flush) { req_flush = false; stream_close_block(); }
        if (!g_sampler.recording) { led_run(false); sleep_us(1000); continue; }

        if (!attached) {
            blink_run(now);
            if (now >= next_reconnect) {
                dap_set_profile(&g_manifest.profile);
                if (!try_connect()) next_reconnect = now + UBB_RECONNECT_US;
            } else {
                sleep_us(1000);
            }
            // keep partial blocks moving to flash even without a target
            if (stream_block_age_us(now) > 5000000ull) stream_close_block();
            continue;
        }

        if (UBB_PIN_TRIGGER >= 0 && !gpio_get(UBB_PIN_TRIGGER) && now - btn_last > 500000) {
            btn_last = now;
            req_mark = true; mark_id = 0xB0000000u; mark_snapshot = true;
        }
        if (req_mark) {
            req_mark = false;
            rec_u32(UBB_REC_MARK, mark_id);
            g_sampler.event_latched = 1;
            led_event(true);
            if (mark_snapshot) capture_fault(UBB_FAULT_MARK, held, true);
        }

        if (now >= next_fast) {
            next_fast += M->hdr.fast_period_us;
            if (next_fast < now) next_fast = now + M->hdr.fast_period_us;   // fell behind: resync
            fast_frame(now);
            if (!attached) continue;
        }
        if (now >= next_slow) {
            next_slow += M->hdr.slow_period_us;
            if (next_slow < now) next_slow = now + M->hdr.slow_period_us;
            slow_frame();
            if (!attached) continue;
        }
        if (now >= next_core1) {
            next_core1 = now + UBB_CORE1_POLL_US;
            core1_poll();
        }
        if (now >= next_keyframe && have_last) {
            next_keyframe = now + M->hdr.keyframe_us;
            write_keyframe();
        }
        if (now >= next_linkstat) {
            next_linkstat = now + UBB_DEFAULT_LINKSTAT_US;
            rec_linkstat(now);
            uint32_t dr = stream_dropped_records();
            if (dr != dropped_since_note) { rec_status(UBB_ST_OVERRUN, dr - dropped_since_note); dropped_since_note = dr; }
        }
        if (stream_block_age_us(now) > 5000000ull) stream_close_block();

        // Sleep until the next due time so the SWD link idles between frames.
        uint64_t next = next_fast;
        if (next_slow < next) next = next_slow;
        now = time_us_64();
        if (next > now + 20) sleep_us((uint32_t)(next - now - 10));
    }
}
