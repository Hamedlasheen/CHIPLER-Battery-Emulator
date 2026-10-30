/* TI INA228 power monitor (master on I2C0). Readiness = checked init + OV limit (V02-01).
 * Re-verify scaling against a DMM and PSU meter (gate S3) before trusting the numbers. */
#ifndef CHIPLER_INA228_H
#define CHIPLER_INA228_H
#include <stdbool.h>
#include <stdint.h>
#include "ina228_core.h"

bool ina228_init(float r_shunt, float i_max);   /* true = identified + configured + read back */
bool ina228_present(void);                       /* READY: init OK AND OV limit programmed */
bool ina228_read(ina228_reading_t *r);
bool ina228_set_bus_ov_alert(float volts);       /* write + read back; failure clears READY */
uint16_t ina228_device_id(void);
ina_status_t ina228_status(void);
#endif
