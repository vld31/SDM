// Universal Black Box - the recorder loop (core 0)
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    volatile uint8_t  recording;      // 1 while the loop samples
    volatile uint8_t  link_state;     // 0 none, 1 attached, 2 held
    volatile uint8_t  event_latched;
    volatile uint32_t dpidr;
    volatile uint32_t last_dhcsr;
    volatile uint32_t frames;
    volatile uint32_t words_per_s;
    volatile uint32_t faults_captured;
    volatile uint32_t resets_seen;
} sampler_status_t;

extern sampler_status_t g_sampler;

// Requests from core 1 (host side).  Each is consumed by the sampler loop.
void sampler_request_pause(void);     // blocks until the loop is parked
void sampler_resume_after_pause(void);
void sampler_request_reload(void);    // reload manifest from flash on next pause end
void sampler_request_mark(uint32_t id, bool with_snapshot);
void sampler_request_start(void);
void sampler_request_stop(void);
void sampler_request_flush(void);
void sampler_clear_event(void);

void sampler_main(void);   // never returns; run on core 0
