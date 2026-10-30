#include "battery_bus.h"
#include "board_config.h"
#include "bus_en.h"
#include "pico/stdlib.h"
#include "pico/i2c_slave.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "../batt/target_prepare.h"
#include "../batt/live_setup.h"
#include "../batt/live_prepare.h"
#include "live_port.h"
#include <string.h>

static target_engine_t s_eng;
static bool s_running;
static bool s_prepared;
static bool s_live;
static st_target_t s_smb;
static struct {uint32_t txns,rx,tx,unknown,writes,errors;} s_base;
static live_setup_t s_setup;
#if CHIPLER_AUTO_LEARN
static uint32_t s_obs_drop_base;   /* observations dropped by earlier target instances (st_init clears the live counter) */
#endif   /* evidence gate + selector pages; built by live_setup_prepare (review 105 R3) */
/* IRQ context: keep minimal. FIFO access only; logging is drained by the main loop. */
static void __not_in_flash_func(on_i2c)(i2c_inst_t *i2c, i2c_slave_event_t ev) {
    uint32_t t = time_us_32();
    switch (ev) {
    case I2C_SLAVE_RECEIVE:
        while (i2c_get_read_available(i2c)) te_on_receive(&s_eng, i2c_read_byte_raw(i2c), t);
        break;
    case I2C_SLAVE_REQUEST:
        i2c_write_byte_raw(i2c, te_on_request(&s_eng, t));
        break;
    case I2C_SLAVE_FINISH:
        te_on_finish(&s_eng);
        break;
    }
}

void battery_bus_init(void) {
    bus_en_init();
    /* leave SDA/SCL as high-impedance inputs while stopped */
    gpio_init(PIN_BAT_SDA); gpio_set_dir(PIN_BAT_SDA, GPIO_IN); gpio_disable_pulls(PIN_BAT_SDA);
    gpio_init(PIN_BAT_SCL); gpio_set_dir(PIN_BAT_SCL, GPIO_IN); gpio_disable_pulls(PIN_BAT_SCL);
    te_init(&s_eng, 0);
    s_running = false;
}

bool battery_bus_start(const batt_profile_t *p) {
    return battery_bus_prepare(p,50) && battery_bus_activate();
}
bool battery_bus_prepare(const batt_profile_t *p,int soc) {
    battery_bus_stop();
    if(live_port_fault())return false;
    s_live=p&&(!strcmp(p->id,"SMB-BENCH")||!strcmp(p->id,"CAP12PM-B1")||profile_uses_12pm_backend(p));
    if(s_live){
#if CHIPLER_AUTO_LEARN
        s_obs_drop_base+=s_smb.obs_dropped;   /* review fix 2: keep capture loss monotonic across target restarts */
#endif
        st_init(&s_smb,0x0B);
        memset(&s_base,0,sizeof s_base);
        st_set_guard(&s_smb,live_port_guard_enter,live_port_guard_exit,0);
        s_prepared=live_setup_prepare(&s_setup,&s_smb,p,soc,0);
        if(s_prepared)s_eng.prof=p;
        return s_prepared;
    }
    s_prepared=target_prepare(&s_eng,p,soc)==TP_OK;
    return s_prepared;
}
bool battery_bus_activate(void) {
    if(!s_prepared || s_running || !s_eng.prof) return false;
    if(s_live){s_running=live_port_start(&s_smb);return s_running;}
    gpio_set_function(PIN_BAT_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_BAT_SCL, GPIO_FUNC_I2C);
    /* No internal pull-ups: the bus owner (phone or bench master) provides them; MCU-side
     * pull-ups belong to the translator circuit. */
    i2c_init(BAT_I2C_INSTANCE, 400 * 1000);
    i2c_slave_init(BAT_I2C_INSTANCE, s_eng.prof->i2c_addr, &on_i2c);
    irq_set_priority(I2C0_IRQ + i2c_hw_index(BAT_I2C_INSTANCE), PICO_HIGHEST_IRQ_PRIORITY);
    bus_en_put(true);
    s_running = true;
    return true;
}

bb_start_t battery_bus_start_seq(const batt_profile_t *p,int soc,bool (*initial)(void *),void *ctx) {
    if(!battery_bus_prepare(p,soc))return BB_START_PREPARE;
    /* review 105 R1: the target is prepared but NOT addressable yet (I2C block off, address NACKed). Whatever
     * initial() publishes is in the map before the first ACK; a failure keeps the target off. */
    if(initial&&!initial(ctx)){battery_bus_stop();return BB_START_WORDS;}
    return battery_bus_activate()?BB_START_OK:BB_START_ACTIVATE;
}
bool battery_bus_pause(void (*fn)(void *),void *ctx) {
    if(!s_live||!s_running||live_port_fault())return false;
    /* review 105 R2: the target is switched off first (IRQ disabled, I2C block disabled and reset: the address is
     * NACKed, SCL is never held), fn runs with nothing that can be addressed, then the SAME prepared map is re-armed. */
    live_port_stop();
    s_running=false;
    fn(ctx);
    s_running=live_port_start(&s_smb);
    return s_running;
}

void battery_bus_stop(void) {
    bus_en_put(false);
    if(s_live)live_port_stop();
    else if (s_running) {
        i2c_slave_deinit(BAT_I2C_INSTANCE);
        i2c_deinit(BAT_I2C_INSTANCE);
    }
    gpio_init(PIN_BAT_SDA); gpio_set_dir(PIN_BAT_SDA, GPIO_IN); gpio_disable_pulls(PIN_BAT_SDA);
    gpio_init(PIN_BAT_SCL); gpio_set_dir(PIN_BAT_SCL, GPIO_IN); gpio_disable_pulls(PIN_BAT_SCL);
    s_running = false;
    s_prepared = false;
    s_live = false;
    te_on_finish(&s_eng);
    te_init(&s_eng,0);
}

#if CHIPLER_AUTO_LEARN
void battery_bus_set_listen(bool on){if(s_live)st_set_listen(&s_smb,on);}
bool battery_bus_listening(void){return s_live&&s_smb.listen;}
bool battery_bus_pop_obs(uint32_t *o){return s_live&&st_pop_obs(&s_smb,o);}
uint32_t battery_bus_obs_dropped(void){return s_obs_drop_base+s_smb.obs_dropped;}   /* monotonic for the whole boot */
#endif
bool battery_bus_running(void) { return s_running&&!live_port_fault(); }
bool battery_bus_fault(void) {return live_port_fault();}
void battery_bus_reset_fault(void) {battery_bus_stop();live_port_clear_fault();}
bool battery_bus_publish(uint8_t cmd,const uint8_t *data,uint8_t len) {
    if(!s_prepared||live_port_fault())return false;
    return s_live?live_publish_soc(&s_smb,cmd,data,len):te_publish(&s_eng,cmd,data,len);
}
bool battery_bus_history(uint32_t h[8],uint8_t *site) {
    if(!s_live||!h)return false;
    uint32_t g=live_port_guard_enter(0);live_port_history(h,site);live_port_guard_exit(0,g);return true;
}
bool battery_bus_word(uint8_t cmd,uint16_t *v) {
    if(!s_live||!s_prepared||!v)return false;
    uint32_t g=live_port_guard_enter(0);bool ok=st_get_word(&s_smb,cmd,v);
    live_port_guard_exit(0,g);return ok;
}
uint32_t battery_bus_idle_us(uint32_t now_us) {
    if(!s_live)return UINT32_MAX;
    int32_t age=(int32_t)(now_us-live_port_last_event_us());   /* the IRQ may stamp after now_us was sampled */
    return age<0?0u:(uint32_t)age;
}
void battery_bus_service(uint32_t now_us) {
    if(!s_live)return;
    live_port_service(now_us);
    uint32_t g=live_port_guard_enter(0);
    s_eng.txns=s_smb.txns-s_base.txns;s_eng.rx_bytes=s_smb.rx_bytes-s_base.rx;s_eng.tx_bytes=s_smb.tx_bytes-s_base.tx;
    s_eng.unknown_reads=s_smb.unknown_reads-s_base.unknown;s_eng.ignored_writes=s_smb.writes_ok-s_base.writes;
    s_eng.overflows=s_smb.overreads+s_smb.bad_len+s_smb.aborts+s_smb.queue_dropped-s_base.errors;
    s_eng.last_reg=s_smb.have_last_cmd?s_smb.last_cmd:s_eng.last_reg;
    ag_poll_counters(&s_setup.auth,s_smb.unknown_reads,s_smb.have_last_cmd?s_smb.last_cmd:0xFF);
    live_port_guard_exit(0,g);
}
bool battery_bus_pop_write(st_write_t *out){return s_live&&st_pop_write(&s_smb,out);}
bool battery_bus_auth_status(battery_bus_auth_t *a){
    if(!a)return false;
    a->auth_state=(int)ag_auth_state(&s_setup.auth);
    a->auth_str=ag_auth_state_str(&s_setup.auth);
    a->refused_reads=s_setup.auth.refused_reads;a->refused_writes=s_setup.auth.refused_writes;
    a->dynamic_refusals=s_setup.auth.dynamic_refusals;a->servable=ag_servable_count(&s_setup.auth);
    a->selector_pages=s_setup.pages.applied;a->unknown_pages=s_setup.pages.unknown_pages;
    a->last_selector_target=sp_last_target(&s_setup.pages);
    a->last_refused_cmd=s_setup.auth.have_last_refused?s_setup.auth.last_refused_cmd:0xFF;
    return true;
}
const char *battery_bus_auth_state_str(void){return ag_auth_state_str(&s_setup.auth);}
void battery_bus_clear_counters(void) {
    uint32_t g=live_port_guard_enter(0);
    if(s_live){
        s_base.txns=s_smb.txns;s_base.rx=s_smb.rx_bytes;s_base.tx=s_smb.tx_bytes;
        s_base.unknown=s_smb.unknown_reads;s_base.writes=s_smb.writes_ok;
        s_base.errors=s_smb.overreads+s_smb.bad_len+s_smb.aborts+s_smb.queue_dropped;
    }
    te_clear_counters(&s_eng);live_port_guard_exit(0,g);
}
bool battery_bus_diagnostic(battery_bus_diag_t *d) {
    if(!s_live||!d)return false;
    uint32_t g=live_port_guard_enter(0);
    d->reads=s_smb.reads_done;d->short_reads=s_smb.reads_short;d->writes=s_smb.writes_ok;
    d->errors=s_smb.bad_len+s_smb.writes_bad+s_smb.unknown_reads+s_smb.aborts+s_smb.overreads;
    d->command=s_smb.have_last_cmd?s_smb.last_cmd:0xFF;
    live_port_diagnostic(&d->fault,&d->irq,&d->abort_source);
    live_port_guard_exit(0,g);return true;
}
target_engine_t *battery_bus_engine(void) { return &s_eng; }
