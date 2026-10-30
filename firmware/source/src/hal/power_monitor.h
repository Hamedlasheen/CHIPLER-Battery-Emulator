#ifndef CHIPLER_POWER_MONITOR_H
#define CHIPLER_POWER_MONITOR_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { float v_bus,i,p,t_die; bool ok,new_sample,temperature_valid,uncalibrated; } pm_reading_t;
typedef struct {int16_t raw_shunt,raw_current;uint16_t raw_bus,raw_power;uint32_t seq;bool valid;} pm_diagnostic_t;
bool pm_diagnostic(pm_diagnostic_t *);
bool pm_init(void);
bool pm_present(void);
bool pm_read(pm_reading_t *r);
uint16_t pm_device_id(void);
const char *pm_status(void);
const char *pm_name(void);
bool pm_fault_latched(void);
bool pm_ack_fault(void);
#endif
