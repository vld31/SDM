// Universal Black Box - demo target ("the machine under observation").
//
// Nothing in here knows about the probe.  It is an ordinary PicoBricks-style
// program with a state machine, a few counters, a PWM sweep on the LED, a
// potentiometer on the ADC and a button.  After a while (or on a long button
// press) it dereferences a bad pointer and hard-faults; the watchdog then
// reboots it.  The black box should be able to tell that whole story from
// the outside.
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/watchdog.h"
#include <stdint.h>
#include <string.h>

// ---- PicoBricks pin map (override with -D if your board differs) ----
#ifndef PIN_LED
#define PIN_LED     7
#endif
#ifndef PIN_BUTTON
#define PIN_BUTTON  10
#endif
#ifndef PIN_BUZZER
#define PIN_BUZZER  20
#endif
#ifndef PIN_POT
#define PIN_POT     26     // ADC0
#endif
#ifndef CRASH_AFTER_S
#define CRASH_AFTER_S 90   // deliberate fault after this many seconds (0 = only on long press)
#endif

typedef enum { ST_BOOT = 0, ST_IDLE, ST_RAMP_UP, ST_HOLD, ST_RAMP_DOWN, ST_ALARM, ST_FAILING } machine_state_t;

typedef struct {
    uint8_t  led_on      : 1;
    uint8_t  button_down : 1;
    uint8_t  alarm       : 1;
    uint8_t  reserved    : 5;
    uint8_t  duty_pct;
    uint16_t pot_raw;
} io_flags_t;

// ---- the globals the black box will watch ----
volatile machine_state_t g_state = ST_BOOT;
volatile uint32_t g_loop_count;
volatile uint32_t g_button_presses;
volatile uint32_t g_uptime_ms;
volatile int32_t  g_temperature_c10 = 215;     // fake sensor, tenths of a degree
volatile float    g_filtered_pot;
volatile io_flags_t g_io;
volatile uint32_t g_errors;
volatile uint32_t g_state_enter_ms;
volatile uint32_t g_cycles;
volatile char     g_last_event[16] = "boot";
volatile uint32_t g_history[8];                 // ring of state codes
static uint32_t   s_history_idx;
void (*volatile g_bad_fn)(void) = 0;            // the NULL pointer we eventually call

static void set_event(const char *s)
{
    size_t n = strlen(s);
    if (n > 15) n = 15;
    memcpy((void *)g_last_event, s, n);
    g_last_event[n] = 0;
}

static void enter(machine_state_t s)
{
    g_state = s;
    g_state_enter_ms = g_uptime_ms;
    g_history[s_history_idx++ & 7] = s;
    const char *names[] = { "boot", "idle", "ramp_up", "hold", "ramp_down", "alarm", "failing" };
    set_event(names[s]);
}

static void led_set_pct(uint8_t pct)
{
    g_io.duty_pct = pct;
    g_io.led_on = pct > 0;
    uint slice = pwm_gpio_to_slice_num(PIN_LED);
    pwm_set_gpio_level(PIN_LED, (uint16_t)(pct * 65535u / 100u));
    (void)slice;
}

int main(void)
{
    stdio_init_all();
    gpio_init(PIN_BUTTON);
    gpio_set_dir(PIN_BUTTON, GPIO_IN);
    gpio_pull_up(PIN_BUTTON);
    gpio_set_function(PIN_LED, GPIO_FUNC_PWM);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 4.0f);
    pwm_init(pwm_gpio_to_slice_num(PIN_LED), &cfg, true);
    adc_init();
    adc_gpio_init(PIN_POT);
    adc_select_input(0);
    gpio_init(PIN_BUZZER);
    gpio_set_dir(PIN_BUZZER, GPIO_OUT);

    watchdog_enable(2000, true);     // a crash ends in a watchdog reboot, like a real machine
    enter(ST_IDLE);

    uint32_t press_started = 0;
    uint8_t duty = 0;
    for (;;) {
        g_loop_count++;
        g_uptime_ms = to_ms_since_boot(get_absolute_time());
        watchdog_update();

        bool down = !gpio_get(PIN_BUTTON);
        if (down && !g_io.button_down) { g_button_presses++; press_started = g_uptime_ms; set_event("press"); }
        if (!down && g_io.button_down && g_uptime_ms - press_started > 1500) enter(ST_FAILING);
        g_io.button_down = down;

        g_io.pot_raw = adc_read();
        g_filtered_pot = g_filtered_pot * 0.9f + (float)g_io.pot_raw * 0.1f;
        g_temperature_c10 = 215 + (int32_t)(g_filtered_pot / 100.0f);

        uint32_t in_state = g_uptime_ms - g_state_enter_ms;
        switch (g_state) {
        case ST_IDLE:
            if (g_io.button_down || in_state > 3000) enter(ST_RAMP_UP);
            break;
        case ST_RAMP_UP:
            if (in_state / 50 > duty) { duty = (uint8_t)(in_state / 50); if (duty > 100) duty = 100; led_set_pct(duty); }
            if (duty >= 100) enter(ST_HOLD);
            break;
        case ST_HOLD:
            if (in_state > 2000) enter(ST_RAMP_DOWN);
            if (g_io.pot_raw > 4000 && !g_io.alarm) { g_io.alarm = 1; g_errors++; enter(ST_ALARM); }
            break;
        case ST_RAMP_DOWN:
            if (duty > 0 && in_state / 50 > (uint32_t)(100 - duty)) { duty--; led_set_pct(duty); }
            if (duty == 0) { g_cycles++; enter(ST_IDLE); }
            break;
        case ST_ALARM:
            gpio_put(PIN_BUZZER, (in_state / 100) & 1);
            if (in_state > 1500) { gpio_put(PIN_BUZZER, 0); g_io.alarm = 0; enter(ST_RAMP_DOWN); }
            break;
        case ST_FAILING:
            set_event("crash!");
            g_errors++;
            sleep_ms(50);
            g_bad_fn();                 // NULL call -> HardFault -> watchdog reboot
            break;
        default:
            enter(ST_IDLE);
        }
        if (CRASH_AFTER_S && g_uptime_ms > (uint32_t)CRASH_AFTER_S * 1000u && g_state != ST_FAILING) enter(ST_FAILING);
        sleep_ms(5);
    }
}
