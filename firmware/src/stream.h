// Universal Black Box - record stream staged in RAM blocks.
// Producer: sampler (core 0).  Consumer: flash writer (core 1).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ubb_format.h"

void     stream_init(uint32_t first_seq, uint32_t session_id, uint32_t manifest_hash);
void     stream_set_manifest_hash(uint32_t h);

// ---- producer side (core 0 only) ----
// Bytes still free in the current block (a block is opened lazily).
uint32_t stream_remaining(void);
bool     stream_block_empty(void);      // true if no record is in the current block yet
// Append one record. Returns false (and writes nothing) if it does not fit.
bool     stream_put(uint8_t tag, const void *payload, uint32_t len);
// Same, with the record assembled from two parts (header + bulk data).
bool     stream_put2(uint8_t tag, const void *p1, uint32_t l1, const void *p2, uint32_t l2);
// Close the current block (if any) and hand it to the consumer.
void     stream_close_block(void);
// Age of the current open block in microseconds (0 if none).
uint64_t stream_block_age_us(uint64_t now);
void     stream_mark_keyframe(void);    // sets HAS_KEYFRAME on the current block
void     stream_note_drop(uint32_t n);  // records dropped by the producer

uint32_t stream_next_seq(void);
uint32_t stream_overruns(void);
uint32_t stream_dropped_records(void);
uint32_t stream_blocks_staged(void);

// ---- consumer side (core 1 only) ----
const uint8_t *stream_peek_block(void);  // NULL if none ready
void           stream_release_block(void);

// helpers
uint32_t ubb_varint_encode(uint8_t *out, uint32_t v);
