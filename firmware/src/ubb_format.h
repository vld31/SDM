// Universal Black Box - on-flash record format and manifest layout.
// Shared contract between the probe firmware and tools/ubb/format.py.
#pragma once
#include <stdint.h>

// ------------------------------------------------------------------------
// Flash block (one 4 KB sector = one block).  Little endian throughout.
// ------------------------------------------------------------------------
#define UBB_BLOCK_MAGIC   0x58424255u   // "UBBX"
#define UBB_BLOCK_HDR_LEN 32

enum ubb_block_type {
    UBB_BLK_STREAM = 1,   // continuous record stream
};

#define UBB_BLKF_HAS_KEYFRAME 0x01   // block starts with a keyframe
#define UBB_BLKF_AFTER_GAP    0x02   // records were dropped before this block

typedef struct __attribute__((packed)) {
    uint32_t magic;          // UBB_BLOCK_MAGIC
    uint32_t seq;            // monotonically increasing, never reused in one probe lifetime
    uint64_t ts_us;          // probe time (us since probe boot) of the first record
    uint8_t  type;           // enum ubb_block_type
    uint8_t  flags;          // UBB_BLKF_*
    uint16_t len;            // payload bytes used (<= 4096 - 32)
    uint32_t manifest_hash;  // CRC32 of the manifest in use
    uint32_t session_id;     // changes on every probe boot
    uint32_t crc;            // CRC32 over header (crc=0) + payload[len]
} ubb_block_hdr_t;

// ------------------------------------------------------------------------
// Records inside a block:  tag u8, varint dt_us (since previous record,
// first record relative to block ts_us), then tag-specific payload.
// Multi-byte integers are little endian; "varint" is LEB128 unsigned.
// ------------------------------------------------------------------------
enum ubb_rec_tag {
    UBB_REC_KEYFRAME    = 0x01,  // all manifest entries in manifest order, raw bytes
    UBB_REC_CHANGE      = 0x02,  // varint id, raw bytes (size from manifest)
    UBB_REC_PCSAMPLE    = 0x03,  // u32 pc (DWT_PCSR)
    UBB_REC_TARGET_TIME = 0x04,  // u32 target TIMERAWL at this probe time
    UBB_REC_STATUS      = 0x05,  // u8 status code, u32 arg
    UBB_REC_FAULT       = 0x06,  // u8 core, u8 reason, u32 dhcsr, u32 dfsr, u32 icsr, u32 regs[17] (r0-r12,sp,lr,pc,xpsr), u32 frame[8]
    UBB_REC_SNAPSHOT    = 0x07,  // u32 addr, u16 len, bytes
    UBB_REC_RESET_CAUSE = 0x08,  // u32 chip_reset, u32 watchdog_reason
    UBB_REC_MARK        = 0x09,  // u32 mark id (host or button trigger)
    UBB_REC_LINKSTAT    = 0x0A,  // u32 words/s, u32 wait acks, u32 fault acks, u32 frames, u32 overruns
    UBB_REC_SESSION     = 0x0B,  // u32 fw version, u32 session id, u32 manifest hash, u8 level
    UBB_REC_CORE1       = 0x0C,  // u32 dhcsr of core 1
    UBB_REC_TRIGGER     = 0x0D,  // u8 trigger index, varint id, raw bytes (value that fired it)
};

enum ubb_status_code {
    UBB_ST_ATTACHED      = 1,   // arg = DPIDR
    UBB_ST_LOST          = 2,   // arg = last ACK / error code
    UBB_ST_RESET_SEEN    = 3,   // S_RESET_ST observed; arg = DHCSR
    UBB_ST_LOCKUP        = 4,   // S_LOCKUP observed; arg = DHCSR
    UBB_ST_HALTED        = 5,   // S_HALT observed; arg = DHCSR
    UBB_ST_RESUMED       = 6,   // probe resumed the core; arg = halt duration us
    UBB_ST_SWD_ERROR     = 7,   // arg = CTRL/STAT
    UBB_ST_OVERRUN       = 8,   // arg = records dropped
    UBB_ST_ARMED         = 9,   // debug config written; arg = DEMCR
    UBB_ST_SNAPSHOT_DONE = 10,  // arg = bytes captured
    UBB_ST_REC_START     = 11,  // recording started (host command or boot)
    UBB_ST_REC_STOP      = 12,  // recording stopped by host
    UBB_ST_HELD          = 13,  // core left halted on purpose (hold policy)
    UBB_ST_SLEEPING      = 14,  // S_SLEEP observed (informational)
    UBB_ST_STUCK         = 15,  // core re-halted right after release (BKPT loop / handler loop); fault catch disarmed until reset
    UBB_ST_DISARMED      = 16,  // debug config cleared so the target behaves naturally again; arg = DHCSR
};

enum ubb_fault_reason {
    UBB_FAULT_VC_HARDERR = 1,   // vector catch on HardFault
    UBB_FAULT_LOCKUP     = 2,
    UBB_FAULT_BKPT       = 3,   // halted by BKPT/other debug event
    UBB_FAULT_MARK       = 4,   // host/button mark with snapshot
    UBB_FAULT_TRIGGER    = 5,   // value trigger
    UBB_FAULT_RESET      = 6,   // reset observed, snapshot after reset
};

// ------------------------------------------------------------------------
// Manifest (uploaded by the PC tool, stored in flash)
// ------------------------------------------------------------------------
#define UBB_MANIFEST_MAGIC   0x4D424255u   // "UBBM"
#define UBB_MANIFEST_VERSION 2

#define UBB_CLASS_FAST  0
#define UBB_CLASS_SLOW  1

#define UBB_LEVEL_PASSIVE 0   // L0: reads only
#define UBB_LEVEL_ARMED   1   // L0 + L1: DEMCR/DHCSR config writes, halt only on fault (L2)

#define UBB_ONFAULT_RESUME 0
#define UBB_ONFAULT_HOLD   1

#define UBB_TGT_RP2040 1
#define UBB_TGT_RP2350 2

#define UBB_TRIG_EQ 0
#define UBB_TRIG_NE 1
#define UBB_TRIG_GT 2
#define UBB_TRIG_LT 3
#define UBB_TRIG_CHANGE 4
#define UBB_TRIG_BITSET 5

#define UBB_TRIGF_SNAPSHOT 0x01   // capture snapshot regions when fired
#define UBB_TRIGF_ONCE     0x02   // fire only once per session

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint8_t  size;     // 1,2,4,8 (raw byte count stored)
    uint8_t  cls;      // UBB_CLASS_*
    uint16_t id;       // index in manifest order
} ubb_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint32_t len;
} ubb_region_t;

typedef struct __attribute__((packed)) {
    uint16_t id;        // entry to watch
    uint8_t  op;        // UBB_TRIG_*
    uint8_t  flags;     // UBB_TRIGF_*
    uint32_t value;
} ubb_trigger_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;      // sizeof(ubb_manifest_hdr_t)
    uint32_t total_len;       // header + tables
    uint32_t crc;             // CRC32 of everything with crc=0
    uint32_t elf_hash;        // CRC32 of the ELF file used (informational)
    uint8_t  target;          // UBB_TGT_*
    uint8_t  level;           // UBB_LEVEL_*
    uint8_t  on_fault;        // UBB_ONFAULT_*
    uint8_t  pc_sampling;     // 1 = read DWT_PCSR each fast frame
    uint32_t swd_hz;
    uint32_t fast_period_us;
    uint32_t slow_period_us;
    uint32_t keyframe_us;
    uint32_t snapshot_on_fault;  // 1 = capture regions on fault/lockup
    uint32_t snapshot_on_reset;  // 1 = capture regions when a reset is seen (after it)
    uint16_t n_entries;
    uint16_t n_regions;
    uint16_t n_triggers;
    uint16_t reserved;
    // followed by ubb_entry_t[n_entries], ubb_region_t[n_regions], ubb_trigger_t[n_triggers]
} ubb_manifest_hdr_t;

// ------------------------------------------------------------------------
// USB host protocol (framed over the CDC serial port)
//   frame: 'U' 'B' cmd:u8 len:u16 payload[len] crc16:u16   (CRC-16/CCITT-FALSE over cmd..payload)
// ------------------------------------------------------------------------
#define UBB_PROTO_SYNC0 'U'
#define UBB_PROTO_SYNC1 'B'

enum ubb_cmd {
    UBB_CMD_PING          = 0x01,  // -> UBB_RSP_INFO
    UBB_CMD_STATUS        = 0x02,  // -> ubb_status_rsp_t
    UBB_CMD_READ_SECTOR   = 0x03,  // u32 index -> 4096 bytes (raw) or error
    UBB_CMD_ERASE_LOG     = 0x04,  // erases the whole log ring (slow)
    UBB_CMD_MANIFEST_BEGIN= 0x05,  // u32 total_len
    UBB_CMD_MANIFEST_DATA = 0x06,  // u32 offset, bytes
    UBB_CMD_MANIFEST_END  = 0x07,  // commits to flash, reloads, restarts recording
    UBB_CMD_MARK          = 0x08,  // u32 id, u8 with_snapshot
    UBB_CMD_REC_START     = 0x09,
    UBB_CMD_REC_STOP      = 0x0A,
    UBB_CMD_CLEAR_EVENT   = 0x0B,  // turn off the event LED
    UBB_CMD_READ_MANIFEST = 0x0C,  // u32 offset, u16 len -> bytes of the active manifest
    UBB_CMD_FLUSH         = 0x0D,  // force the current partial block to flash
};

enum ubb_rsp {
    UBB_RSP_OK    = 0x80,
    UBB_RSP_ERR   = 0x81,  // u8 error code, char[] text
    UBB_RSP_INFO  = 0x82,
    UBB_RSP_DATA  = 0x83,
};

typedef struct __attribute__((packed)) {
    uint8_t  fw_major, fw_minor;
    uint32_t session_id;
    uint32_t manifest_hash;
    uint8_t  manifest_valid;
    uint8_t  target_type;
    uint32_t log_offset;
    uint32_t log_sectors;
    uint32_t block_size;
    uint32_t sector_size;
} ubb_info_rsp_t;

typedef struct __attribute__((packed)) {
    uint8_t  recording;       // 1 = sampler running
    uint8_t  link_state;      // 0 = no target, 1 = attached, 2 = halted/held
    uint8_t  level;
    uint8_t  event_latched;   // event LED state
    uint32_t dpidr;
    uint32_t last_dhcsr;
    uint32_t frames;
    uint32_t words_per_s;
    uint32_t wait_acks;
    uint32_t fault_acks;
    uint32_t overruns;
    uint32_t blocks_written;
    uint32_t next_seq;
    uint32_t ram_blocks_used;
    uint32_t faults_captured;
    uint32_t resets_seen;
    uint64_t uptime_us;
    uint32_t first_valid_sector;
    uint32_t valid_sectors;
} ubb_status_rsp_t;
