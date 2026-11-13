// Universal Black Box - probe build configuration
#pragma once

#define UBB_FW_VERSION_MAJOR 0
#define UBB_FW_VERSION_MINOR 2

// ---- Wiring (Pico 2 W as probe) -----------------------------------------
#define UBB_PIN_SWCLK      2
#define UBB_PIN_SWDIO      3
#define UBB_PIN_LED_RUN    12   // active LOW: lit while recording a live target
#define UBB_PIN_LED_EVENT  18   // active LOW: lit after a fault/snapshot was captured
#define UBB_PIN_TRIGGER    -1   // optional push button to GND; -1 = none

#define UBB_LED_ACTIVE_LOW 1

// ---- SWD link ------------------------------------------------------------
#define UBB_SWD_PIO            pio1     // leave pio0 free
#define UBB_SWD_DEFAULT_HZ     4000000  // manifest may override; jumper wires like 4..10 MHz
#define UBB_SWD_WAIT_RETRIES   64

// ---- Flash layout (Pico 2 W: 4 MB) ---------------------------------------
// Firmware is copied to RAM at boot and lives in the first 1 MB - 64 KB.
#define UBB_FLASH_TOTAL        (4u * 1024 * 1024)
#define UBB_MANIFEST_OFFSET    (0x100000u - 0x10000u)   // 64 KB: two 32 KB manifest slots
#define UBB_MANIFEST_SLOT_SIZE 0x8000u
#define UBB_LOG_OFFSET         0x100000u                 // log ring starts at 1 MB
#define UBB_LOG_SIZE           (UBB_FLASH_TOTAL - UBB_LOG_OFFSET)
#define UBB_LOG_SECTORS        (UBB_LOG_SIZE / 4096u)

// ---- RAM buffering -------------------------------------------------------
#define UBB_BLOCK_SIZE         4096u
#define UBB_RAM_BLOCKS         40u      // 160 KB of staged blocks between sampler and flash writer
#define UBB_SNAPSHOT_CHUNK     1024u    // bytes per snapshot record

// ---- Recording defaults (manifest can override) ---------------------------
#define UBB_DEFAULT_FAST_PERIOD_US    1000u
#define UBB_DEFAULT_SLOW_PERIOD_US    10000u
#define UBB_DEFAULT_KEYFRAME_US       1000000u
#define UBB_DEFAULT_LINKSTAT_US       1000000u
#define UBB_CORE1_POLL_US             10000u
#define UBB_RECONNECT_US              200000u

#define UBB_MAX_ENTRIES        256
#define UBB_MAX_REGIONS        16
#define UBB_MAX_TRIGGERS       8
