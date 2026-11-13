#pragma once
#include <stdbool.h>
#include "config.h"
#include "hardware/gpio.h"

static inline void led_init(void)
{
    gpio_init(UBB_PIN_LED_RUN);
    gpio_set_dir(UBB_PIN_LED_RUN, GPIO_OUT);
    gpio_init(UBB_PIN_LED_EVENT);
    gpio_set_dir(UBB_PIN_LED_EVENT, GPIO_OUT);
    gpio_put(UBB_PIN_LED_RUN, UBB_LED_ACTIVE_LOW);
    gpio_put(UBB_PIN_LED_EVENT, UBB_LED_ACTIVE_LOW);
}

static inline void led_run(bool on)   { gpio_put(UBB_PIN_LED_RUN,   UBB_LED_ACTIVE_LOW ? !on : on); }
static inline void led_event(bool on) { gpio_put(UBB_PIN_LED_EVENT, UBB_LED_ACTIVE_LOW ? !on : on); }
