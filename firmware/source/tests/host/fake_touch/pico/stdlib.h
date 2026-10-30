#pragma once
/* AG-TOUCH fake SDK for test_touch_hw_sim.c: only what src/input/touch_hw.c uses. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef unsigned int uint;
void gpio_put(unsigned pin, bool v);
bool gpio_get(unsigned pin);
