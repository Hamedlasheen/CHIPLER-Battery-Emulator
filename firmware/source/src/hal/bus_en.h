/* Translator enable abstraction (P1_r1).
 * Legacy round-panel hardware had a real EN line (GP5). The P1 prototype uses a discrete MOSFET
 * translator WITHOUT EN: bus_en_put() is then a no-op and the code must not claim electrical isolation.
 * "Stopped" in P1 means: I2C1 disabled, IRQ off, GP6/GP7 released to inputs without pulls. */
#ifndef CHIPLER_BUS_EN_H
#define CHIPLER_BUS_EN_H
#include <stdbool.h>
#include "board_config.h"
#include "pico/stdlib.h"
#if BAT_BUS_EN_PRESENT
static inline void bus_en_init(void) { gpio_init(PIN_BAT_BUS_EN); gpio_set_dir(PIN_BAT_BUS_EN, GPIO_OUT); gpio_put(PIN_BAT_BUS_EN, 0); }
static inline void bus_en_put(bool on) { gpio_put(PIN_BAT_BUS_EN, on); }
#else
static inline void bus_en_init(void) {}
static inline void bus_en_put(bool on) { (void)on; }
#endif
/* Release both battery-bus pins to plain inputs without pulls (hi-Z from the MCU side). */
static inline void bus_pins_release(void) {
    gpio_init(PIN_BAT_SDA); gpio_set_dir(PIN_BAT_SDA, GPIO_IN); gpio_disable_pulls(PIN_BAT_SDA);
    gpio_init(PIN_BAT_SCL); gpio_set_dir(PIN_BAT_SCL, GPIO_IN); gpio_disable_pulls(PIN_BAT_SCL);
}
#endif
