// Universal Black Box - USB host protocol + flash writer loop (core 1).
//
// Frame: 'U' 'B' cmd:u8 len:u16 payload[len] crc16:u16 (CRC-16/CCITT-FALSE over cmd..payload)
#include "host.h"
#include "config.h"
#include "ubb_format.h"
#include "crc.h"
#include "stream.h"
#include "flashlog.h"
#include "sampler.h"
#include "manifest.h"
#include "swd.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/stdio/driver.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

extern uint32_t g_session_id;

#define MAX_PAYLOAD 4608

static uint8_t  rx_payload[MAX_PAYLOAD];
static uint8_t  tx_buf[MAX_PAYLOAD + 8];
static uint8_t  sector_buf[UBB_BLOCK_SIZE];
static uint8_t  manifest_buf[UBB_MANIFEST_SLOT_SIZE];
static uint32_t manifest_total, manifest_received;

static void send_frame(uint8_t type, const void *payload, uint32_t len)
{
    tx_buf[0] = UBB_PROTO_SYNC0;
    tx_buf[1] = UBB_PROTO_SYNC1;
    tx_buf[2] = type;
    tx_buf[3] = (uint8_t)len;
    tx_buf[4] = (uint8_t)(len >> 8);
    if (len) memcpy(tx_buf + 5, payload, len);
    uint16_t c = crc16_ccitt(tx_buf + 2, 3 + len);
    tx_buf[5 + len] = (uint8_t)c;
    tx_buf[6 + len] = (uint8_t)(c >> 8);
    // Straight to the USB CDC driver: no newlib buffering, no CRLF translation.
    uint32_t total = 7 + len, sent = 0;
    while (sent < total) {
        uint32_t n = total - sent > 256 ? 256 : total - sent;
        stdio_usb.out_chars((const char *)tx_buf + sent, (int)n);
        sent += n;
    }
}

static void send_ok(void) { send_frame(UBB_RSP_OK, NULL, 0); }

static void send_err(uint8_t code, const char *msg)
{
    uint8_t b[64];
    b[0] = code;
    size_t n = strlen(msg);
    if (n > 62) n = 62;
    memcpy(b + 1, msg, n);
    send_frame(UBB_RSP_ERR, b, (uint32_t)(1 + n));
}

static void handle_cmd(uint8_t cmd, const uint8_t *p, uint32_t len)
{
    switch (cmd) {
    case UBB_CMD_PING: {
        ubb_info_rsp_t i;
        memset(&i, 0, sizeof i);
        i.fw_major = UBB_FW_VERSION_MAJOR;
        i.fw_minor = UBB_FW_VERSION_MINOR;
        i.session_id = g_session_id;
        i.manifest_hash = g_manifest.hash;
        i.manifest_valid = g_manifest.valid;
        i.target_type = g_manifest.hdr.target;
        i.log_offset = UBB_LOG_OFFSET;
        i.log_sectors = UBB_LOG_SECTORS;
        i.block_size = UBB_BLOCK_SIZE;
        i.sector_size = 4096;
        send_frame(UBB_RSP_INFO, &i, sizeof i);
        break;
    }
    case UBB_CMD_STATUS: {
        ubb_status_rsp_t s;
        const flashlog_state_t *fl = flashlog_state();
        memset(&s, 0, sizeof s);
        s.recording = g_sampler.recording;
        s.link_state = g_sampler.link_state;
        s.level = g_manifest.hdr.level;
        s.event_latched = g_sampler.event_latched;
        s.dpidr = g_sampler.dpidr;
        s.last_dhcsr = g_sampler.last_dhcsr;
        s.frames = g_sampler.frames;
        s.words_per_s = g_sampler.words_per_s;
        s.wait_acks = swd_stats.wait_acks;
        s.fault_acks = swd_stats.fault_acks;
        s.overruns = stream_overruns() + stream_dropped_records();
        s.blocks_written = fl->blocks_written;
        s.next_seq = stream_next_seq();
        s.ram_blocks_used = stream_blocks_staged();
        s.faults_captured = g_sampler.faults_captured;
        s.resets_seen = g_sampler.resets_seen;
        s.uptime_us = time_us_64();
        s.first_valid_sector = fl->first_valid_sector;
        s.valid_sectors = fl->valid_sectors;
        send_frame(UBB_RSP_DATA, &s, sizeof s);
        break;
    }
    case UBB_CMD_READ_SECTOR: {
        if (len < 4) { send_err(1, "need index"); break; }
        uint32_t idx; memcpy(&idx, p, 4);
        if (!flashlog_read_sector(idx, sector_buf)) { send_err(2, "bad index"); break; }
        send_frame(UBB_RSP_DATA, sector_buf, UBB_BLOCK_SIZE);
        break;
    }
    case UBB_CMD_FLUSH:
        sampler_request_flush();
        sleep_ms(5);
        // drain staged blocks before answering so the host sees everything
        for (int i = 0; i < 100; i++) {
            const uint8_t *b = stream_peek_block();
            if (!b) break;
            flashlog_write_block(b);
            stream_release_block();
        }
        send_ok();
        break;
    case UBB_CMD_ERASE_LOG:
        sampler_request_pause();
        while (stream_peek_block()) stream_release_block();
        flashlog_erase_all();
        sampler_resume_after_pause();
        send_ok();
        break;
    case UBB_CMD_MANIFEST_BEGIN:
        if (len < 4) { send_err(1, "need len"); break; }
        memcpy(&manifest_total, p, 4);
        if (manifest_total == 0 || manifest_total > sizeof manifest_buf - 16) { send_err(3, "too big"); manifest_total = 0; break; }
        manifest_received = 0;
        send_ok();
        break;
    case UBB_CMD_MANIFEST_DATA: {
        if (len < 4) { send_err(1, "need offset"); break; }
        uint32_t off; memcpy(&off, p, 4);
        uint32_t n = len - 4;
        if (!manifest_total || off + n > manifest_total) { send_err(4, "out of range"); break; }
        memcpy(manifest_buf + off, p + 4, n);
        manifest_received += n;
        send_ok();
        break;
    }
    case UBB_CMD_MANIFEST_END: {
        if (!manifest_total || manifest_received < manifest_total) { send_err(5, "incomplete"); break; }
        // validate before touching flash
        static ubb_manifest_t check;
        if (!manifest_parse(manifest_buf, manifest_total, &check)) { send_err(6, "invalid manifest"); break; }
        sampler_request_pause();
        bool w = flashlog_manifest_write(manifest_buf, manifest_total);
        sampler_request_reload();
        sampler_resume_after_pause();
        manifest_total = 0;
        if (w) send_ok(); else send_err(7, "flash write failed");
        break;
    }
    case UBB_CMD_READ_MANIFEST: {
        if (len < 6) { send_err(1, "need offset,len"); break; }
        uint32_t off; uint16_t n;
        memcpy(&off, p, 4); memcpy(&n, p + 4, 2);
        const uint8_t *mp; uint32_t ml;
        if (!flashlog_manifest_read(&mp, &ml)) { send_err(8, "no manifest"); break; }
        if (off >= ml) { send_frame(UBB_RSP_DATA, NULL, 0); break; }
        if (off + n > ml) n = (uint16_t)(ml - off);
        if (n > 4096) n = 4096;
        send_frame(UBB_RSP_DATA, mp + off, n);
        break;
    }
    case UBB_CMD_MARK: {
        uint32_t id = 0; uint8_t snap = 0;
        if (len >= 4) memcpy(&id, p, 4);
        if (len >= 5) snap = p[4];
        sampler_request_mark(id, snap != 0);
        send_ok();
        break;
    }
    case UBB_CMD_REC_START: sampler_request_start(); send_ok(); break;
    case UBB_CMD_REC_STOP:  sampler_request_stop();  send_ok(); break;
    case UBB_CMD_CLEAR_EVENT: sampler_clear_event(); send_ok(); break;
    default:
        send_err(0xFF, "unknown command");
    }
}

// ---- receive state machine ----------------------------------------------------------
static void poll_host(void)
{
    static int state = 0;
    static uint8_t cmd;
    static uint32_t len, got;
    static uint16_t crc_rx;
    for (int budget = 0; budget < 8192; budget++) {
        int c = getchar_timeout_us(0);
        if (c < 0) return;
        uint8_t b = (uint8_t)c;
        switch (state) {
        case 0: state = (b == UBB_PROTO_SYNC0) ? 1 : 0; break;
        case 1: state = (b == UBB_PROTO_SYNC1) ? 2 : (b == UBB_PROTO_SYNC0 ? 1 : 0); break;
        case 2: cmd = b; state = 3; break;
        case 3: len = b; state = 4; break;
        case 4: len |= (uint32_t)b << 8; got = 0; state = (len > MAX_PAYLOAD) ? 0 : (len ? 5 : 6); break;
        case 5: rx_payload[got++] = b; if (got == len) state = 6; break;
        case 6: crc_rx = b; state = 7; break;
        case 7: {
            crc_rx |= (uint16_t)b << 8;
            uint8_t hdr[3] = { cmd, (uint8_t)len, (uint8_t)(len >> 8) };
            uint16_t c = crc16_ccitt(hdr, 3);
            // continue the CRC over the payload
            uint16_t crc = c;
            for (uint32_t i = 0; i < len; i++) {
                crc ^= (uint16_t)rx_payload[i] << 8;
                for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
            }
            state = 0;
            if (crc == crc_rx) handle_cmd(cmd, rx_payload, len);
            else send_err(0xFE, "bad crc");
            break;
        }
        }
    }
}

void host_main(void)
{
    // USB is initialised on core 0 (main.c): the SDK's USB timer/IRQ plumbing must live on one core.
    for (;;) {
        const uint8_t *b = stream_peek_block();
        if (b) {
            flashlog_write_block(b);
            stream_release_block();
        }
        poll_host();
        if (!b) sleep_us(500);
    }
}
