// Universal Black Box - record stream staged in RAM blocks.
#include "stream.h"
#include "config.h"
#include "crc.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include <string.h>

static uint8_t  blocks[UBB_RAM_BLOCKS][UBB_BLOCK_SIZE] __attribute__((aligned(4)));
static volatile uint32_t head;   // next block index the producer will finalize into
static volatile uint32_t tail;   // next block index the consumer will take

static bool     open_block;
static uint32_t used;            // payload bytes used in the open block
static uint64_t block_ts;        // probe time of the first record
static uint64_t last_rec_ts;
static uint8_t  block_flags;
static bool     pending_gap;

static uint32_t next_seq;
static uint32_t session_id;
static uint32_t manifest_hash;
static uint32_t overruns;
static uint32_t dropped;

#define PAYLOAD_MAX (UBB_BLOCK_SIZE - UBB_BLOCK_HDR_LEN)

uint32_t ubb_varint_encode(uint8_t *out, uint32_t v)
{
    uint32_t n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        out[n++] = b;
    } while (v);
    return n;
}

void stream_init(uint32_t first_seq, uint32_t sid, uint32_t mhash)
{
    head = tail = 0;
    open_block = false;
    used = 0;
    next_seq = first_seq;
    session_id = sid;
    manifest_hash = mhash;
    overruns = dropped = 0;
    pending_gap = false;
}

void stream_set_manifest_hash(uint32_t h) { manifest_hash = h; }

static inline bool ring_full(void)
{
    return ((head + 1) % UBB_RAM_BLOCKS) == tail;
}

static uint8_t *cur_payload(void)
{
    return blocks[head] + UBB_BLOCK_HDR_LEN;
}

static void open_new_block(uint64_t now)
{
    open_block = true;
    used = 0;
    block_ts = now;
    last_rec_ts = now;
    block_flags = pending_gap ? UBB_BLKF_AFTER_GAP : 0;
    pending_gap = false;
}

uint32_t stream_remaining(void)
{
    if (!open_block) return PAYLOAD_MAX;
    return PAYLOAD_MAX - used;
}

bool stream_block_empty(void)
{
    return !open_block || used == 0;
}

void stream_mark_keyframe(void)
{
    if (open_block) block_flags |= UBB_BLKF_HAS_KEYFRAME;
}

void stream_note_drop(uint32_t n)
{
    dropped += n;
    pending_gap = true;
}

bool stream_put2(uint8_t tag, const void *p1, uint32_t l1, const void *p2, uint32_t l2)
{
    uint64_t now = time_us_64();
    if (!open_block) {
        if (ring_full()) { overruns++; dropped++; pending_gap = true; return false; }
        open_new_block(now);
    }
    uint8_t vb[5];
    uint32_t dt = (uint32_t)(now - last_rec_ts);
    uint32_t vn = ubb_varint_encode(vb, dt);
    uint32_t need = 1 + vn + l1 + l2;
    if (used + need > PAYLOAD_MAX) return false;
    uint8_t *p = cur_payload() + used;
    *p++ = tag;
    memcpy(p, vb, vn); p += vn;
    if (l1) { memcpy(p, p1, l1); p += l1; }
    if (l2) { memcpy(p, p2, l2); p += l2; }
    used += need;
    last_rec_ts = now;
    return true;
}

bool stream_put(uint8_t tag, const void *payload, uint32_t len)
{
    return stream_put2(tag, payload, len, NULL, 0);
}

void stream_close_block(void)
{
    if (!open_block) return;
    if (used == 0) { open_block = false; return; }
    ubb_block_hdr_t *h = (ubb_block_hdr_t *)blocks[head];
    h->magic = UBB_BLOCK_MAGIC;
    h->seq = next_seq++;
    h->ts_us = block_ts;
    h->type = UBB_BLK_STREAM;
    h->flags = block_flags;
    h->len = (uint16_t)used;
    h->manifest_hash = manifest_hash;
    h->session_id = session_id;
    h->crc = 0;
    // pad the unused tail with 0xFF so flash programming of a partial block is clean
    memset(blocks[head] + UBB_BLOCK_HDR_LEN + used, 0xFF, PAYLOAD_MAX - used);
    h->crc = crc32_buf(blocks[head], UBB_BLOCK_HDR_LEN + used);
    __dmb();
    head = (head + 1) % UBB_RAM_BLOCKS;
    open_block = false;
    used = 0;
}

uint64_t stream_block_age_us(uint64_t now)
{
    if (!open_block || used == 0 || now <= block_ts) return 0;   // callers may hold a slightly stale 'now'
    return now - block_ts;
}

uint32_t stream_next_seq(void) { return next_seq; }
uint32_t stream_overruns(void) { return overruns; }
uint32_t stream_dropped_records(void) { return dropped; }
uint32_t stream_blocks_staged(void) { return (head + UBB_RAM_BLOCKS - tail) % UBB_RAM_BLOCKS; }

const uint8_t *stream_peek_block(void)
{
    if (head == tail) return NULL;
    __dmb();
    return blocks[tail];
}

void stream_release_block(void)
{
    if (head == tail) return;
    __dmb();
    tail = (tail + 1) % UBB_RAM_BLOCKS;
}
