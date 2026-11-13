// Universal Black Box - CRC-protected sector ring in the probe's own flash.
// All functions run on core 1 only.  The firmware is copy_to_ram, so the
// sampler on core 0 never touches XIP while we erase/program.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t next_seq;          // seq the next written block should carry
    uint32_t write_sector;      // ring index the next block goes to
    uint32_t valid_sectors;     // blocks with good CRC found at boot
    uint32_t first_valid_sector;
    uint32_t blocks_written;    // since boot
} flashlog_state_t;

void flashlog_init(void);                       // scans the ring
const flashlog_state_t *flashlog_state(void);
void flashlog_write_block(const uint8_t *block);   // 4096 bytes, header already final
void flashlog_erase_all(void);                     // slow; erases every sector that holds data
// Copy raw sector `index` (0..UBB_LOG_SECTORS-1) from XIP into buf.
bool flashlog_read_sector(uint32_t index, uint8_t *buf);

// Manifest storage (two slots, highest valid generation wins)
bool     flashlog_manifest_read(const uint8_t **ptr_out, uint32_t *len_out); // XIP pointer
bool     flashlog_manifest_write(const uint8_t *data, uint32_t len);
