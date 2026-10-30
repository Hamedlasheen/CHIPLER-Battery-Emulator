#pragma once
#define I2C0_IRQ 0
#define PICO_HIGHEST_IRQ_PRIORITY 0
void irq_set_priority(unsigned,unsigned);
