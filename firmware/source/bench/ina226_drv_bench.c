/* CR006A-r2, Codex one-time owner-authorized implementation while Claude limit expired.
 * Isolated dummy-load bench. No phone. OUT_EN stays low. Main firmware unchanged.
 * Log production is bounded; USB consumption never waits. Poll deadline is 20ms.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "tusb.h"
#include "board_config.h"
#include "hal/board_io.h"
#include "hal/ina226_core.h"
#include "hal/ina226_pico.h"
#include "log/log.h"
#include "ina226_bench_policy.h"
#define REV "CR006A-r2-CODEX"
static const i226_cfg_t CFG = {0.010f, 0.000250f, 0x4127, 0.60f, false};

/* Direct TinyUSB FIFO write: no stdio timeout or newline expansion. Advance only
 * accepted bytes, bounded to one contiguous ring span / 256 bytes per pass. */
static void pump_nonblocking(void) {
    if (!tud_cdc_connected()) return;
    size_t n = lr_used(&g_log), available = tud_cdc_write_available();
    size_t offset = g_log.tail % g_log.cap, contiguous = g_log.cap - offset;
    if (n > available) n = available;
    if (n > LOG_PUMP_BUDGET) n = LOG_PUMP_BUDGET;
    if (n > contiguous) n = contiguous;
    if (!n) return;
    g_log.tail += tud_cdc_write(g_log.buf + offset, (uint32_t)n);
    tud_cdc_write_flush();
}
static void print_dev(const ina226_dev_t *d, i226_status_t st) {
    char fe[8] = "----", ff[8] = "----";
    if (d->mfr_read) snprintf(fe,sizeof fe,"%04X",d->mfr_id);
    if (d->die_read) snprintf(ff,sizeof ff,"%04X",d->die_id);
    LOG("DRV", "%s init=%s FE=%s FF=%s fault=%d", REV,i226_status_str(st),fe,ff,d->fault_latched);
    if (st == I226_OK)
        LOG("DRV", "CONFIG=%04X CAL=%04X ALERT=%04X MASK=%04X UNCAL; protection=UNQUALIFIED",d->config,d->cal,d->alert_limit,d->mask);
    else LOG("DRV", "step=%s reg=%02X rc=%d",d->fail_step ? d->fail_step : "?",d->fail_reg,ina226_pico_last_rc());
}
int main(void) {
    board_io_init(); stdio_init_all(); log_init();
    const ina226_bus_t *bus = ina226_pico_bus_init(100000);
    ina226_dev_t d; i226_sample_t s = {0};
    i226_status_t st = ina226_init(&d,bus,&CFG);
    LOG("DRV", "%s dummy load only; R=10mOhm PROVISIONAL; SOL=0.60A; OUT_EN=OFF",REV);
    print_dev(&d,st);
    uint32_t now=to_ms_since_boot(get_absolute_time()), next=now, missed=0;
    uint32_t next_print=now+1000, next_summary=now+10000, last_init=now;
    uint32_t reads=0,ok=0,bad=0,reinit=0,alert_events=0;
    for (;;) {
        now=to_ms_since_boot(get_absolute_time());
        if (ina226_pico_alert_low()) {
            if (!d.fault_latched) alert_events++;
            ina226_alert_pin_event(&d);
        }
        if (i226_poll_due(now,&next,&missed)) {
            reads++;
            if (ina226_read(&d,&s,now)) ok++; else bad++;
            if (!d.configured && (uint32_t)(now-last_init)>=2000) {
                last_init=now; reinit++;
                st=ina226_reinit(&d,bus,&CFG); /* never acknowledges a fault */
                print_dev(&d,st);
            }
        }
        now=to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now-next_print)>=0) {
            next_print=now+1000;
            if (s.valid && !ina226_stale(&d,now))
                LOG("DRV", "#%lu V=%.3f I=%+.2fmA P=%+.3fW acq=%lums fault=%d UNCAL",(unsigned long)s.seq,
                    (double)s.v_bus,(double)(s.i*1000),(double)s.p_signed,(unsigned long)(s.t_end_ms-s.t_start_ms),d.fault_latched);
            else LOG("DRV", "INVALID %s stale=%d reg=%02X rc=%d fault=%d",i226_status_str(d.status),ina226_stale(&d,now),d.fail_reg,ina226_pico_last_rc(),d.fault_latched);
        }
        if ((int32_t)(now-next_summary)>=0) {
            next_summary=now+10000;
            LOG("DRV", "summary reads=%lu ok=%lu invalid=%lu reinit=%lu",(unsigned long)reads,(unsigned long)ok,(unsigned long)bad,(unsigned long)reinit);
            LOG("DRV", "missed_slots=%lu log_dropped=%lu alerts=%lu fault=%d",(unsigned long)missed,(unsigned long)log_dropped(),(unsigned long)alert_events,d.fault_latched);
        }
        pump_nonblocking(); sleep_ms(1);
    }
}
