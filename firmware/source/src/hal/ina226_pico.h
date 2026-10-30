/* Thin Pico HAL for ina226_core (CR-006 Phase A): I2C0 GP8/GP9, ALERT input GP10 (external pull-up). */
#ifndef CHIPLER_INA226_PICO_H
#define CHIPLER_INA226_PICO_H
#include <stdbool.h>
#include <stdint.h>
#include "ina226_core.h"
const ina226_bus_t *ina226_pico_bus_init(uint32_t hz);
bool ina226_pico_alert_low(void);
int  ina226_pico_last_rc(void);
#endif
