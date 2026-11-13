// Universal Black Box - SWD transaction layer on top of the PIO engine.
// Everything here runs from RAM (whole binary is copy_to_ram) and is called
// only from the sampler core.
#include "swd.h"
#include "config.h"
#include "swd.pio.h"
#include "hardware/pio.h"
#include "pico/time.h"

static PIO  pio;
static uint sm;
static uint prog_offset;
static bool line_output;   // true when the probe currently drives SWDIO

swd_stats_t swd_stats;

static inline uint32_t fmt_cmd(uint32_t nbits, bool output, uint target)
{
    return (nbits - 1) | ((output ? 1u : 0u) << 8) | ((prog_offset + target) << 9);
}

static inline void put(uint32_t w) { pio_sm_put_blocking(pio, sm, w); }
static inline uint32_t get(void)   { return pio_sm_get_blocking(pio, sm); }

static inline void write_word_bits(uint32_t data, uint32_t nbits)
{
    put(fmt_cmd(nbits, true, swd_offset_write_bits));
    put(data);
    line_output = true;
}

static inline uint32_t read_word_bits(uint32_t nbits)
{
    put(fmt_cmd(nbits, false, swd_offset_read_bits));
    line_output = false;
    uint32_t v = get();
    if (nbits < 32) v >>= (32 - nbits);
    return v;
}

// One turnaround clock with SWDIO released.
static inline void turnaround(void)
{
    put(fmt_cmd(1, false, swd_offset_write_bits));
    put(0);
    line_output = false;
}

void swd_init(uint32_t swclk_hz)
{
    pio = UBB_SWD_PIO;
    sm = pio_claim_unused_sm(pio, true);
    prog_offset = pio_add_program(pio, &swd_program);
    swd_program_init(pio, sm, prog_offset, UBB_PIN_SWCLK, UBB_PIN_SWDIO, (float)swclk_hz);
    line_output = false;
}

void swd_set_clock(uint32_t swclk_hz)
{
    float div = (float)clock_get_hz(clk_sys) / (4.0f * (float)swclk_hz);
    if (div < 1.0f) div = 1.0f;
    pio_sm_set_clkdiv(pio, sm, div);
}

void swd_write_bits(const uint32_t *words, uint32_t nbits)
{
    while (nbits) {
        uint32_t n = nbits > 32 ? 32 : nbits;
        write_word_bits(*words++, n);
        nbits -= n;
    }
}

void swd_idle(uint32_t clocks)
{
    while (clocks) {
        uint32_t n = clocks > 32 ? 32 : clocks;
        write_word_bits(0, n);
        clocks -= n;
    }
}

void swd_line_reset(void)
{
    // 56 clocks high, then at least 2 idle clocks low.
    write_word_bits(0xFFFFFFFFu, 32);
    write_word_bits(0xFFFFFFFFu, 24);
    write_word_bits(0, 8);
}

void swd_jtag_to_swd(void)
{
    // 16-bit JTAG-to-SWD select sequence 0xE79E, LSB first.
    write_word_bits(0xFFFFFFFFu, 32);
    write_word_bits(0xFFFFFFFFu, 24);
    write_word_bits(0xE79Eu, 16);
    swd_line_reset();
}

void swd_dormant_to_swd(void)
{
    // ADIv5.2 dormant -> SWD: >=8 high, 128-bit selection alert, 4 low, activation code 0x1A.
    static const uint32_t alert[4] = { 0x6209F392u, 0x86852D95u, 0xE3DDAFE9u, 0x19BC0EA2u };
    write_word_bits(0xFFu, 8);
    swd_write_bits(alert, 128);
    write_word_bits(0x1Au << 4, 12);
    swd_line_reset();
}

static inline uint32_t parity32(uint32_t v)
{
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return v & 1u;
}

static inline uint8_t make_request(bool ap, bool read, uint8_t addr)
{
    uint32_t a = (addr >> 2) & 3u;
    uint32_t par = ((ap ? 1u : 0u) + (read ? 1u : 0u) + (a & 1u) + (a >> 1)) & 1u;
    return (uint8_t)(1u | ((ap ? 1u : 0u) << 1) | ((read ? 1u : 0u) << 2) | (a << 3) | (par << 5) | (1u << 7));
}

static int transfer_once(bool ap, bool read, uint8_t addr, uint32_t *data, uint32_t *ack_out)
{
    write_word_bits(make_request(ap, read, addr), 8);
    turnaround();
    uint32_t ack = read_word_bits(3);
    *ack_out = ack;
    if (ack == SWD_ACK_OK) {
        if (read) {
            uint32_t v = read_word_bits(32);
            uint32_t p = read_word_bits(1);
            turnaround();
            if (parity32(v) != p) {
                swd_stats.parity_errors++;
                swd_idle(8);
                return SWD_ERR_PARITY;
            }
            *data = v;
        } else {
            turnaround();
            write_word_bits(*data, 32);
            write_word_bits(parity32(*data), 1);
        }
        swd_idle(8);
        return SWD_OK;
    }
    // WAIT / FAULT / no response: no data phase (overrun detection is off)
    turnaround();
    swd_idle(8);
    if (ack == SWD_ACK_WAIT)  { swd_stats.wait_acks++;  return SWD_ERR_WAIT; }
    if (ack == SWD_ACK_FAULT) { swd_stats.fault_acks++; return SWD_ERR_FAULT; }
    swd_stats.proto_errors++;
    return SWD_ERR_PROTO;
}

int swd_transfer(bool ap, bool read, uint8_t addr, uint32_t *data)
{
    uint32_t ack;
    swd_stats.transfers++;
    for (int i = 0; i < UBB_SWD_WAIT_RETRIES; i++) {
        int r = transfer_once(ap, read, addr, data, &ack);
        if (r != SWD_ERR_WAIT) return r;
    }
    return SWD_ERR_WAIT;
}

void swd_write_targetsel(uint32_t id)
{
    // DP write to address 0xC while the line is in reset state: the target
    // does not drive the ACK, so we clock through it and send the data anyway.
    write_word_bits(make_request(false, false, 0xC), 8);
    turnaround();
    (void)read_word_bits(3);
    turnaround();
    write_word_bits(id, 32);
    write_word_bits(parity32(id), 1);
    swd_idle(8);
}
