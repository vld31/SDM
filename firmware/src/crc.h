#pragma once
#include <stdint.h>
#include <stddef.h>

// CRC-32 (IEEE 802.3, reflected, init 0xFFFFFFFF, final xor) - matches zlib.crc32
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
static inline uint32_t crc32_buf(const void *data, size_t len) { return crc32_update(0, data, len); }

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) for the USB protocol frames
uint16_t crc16_ccitt(const void *data, size_t len);
