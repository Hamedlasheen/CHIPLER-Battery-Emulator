#pragma once
#include <stdbool.h>
#define I2C0_IRQ 36
#define PICO_HIGHEST_IRQ_PRIORITY 0
void irq_set_enabled(unsigned, bool);
void irq_set_exclusive_handler(unsigned, void (*)(void));
void irq_remove_handler(unsigned, void (*)(void));
void irq_set_priority(unsigned, unsigned);
