#include "power_monitor.h"
#include "pico/stdlib.h"
#include "board_io.h"
#include "board_config.h"
#include <math.h>
/* Explicit compile-time legacy backend; never probe/fallback between unrelated chips. */
#ifdef CHIPLER_PM_USE_INA228
#include "ina228.h"
bool pm_init(void) {return ina228_init(SHUNT_OHMS,CURRENT_MAX_A)&&ina228_set_bus_ov_alert(LIMIT_OV_V);}
bool pm_present(void) {return ina228_present();}
uint16_t pm_device_id(void) {return ina228_device_id();}
const char *pm_status(void) {return ina_status_str(ina228_status());}
const char *pm_name(void) {return "INA228";}
bool pm_diagnostic(pm_diagnostic_t *r) {(void)r;return false;}
bool pm_fault_latched(void) {return pm_alert_active();}
bool pm_ack_fault(void) {return !pm_alert_active();}
bool pm_read(pm_reading_t *r) {
    ina228_reading_t old={0}; bool ok=ina228_read(&old);
    r->ok=ok;r->new_sample=ok&&old.new_sample;r->temperature_valid=ok;r->uncalibrated=true;
    if(ok) {r->v_bus=old.v_bus;r->i=old.i;r->p=old.p;r->t_die=old.t_die;}return ok;
}
#else
#include "ina226_core.h"
#include "ina226_pico.h"
static ina226_dev_t dev;
static i226_sample_t sample;
static const ina226_bus_t *bus;
static bool constructed;
static uint32_t last_retry;
static const i226_cfg_t cfg={SHUNT_OHMS,0.000250f,0x4127,INA_SOL_TRIP_A,false};
bool pm_init(void) {
    /* Owner-selected R010, not the obsolete 5mOhm config value. */
    bus=ina226_pico_bus_init(100000);
    i226_status_t st=constructed ? ina226_reinit(&dev,bus,&cfg) : ina226_init(&dev,bus,&cfg);
    constructed=true;last_retry=to_ms_since_boot(get_absolute_time());return st==I226_OK;
}
bool pm_present(void) {return constructed&&dev.configured;}
uint16_t pm_device_id(void) {return dev.die_read?dev.die_id:0;}
const char *pm_status(void) {return i226_status_str(dev.status);}
const char *pm_name(void) {return "INA226 UNCAL";}
bool pm_diagnostic(pm_diagnostic_t *r) {
    if(!r||!pm_present()||!sample.valid||ina226_stale(&dev,to_ms_since_boot(get_absolute_time())))return false;
    r->raw_shunt=sample.raw_shunt;r->raw_current=sample.raw_current;r->raw_bus=sample.raw_bus;
    r->raw_power=sample.raw_power;r->seq=sample.seq;r->valid=true;return true;
}
bool pm_fault_latched(void) {
    if(constructed&&ina226_pico_alert_low()) ina226_alert_pin_event(&dev);
    return constructed&&dev.fault_latched;
}
bool pm_ack_fault(void) {
    if(!pm_present()||ina226_pico_alert_low()) return false;
    ina226_fault_ack(&dev);return true;
}
bool pm_read(pm_reading_t *r) {
    uint32_t now=to_ms_since_boot(get_absolute_time());
    r->ok=false;r->new_sample=false;r->temperature_valid=false;r->uncalibrated=true;
    if(!constructed) return false;
    if(ina226_pico_alert_low()) ina226_alert_pin_event(&dev);
    if(!dev.configured) {
        if((uint32_t)(now-last_retry)>=2000) {last_retry=now;ina226_reinit(&dev,bus,&cfg);}
        return false;
    }
    if(!ina226_read(&dev,&sample,now)||ina226_stale(&dev,to_ms_since_boot(get_absolute_time()))) return false;
    r->v_bus=sample.v_bus;r->i=sample.i;r->p=sample.p_signed;
    r->ok=true;r->new_sample=sample.new_conversion;return true;
}
#endif
