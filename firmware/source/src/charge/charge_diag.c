#include "charge_diag.h"
#include <math.h>

chg_flow_t chg_flow(telem_quality_t tq, bool have_sample, uint32_t age_ms, float i) {
    if (tq == TQ_ABSENT) {
        if (have_sample && age_ms <= CHG_STALE_SHOW_MS) return CHF_STALE;
        return CHF_UNAVAILABLE;
    }
    if (i != i) return CHF_UNAVAILABLE;               /* NaN guard */
    if (fabsf(i) < CHG_I_THRESH_A) return CHF_NO_NET;
    return i > 0 ? CHF_SUPPLYING : CHF_RECEIVING;
}

void chg_eval_checks(chg_checks_t *k, bool profile_verified, bool sink_ready, telem_quality_t tq,
                     bool input_sensor_present, bool charger_detected, bool recipe_loaded) {
    k->c[CHK_PROFILE] = profile_verified ? CHK_PASS : CHK_FAIL;
    k->c[CHK_SINK]    = sink_ready ? CHK_PASS : CHK_FAIL;
    k->c[CHK_SENSORS] = tq == TQ_MEASURED ? CHK_PASS : CHK_FAIL;   /* simulated never counts */
    k->c[CHK_CHARGER] = !input_sensor_present ? CHK_UNKNOWN : (charger_detected ? CHK_PASS : CHK_FAIL);
    k->c[CHK_RECIPE]  = recipe_loaded ? CHK_PASS : CHK_FAIL;
}

bool chg_start_allowed(const chg_checks_t *k) {
    for (int i = 0; i < CHK_N; i++) if (k->c[i] != CHK_PASS) return false;
    return true;
}

const char *chg_flow_str(chg_flow_t f) {
    static const char *s[] = {"UNAVAILABLE", "STALE", "SUPPLYING", "RECEIVING", "NO_NET"};
    return (unsigned)f < 5 ? s[f] : "?";
}

const char *chk_name(int i) {
    static const char *s[] = {"VERIFIED PROFILE", "SINK READY + LIMITS", "SENSORS FRESH", "CHARGER DETECTED",
                              "TEST RECIPE LOADED"};
    return (unsigned)i < CHK_N ? s[i] : "?";
}
