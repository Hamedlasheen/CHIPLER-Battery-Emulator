#pragma once
#include <stdbool.h>
void gpio_init(unsigned pin);
void gpio_set_dir(unsigned pin, bool out);
void gpio_put(unsigned pin, bool v);
bool gpio_get(unsigned pin);
void gpio_pull_up(unsigned pin);
void gpio_set_function(unsigned pin, unsigned fn);
