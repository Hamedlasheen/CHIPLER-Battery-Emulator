/* AG-LCD fake pico-sdk (host only). Models just enough of pico/stdlib.h for src/hal/lcd.c,
 * src/hal/spi_bus.c and src/input/touch_hw.c. Implementation: fake_lcd/fake_sdk.c */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef unsigned int uint;
#define GPIO_OUT 1
#define GPIO_IN 0
#define GPIO_FUNC_SPI 1
void tight_loop_contents(void);
void sleep_ms(uint32_t ms);
void sleep_us(uint64_t us);
uint32_t time_us_32(void);
#include "hardware/gpio.h"
