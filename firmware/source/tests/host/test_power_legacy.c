#include <stdio.h>
#include "pico/stdlib.h"
#include "../../src/hal/ina228.h"
static bool present,alert;
bool ina228_init(float r,float imax){present=r>0&&imax>0;return present;}
bool ina228_present(void){return present;}
bool ina228_set_bus_ov_alert(float v){return v>0;}
uint16_t ina228_device_id(void){return 0x2281;}
ina_status_t ina228_status(void){return INA_OK;}
const char *ina_status_str(ina_status_t s){(void)s;return "OK";}
bool pm_alert_active(void){return alert;}
bool ina228_read(ina228_reading_t *r){*r=(ina228_reading_t){4.0f,0.4f,1.6f,25.0f,true,true};return true;}
#define CHIPLER_PM_USE_INA228 1
#include "../../src/hal/power_monitor.c"
int main(void){
    pm_reading_t r={0};
    if(!pm_init()||!pm_present()||!pm_read(&r)||!r.temperature_valid||r.v_bus!=4.0f||pm_device_id()!=0x2281)return 1;
    alert=true;if(!pm_fault_latched()||pm_ack_fault())return 1;
    puts("Legacy INA228 compile-time adapter: PASS (mock HAL, no hardware claim)");return 0;
}
