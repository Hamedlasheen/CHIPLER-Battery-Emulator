#pragma once
#include <stdbool.h>
#include <stdint.h>
#define __not_in_flash_func(x) x
#define GPIO_OUT 1
#define GPIO_IN 0
#define GPIO_FUNC_I2C 3
uint32_t time_us_32(void);
void gpio_init(unsigned);void gpio_set_dir(unsigned,bool);void gpio_put(unsigned,bool);
void gpio_disable_pulls(unsigned);void gpio_set_function(unsigned,unsigned);
void gpio_pull_up(unsigned);bool gpio_get(unsigned);

typedef uint64_t absolute_time_t;
absolute_time_t get_absolute_time(void);
uint32_t to_ms_since_boot(absolute_time_t);
