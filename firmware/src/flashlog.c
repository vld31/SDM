// Universal Black Box - CRC-protected sector ring in the probe's own flash.
#include "flashlog.h"
#include "config.h"
#include "ubb_format.h"
#include "crc.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#include <string.h>

static flashlog_state_t st;

static const uint8_t *sector_ptr(uint32_t index)
{
    return (const uint8_t *)(XIP_BASE + UBB_LOG_OFFSET + index * UBB_BLOCK_SIZE);
}

static bool block_valid(const uint8_t *p, uint32_t *seq_out)
{
    const ubb_block_hdr_t *h = (const ubb_block_hdr_t *)p;
    if (h->magic != UBB_BLOCK_MAGIC) return false;
    if (h->len > UBB_BLOCK_SIZE - UBB_BLOCK_HDR_LEN) return false;
    ubb_block_hdr_t tmp = *h;
    tmp.crc = 0;
    uint32_t c = crc32_update(0, &tmp, sizeof tmp);
    c = crc32_update(c, p + UBB_BLOCK_HDR_LEN, h->len);
    if (c != h->crc) return false;
    *seq_out = h->seq;
    return true;
}

void flashlog_init(void)
{
    memset(&st, 0, sizeof st);
    uint32_t max_seq = 0, min_seq = 0xFFFFFFFFu;
    uint32_t max_idx = 0, min_idx = 0;
    bool any = false;
    for (uint32_t i = 0; i < UBB_LOG_SECTORS; i++) {
        uint32_t seq;
        if (block_valid(sector_ptr(i), &seq)) {
            st.valid_sectors++;
            if (!any || seq > max_seq) { max_seq = seq; max_idx = i; }
            if (!any || seq < min_seq) { min_seq = seq; min_idx = i; }
            any = true;
        }
    }
    if (any) {
        st.next_seq = max_seq + 1;
        st.write_sector = (max_idx + 1) % UBB_LOG_SECTORS;
        st.first_valid_sector = min_idx;
    } else {
        st.next_seq = 1;
        st.write_sector = 0;
        st.first_valid_sector = 0;
    }
}

const flashlog_state_t *flashlog_state(void) { return &st; }

static void erase_sector(uint32_t index)
{
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(UBB_LOG_OFFSET + index * UBB_BLOCK_SIZE, UBB_BLOCK_SIZE);
    restore_interrupts(irq);
}

void flashlog_write_block(const uint8_t *block)
{
    uint32_t idx = st.write_sector;
    erase_sector(idx);
    uint32_t irq = save_and_disable_interrupts();
    flash_range_program(UBB_LOG_OFFSET + idx * UBB_BLOCK_SIZE, block, UBB_BLOCK_SIZE);
    restore_interrupts(irq);
    const ubb_block_hdr_t *h = (const ubb_block_hdr_t *)block;
    st.next_seq = h->seq + 1;
    st.write_sector = (idx + 1) % UBB_LOG_SECTORS;
    st.blocks_written++;
    if (st.valid_sectors < UBB_LOG_SECTORS) st.valid_sectors++;
}

void flashlog_erase_all(void)
{
    for (uint32_t i = 0; i < UBB_LOG_SECTORS; i++) {
        const uint32_t *w = (const uint32_t *)sector_ptr(i);
        // skip sectors that are already blank (first word 0xFFFFFFFF and no magic)
        if (w[0] == 0xFFFFFFFFu && w[1] == 0xFFFFFFFFu) continue;
        erase_sector(i);
    }
    uint32_t keep_seq = st.next_seq;   // sequence numbers never go backwards
    memset(&st, 0, sizeof st);
    st.next_seq = keep_seq;
}

bool flashlog_read_sector(uint32_t index, uint8_t *buf)
{
    if (index >= UBB_LOG_SECTORS) return false;
    memcpy(buf, sector_ptr(index), UBB_BLOCK_SIZE);
    return true;
}

// ---- manifest slots ------------------------------------------------------
// slot layout: [u32 magic 'UBBS'][u32 generation][u32 len][u32 crc32(data)] + data

#define SLOT_MAGIC 0x53424255u

typedef struct __attribute__((packed)) { uint32_t magic, gen, len, crc; } slot_hdr_t;

static const uint8_t *slot_ptr(int s)
{
    return (const uint8_t *)(XIP_BASE + UBB_MANIFEST_OFFSET + s * UBB_MANIFEST_SLOT_SIZE);
}

static bool slot_valid(int s, uint32_t *gen)
{
    const slot_hdr_t *h = (const slot_hdr_t *)slot_ptr(s);
    if (h->magic != SLOT_MAGIC) return false;
    if (h->len == 0 || h->len > UBB_MANIFEST_SLOT_SIZE - sizeof *h) return false;
    if (crc32_buf(slot_ptr(s) + sizeof *h, h->len) != h->crc) return false;
    *gen = h->gen;
    return true;
}

static int best_slot(uint32_t *gen_out)
{
    int best = -1;
    uint32_t best_gen = 0;
    for (int s = 0; s < 2; s++) {
        uint32_t g;
        if (slot_valid(s, &g) && (best < 0 || g > best_gen)) { best = s; best_gen = g; }
    }
    if (gen_out) *gen_out = best_gen;
    return best;
}

bool flashlog_manifest_read(const uint8_t **ptr_out, uint32_t *len_out)
{
    int s = best_slot(NULL);
    if (s < 0) return false;
    const slot_hdr_t *h = (const slot_hdr_t *)slot_ptr(s);
    *ptr_out = slot_ptr(s) + sizeof *h;
    *len_out = h->len;
    return true;
}

bool flashlog_manifest_write(const uint8_t *data, uint32_t len)
{
    if (len == 0 || len > UBB_MANIFEST_SLOT_SIZE - sizeof(slot_hdr_t)) return false;
    uint32_t gen = 0;
    int cur = best_slot(&gen);
    int target = (cur < 0) ? 0 : (cur ^ 1);
    static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    uint32_t off = UBB_MANIFEST_OFFSET + target * UBB_MANIFEST_SLOT_SIZE;

    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(off, UBB_MANIFEST_SLOT_SIZE);
    restore_interrupts(irq);

    slot_hdr_t h = { SLOT_MAGIC, gen + 1, len, crc32_buf(data, len) };
    uint32_t total = sizeof h + len;
    for (uint32_t pos = 0; pos < total; pos += FLASH_PAGE_SIZE) {
        memset(page, 0xFF, sizeof page);
        for (uint32_t i = 0; i < FLASH_PAGE_SIZE && pos + i < total; i++) {
            uint32_t src = pos + i;
            page[i] = src < sizeof h ? ((const uint8_t *)&h)[src] : data[src - sizeof h];
        }
        irq = save_and_disable_interrupts();
        flash_range_program(off + pos, page, FLASH_PAGE_SIZE);
        restore_interrupts(irq);
    }
    return true;
}
