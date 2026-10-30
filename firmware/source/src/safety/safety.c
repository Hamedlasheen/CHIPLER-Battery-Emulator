#include "safety.h"

void safety_init(safety_t *s, const safety_limits_t *lim) {
    s->state = OUT_OFF; s->fault = FLT_NONE; s->fault_value = 0;
    s->armed_at = s->uv_since = 0; s->uv_pending = false;
    s->lim = *lim;
}

static void trip(safety_t *s, fault_t f, float v) {
    s->state = OUT_FAULT; s->fault = f; s->fault_value = v; s->uv_pending = false;
}

static bool vin_over(const safety_t *s, const safety_inputs_t *in) {
    return in->v_in_valid && in->v_in > s->lim.ov_v;
}

arm_result_t safety_request_arm(safety_t *s, uint32_t now, const safety_inputs_t *in,
                                const batt_profile_t *p) {
    if (s->state != OUT_OFF) return ARM_REFUSED_STATE;
    if (!profile_armable(p)) return ARM_REFUSED_PROFILE;    /* first: telemetry never overrides */
    if (in->q != TQ_MEASURED) return ARM_REFUSED_MONITOR;   /* never power a phone blind */
    if (vin_over(s, in) || (in->v_in_valid && in->v_in < s->lim.uv_v)) return ARM_REFUSED_VIN;
    s->state = OUT_ARMED; s->armed_at = now;
    return ARM_OK;
}

bool safety_confirm(safety_t *s, uint32_t now, const batt_profile_t *p) {
    if (s->state != OUT_ARMED) return false;
    if (!profile_armable(p)) { s->state = OUT_OFF; return false; }
    if ((uint32_t)(now - s->armed_at) > s->lim.arm_timeout_ms) { s->state = OUT_OFF; return false; }
    s->state = OUT_ON; s->uv_pending = false;
    return true;
}

void safety_off(safety_t *s) { if (s->state != OUT_FAULT) s->state = OUT_OFF; }
void safety_cancel(safety_t *s) { if (s->state == OUT_ARMED) s->state = OUT_OFF; }

void safety_update(safety_t *s, uint32_t now, const safety_inputs_t *in, const batt_profile_t *p) {
    if (s->state == OUT_FAULT) return;
    if ((s->state == OUT_ARMED || s->state == OUT_ON) && !profile_armable(p)) {
        trip(s, FLT_UNSUPPORTED_MODEL, 0); return;
    }
    /* input overvoltage is a fault in every state (warn before the user tries to arm) */
    if (vin_over(s, in)) { trip(s, FLT_OVERVOLTAGE, in->v_in); return; }
    if (s->state == OUT_ARMED && (uint32_t)(now - s->armed_at) > s->lim.arm_timeout_ms) {
        s->state = OUT_OFF; return;
    }
    if (s->state != OUT_ON && s->state != OUT_ARMED) return;
    if (in->q != TQ_MEASURED) { trip(s, FLT_NO_MONITOR, 0); return; }
    if (in->alert_pin) { trip(s, FLT_ALERT_PIN, 0); return; }
    if (in->v_out > s->lim.ov_v) { trip(s, FLT_OVERVOLTAGE, in->v_out); return; }
    if (in->i_out > s->lim.oc_a) { trip(s, FLT_OVERCURRENT, in->i_out); return; }
    if (s->state == OUT_ON) {
        if (in->v_out < s->lim.uv_v) {
            if (!s->uv_pending) { s->uv_pending = true; s->uv_since = now; }
            else if ((uint32_t)(now - s->uv_since) >= s->lim.uv_hold_ms) trip(s, FLT_UNDERVOLTAGE, in->v_out);
        } else s->uv_pending = false;
    }
}

bool safety_ack(safety_t *s, const safety_inputs_t *in) {
    if (s->state != OUT_FAULT) return false;
    if (vin_over(s, in) || in->alert_pin) return false;  /* condition still present */
    s->state = OUT_OFF; s->fault = FLT_NONE;
    return true;
}

const char *fault_name(fault_t f) {
    switch (f) {
    case FLT_NONE: return "NONE";
    case FLT_OVERVOLTAGE: return "OVERVOLTAGE";
    case FLT_UNDERVOLTAGE: return "UNDERVOLTAGE";
    case FLT_OVERCURRENT: return "OVERCURRENT";
    case FLT_NO_MONITOR: return "NO POWER MONITOR";
    case FLT_ALERT_PIN: return "MONITOR ALERT";
    case FLT_UNSUPPORTED_MODEL: return "UNSUPPORTED MODEL";
    case FLT_WIRING: return "WIRING FAULT";
    }
    return "?";
}
const char *fault_code(fault_t f) {
    static const char *c[] = {"E-00", "E-OV-01", "E-UV-02", "E-OC-03", "E-PM-04", "E-AL-05", "E-MD-06", "E-WR-07"};
    return (unsigned)f < 8 ? c[f] : "E-??";
}
const char *arm_result_str(arm_result_t r) {
    switch (r) {
    case ARM_OK: return "ARMED";
    case ARM_REFUSED_STATE: return "NOT ALLOWED NOW";
    case ARM_REFUSED_MONITOR: return "NO POWER MONITOR";
    case ARM_REFUSED_PROFILE: return "PROFILE LOCKED";
    case ARM_REFUSED_VIN: return "PSU VOLTAGE OUT OF RANGE";
    }
    return "?";
}
