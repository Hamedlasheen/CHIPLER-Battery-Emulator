/* Output-power safety state machine (pure logic, host-testable).
 * The phone power path (eFuse EN) is ON only in state OUT_ON. Everything else = OFF.
 * The eFuse hardware OV/OC protection is the primary protection; this is the second layer. */
#ifndef CHIPLER_SAFETY_H
#define CHIPLER_SAFETY_H
#include <stdint.h>
#include <stdbool.h>
#include "../batt/profile.h"

typedef enum { TQ_ABSENT = 0, TQ_SIMULATED, TQ_MEASURED } telem_quality_t;
typedef enum { OUT_OFF = 0, OUT_ARMED, OUT_ON, OUT_FAULT } out_state_t;
typedef enum {
    FLT_NONE = 0, FLT_OVERVOLTAGE, FLT_UNDERVOLTAGE, FLT_OVERCURRENT, FLT_NO_MONITOR,
    FLT_ALERT_PIN, FLT_UNSUPPORTED_MODEL, FLT_WIRING
} fault_t;
typedef enum {
    ARM_OK = 0, ARM_REFUSED_STATE, ARM_REFUSED_MONITOR, ARM_REFUSED_PROFILE, ARM_REFUSED_VIN
} arm_result_t;

typedef struct {
    telem_quality_t q;
    float v_out, i_out;      /* measured at adapter (INA228) */
    float v_in; bool v_in_valid;
    bool alert_pin;          /* INA228 ALERT asserted */
} safety_inputs_t;

typedef struct {
    float ov_v, uv_v, oc_a;
    uint32_t arm_timeout_ms, uv_hold_ms;
} safety_limits_t;

typedef struct {
    out_state_t state;
    fault_t fault;
    float fault_value;
    uint32_t armed_at, uv_since;
    bool uv_pending;
    safety_limits_t lim;
} safety_t;

void safety_init(safety_t *s, const safety_limits_t *lim);
arm_result_t safety_request_arm(safety_t *s, uint32_t now, const safety_inputs_t *in,
                                const batt_profile_t *p);
bool safety_confirm(safety_t *s, uint32_t now, const batt_profile_t *p);
void safety_off(safety_t *s);
void safety_cancel(safety_t *s);
bool safety_ack(safety_t *s, const safety_inputs_t *in);
/* Re-validates the active profile every cycle: an unqualified profile in ARMED/ON is forced
 * to FAULT (defence in depth; unreachable in normal flow because arming already refuses it). */
void safety_update(safety_t *s, uint32_t now, const safety_inputs_t *in, const batt_profile_t *p);
static inline bool safety_output_enabled(const safety_t *s) { return s->state == OUT_ON; }
/* The ONLY value ever written to the eFuse EN pin (Codex R-02): ON state AND a qualified
 * profile. Evaluated independently of safety_update(), so even a corrupted/forced ON state with
 * an unverified profile drives EN low in the same control cycle. */
static inline bool safety_output_gate(const safety_t *s, const batt_profile_t *p) {
    return s->state == OUT_ON && profile_armable(p);
}
const char *fault_name(fault_t f);
const char *fault_code(fault_t f);
const char *arm_result_str(arm_result_t r);
#endif
