#ifndef CHIPLER_LIVE_WIRE_H
#define CHIPLER_LIVE_WIRE_H
#include "smbus_target.h"
/* Normalized RP2350 IRQ batch. Kept pure so FIFO/boundary ordering is testable. */
typedef struct {uint16_t rx[16];uint8_t count;bool request,nack,stop,error;} live_event_t;
typedef struct {st_target_t *target;bool blocked;uint32_t last_us;st_result_t fault;uint8_t site;} live_wire_t;
/* site = which live_wire check isolated the port (diagnostics only):
 * 1 error/overflow  2 ambiguous boundary  3 data byte without START  4 st_on_rx refused
 * 5 begin_read refused  6 st_on_tx refused  7 st_on_stop result  8 idle expire */
typedef struct {bool have_tx;uint8_t tx;} live_reply_t;
void live_wire_init(live_wire_t *,st_target_t *,uint32_t now_us);
bool live_wire_event(live_wire_t *,const live_event_t *,uint32_t now_us,live_reply_t *);
bool live_wire_expire(live_wire_t *,uint32_t now_us,uint32_t max_idle_us);
#endif
