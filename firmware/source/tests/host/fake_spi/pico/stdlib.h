#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef unsigned int uint;
#define GPIO_OUT 1
#define GPIO_IN 0
#define GPIO_FUNC_SPI 1
void tight_loop_contents(void);
uint32_t time_us_32(void);
void gpio_init(unsigned);
void gpio_set_dir(unsigned, bool);
void gpio_put(unsigned, bool);
void gpio_pull_up(unsigned);
void gpio_set_function(unsigned, unsigned);
