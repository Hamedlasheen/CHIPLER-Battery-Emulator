#ifndef CHIPLER_LIVE_PORT_H
#define CHIPLER_LIVE_PORT_H
#include "../batt/smbus_target.h"
uint32_t live_port_guard_enter(void *);
void live_port_guard_exit(void *,uint32_t);
bool live_port_start(st_target_t *);
void live_port_stop(void);
void live_port_service(uint32_t now_us);
bool live_port_fault(void);
void live_port_diagnostic(st_result_t *,uint32_t *irq,uint32_t *abort_source);
void live_port_clear_fault(void); /* explicit profile reload, while stopped */
void live_port_history(uint32_t out[8],uint8_t *site);
uint32_t live_port_last_event_us(void);  /* time_us_32() of the last bus event handled by the IRQ (review 102) */ /* last 8 IRQ batches (oldest first) + failing live_wire site */
#endif
