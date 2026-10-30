/* Host fake for the MMIO harness (tests/host/test_live_port_mmio.c). Only what live_port.c/bus_en.h need. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define __not_in_flash_func(x) x
#define GPIO_OUT 1
#define GPIO_IN 0
#define GPIO_FUNC_I2C 3
#define GPIO_FUNC_SIO 5
uint32_t time_us_32(void);
void gpio_init(unsigned);
void gpio_set_dir(unsigned, bool);
void gpio_put(unsigned, bool);
void gpio_disable_pulls(unsigned);
void gpio_set_function(unsigned, unsigned);
