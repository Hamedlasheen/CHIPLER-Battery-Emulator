#ifndef CHIPLER_TOUCH_HW_H
#define CHIPLER_TOUCH_HW_H
#include <stdbool.h>
#include "touch_core.h"
void touch_hw_init(void);
/* One poll. Returns false when the bus was refused (wrong core) or no backend is built.
 * r->pen is true only when the pressure measurement says the panel is touched. */
bool touch_hw_read(touch_raw_t *r);
bool touch_hw_irq_active(void);   /* diagnostic only until T_IRQ polarity is confirmed */
#endif
