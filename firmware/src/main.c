// Universal Black Box - probe firmware entry point (Pico 2 W)
//
// Core 0: the recorder (sampler.c) - reads the target over SWD, writes records.
// Core 1: flash writer + USB host protocol (host.c).
// The whole binary runs from RAM (copy_to_ram) so flash erase/program on
// core 1 never stalls the recorder.
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/stdio_usb.h"
#include "pico/unique_id.h"
#include "hardware/structs/sio.h"
#include "config.h"
#include "leds.h"
#include "flashlog.h"
#include "stream.h"
#include "sampler.h"
#include "host.h"
#include "crc.h"

uint32_t g_session_id;

static uint32_t make_session_id(void)
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    // Board id + the next block sequence number: every power-up of the same probe gets a new id,
    // because the flash ring keeps counting up across boots (boot time alone is nearly constant).
    uint32_t c = crc32_buf(id.id, sizeof id.id);
    return c ^ (flashlog_state()->next_seq * 2654435761u) ^ 0x5A5A0001u;
}

static void core1_entry(void)
{
    host_main();
}

int main(void)
{
    led_init();
    led_run(true);
    led_event(true);          // both on for ~200 ms at boot: lamp test
    sleep_ms(200);
    led_run(false);
    led_event(false);

    flashlog_init();                                   // scan the ring (core 0, before the sampler starts)
    g_session_id = make_session_id();
    stream_init(flashlog_state()->next_seq, g_session_id, 0);

    stdio_usb_init();     // on core 0, so its timer + IRQ handlers are serviced here
    // USB + flash writing need far more than the 2 KB default core-1 stack.
    static uint32_t core1_stack[4096];
    multicore_launch_core1_with_stack(core1_entry, core1_stack, sizeof core1_stack);
    sampler_main();                                     // never returns
    return 0;
}
