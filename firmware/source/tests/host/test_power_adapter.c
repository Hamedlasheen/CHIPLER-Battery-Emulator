#include <stdio.h>
static unsigned checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
#include "test_ina226.c"
#include "pico/stdlib.h"
static m226_t sensor;
static uint32_t clock_ms;
static bool pin_low;
absolute_time_t get_absolute_time(void){return clock_ms;}
uint32_t to_ms_since_boot(absolute_time_t t){return (uint32_t)t;}
static ina226_bus_t adapter_bus={m226_wr,m226_rd,m226_delay,&sensor,m226_now};
const ina226_bus_t *ina226_pico_bus_init(uint32_t hz){CHECK(hz==100000);return &adapter_bus;}
bool ina226_pico_alert_low(void){return pin_low;}
#include "../../src/hal/power_monitor.c"
int main(void){
    test_ina226_core();m226_defaults(&sensor);pm_reading_t r={0};
    CHECK(pm_init()&&pm_present()&&pm_device_id()==0x2260);
    m226_set_sample(&sensor,1600,3200,1600,0,true);clock_ms=sensor.now=10;
    CHECK(pm_read(&r)&&r.ok&&r.new_sample&&r.uncalibrated&&!r.temperature_valid);
    CHECK(fabsf(r.v_bus-4.0f)<1e-6f&&fabsf(r.i-0.4f)<1e-6f);
    pm_diagnostic_t d={0};CHECK(pm_diagnostic(&d)&&d.raw_shunt==1600&&d.raw_bus==3200&&d.valid);
    pin_low=true;CHECK(pm_fault_latched()&&!pm_ack_fault());pin_low=false;
    m226_por(&sensor);clock_ms=sensor.now=20;
    CHECK(!pm_read(&r)&&!r.ok&&!pm_present()&&pm_fault_latched());
    CHECK(!pm_diagnostic(&d));
    clock_ms=sensor.now=2100;CHECK(!pm_read(&r)&&pm_present()&&pm_fault_latched());
    m226_set_sample(&sensor,100,3200,100,0,true);clock_ms=sensor.now=2110;
    CHECK(pm_read(&r)&&pm_ack_fault()&&!pm_fault_latched());
    CHECK(strcmp(pm_name(),"INA226 UNCAL")==0);
    printf("Power adapter + core: %u checks, %u failures; recovery, fault and telemetry quality\n",checks,failures);
    return failures?1:0;
}
