#include "ctrl_cycle.h"

ctrl_event_t ctrl_cycle(safety_t *s, uint32_t now, const safety_inputs_t *in, const batt_profile_t *p,
                        const ctrl_hal_t *hal) {
    ctrl_event_t ev;
    ev.before = s->state;
    safety_update(s, now, in, p);
    ev.en = safety_output_gate(s, p);
    hal->en_write(hal->ctx, ev.en);           /* physical gate FIRST, every cycle (R-02, V02-02) */
    ev.after = s->state;
    ev.entered_fault = ev.after == OUT_FAULT && ev.before != OUT_FAULT;
    return ev;
}

void ctrl_log_event(log_ring_t *lr, uint32_t now, const ctrl_event_t *ev, const safety_t *s) {
    if (ev->entered_fault)
        lr_printf(lr, now, "SAFE", "FAULT %s value=%.3f (EN already %s)", fault_name(s->fault),
                  (double)s->fault_value, ev->en ? "ON" : "OFF");
}
