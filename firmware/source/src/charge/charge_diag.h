/* CHARGE TEST mode (CP-09 / CR-004-r1), merged into the main firmware UI in v0.3.0-dev.
 *
 * What this module IS: pure logic that classifies the battery-terminal current direction from the
 * existing measurement, and evaluates the start-readiness checklist. Host-tested.
 * What it is NOT: a charge sink, a charger-input sensor or a test runner. None of that hardware
 * exists (CHG_SINK_FITTED = 0, CHG_INPUT_SENSOR_FITTED = 0 in board_config.h), so a charge test
 * can never start and no result other than NOT EVALUATED can be produced. There is deliberately
 * no pass, healthy or within-limits outcome in firmware.
 *
 * Sign convention: battery-terminal current > 0 = emulator supplies the phone;
 *                  current < 0 = current flows from the phone into the emulator (charge current). */
#ifndef CHIPLER_CHARGE_DIAG_H
#define CHIPLER_CHARGE_DIAG_H
#include <stdint.h>
#include <stdbool.h>
#include "../safety/safety.h"

#define CHG_I_THRESH_A    0.020f   /* |I| below this = no net current. PROVISIONAL */
#define CHG_STALE_SHOW_MS 5000u    /* last good values are shown greyed as STALE up to this age */
#define CHG_PAGES         6        /* overview, charger input, battery terminal, protocol, SOC, result */

typedef enum {
    CHF_UNAVAILABLE = 0,   /* no sample (sensor absent / never read / too old) */
    CHF_STALE,             /* had a sample, no fresh conversion now: values shown greyed */
    CHF_SUPPLYING,         /* emulator -> phone */
    CHF_RECEIVING,         /* phone -> emulator */
    CHF_NO_NET             /* |I| < threshold */
} chg_flow_t;

typedef enum { CHK_FAIL = 0, CHK_PASS, CHK_UNKNOWN } chk_t;
enum { CHK_PROFILE = 0, CHK_SINK, CHK_SENSORS, CHK_CHARGER, CHK_RECIPE, CHK_N };

typedef struct { uint8_t c[CHK_N]; } chg_checks_t;

/* tq: quality of the current sample; have_sample/age_ms: time since the last fresh sample;
 * i: signed battery-terminal current of the sample being classified (last good if stale). */
chg_flow_t chg_flow(telem_quality_t tq, bool have_sample, uint32_t age_ms, float i);

void chg_eval_checks(chg_checks_t *k, bool profile_verified, bool sink_ready, telem_quality_t tq,
                     bool input_sensor_present, bool charger_detected, bool recipe_loaded);
/* true only when every check is PASS (UNKNOWN counts as not ready) */
bool chg_start_allowed(const chg_checks_t *k);
const char *chg_flow_str(chg_flow_t f);
const char *chk_name(int i);
#endif
