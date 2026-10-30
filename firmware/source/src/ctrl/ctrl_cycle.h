/* One 100 Hz control step (V02-02). Pure, host-tested with a HAL mock.
 * Order is fixed: safety_update -> physical EN gate written -> return. Nothing in here logs or
 * waits on USB; the caller logs AFTER this returns (via the non-blocking ring). */
#ifndef CHIPLER_CTRL_CYCLE_H
#define CHIPLER_CTRL_CYCLE_H
#include <stdbool.h>
#include <stdint.h>
#include "../safety/safety.h"
#include "../log/log_ring.h"

typedef struct {
    void (*en_write)(void *ctx, bool on);   /* eFuse EN pin */
    void *ctx;
} ctrl_hal_t;

typedef struct {
    out_state_t before, after;
    bool entered_fault;
    bool en;
} ctrl_event_t;

ctrl_event_t ctrl_cycle(safety_t *s, uint32_t now, const safety_inputs_t *in, const batt_profile_t *p,
                        const ctrl_hal_t *hal);
/* diagnostics for an event, pushed to a ring (dropped if full; never blocks) */
void ctrl_log_event(log_ring_t *lr, uint32_t now, const ctrl_event_t *ev, const safety_t *s);
#endif
