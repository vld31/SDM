// Universal Black Box - SWD transaction layer on top of the PIO engine
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SWD_ACK_OK    1
#define SWD_ACK_WAIT  2
#define SWD_ACK_FAULT 4
#define SWD_ACK_NONE  7   // line idle/high: no target answered

// Result codes for swd_transfer()
#define SWD_OK        0
#define SWD_ERR_WAIT  1   // retried and still WAIT
#define SWD_ERR_FAULT 2
#define SWD_ERR_PROTO 3   // bad ACK (no target) or parity error
#define SWD_ERR_PARITY 4

typedef struct {
    uint32_t transfers;
    uint32_t wait_acks;
    uint32_t fault_acks;
    uint32_t proto_errors;
    uint32_t parity_errors;
} swd_stats_t;

extern swd_stats_t swd_stats;

void swd_init(uint32_t swclk_hz);
void swd_set_clock(uint32_t swclk_hz);

// Raw bit sequences (LSB first)
void swd_write_bits(const uint32_t *words, uint32_t nbits);   // probe drives SWDIO
void swd_idle(uint32_t clocks);                                // SWDIO low for n clocks
void swd_line_reset(void);                                     // >= 50 clocks high + idle
void swd_jtag_to_swd(void);
void swd_dormant_to_swd(void);

// One SWD read/write transaction.  ap=true for AP registers, addr is the
// register address (0x0,0x4,0x8,0xC).  Returns SWD_* code.
int swd_transfer(bool ap, bool read, uint8_t addr, uint32_t *data);

// TARGETSEL write (no ACK is driven by the target).
void swd_write_targetsel(uint32_t id);
