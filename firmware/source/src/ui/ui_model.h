/* Snapshot of everything the UI shows. Produced on core 0, copied to core 1 for rendering.
 * Measured vs simulated data are kept in separate, explicitly-flagged fields. */
#ifndef CHIPLER_UI_MODEL_H
#define CHIPLER_UI_MODEL_H
#include <stdint.h>
#include <stdbool.h>
#include "../safety/safety.h"
#include "../charge/charge_diag.h"

typedef enum {
    SCR_STARTUP = 0, SCR_DASH, SCR_PROTO, SCR_MENU, SCR_MODELS, SCR_SOC, SCR_CONFIRM, SCR_FAULT,
    SCR_CHG, SCR_CHG_MENU,           /* CHARGE TEST mode (CP-09, v0.3.0) - no hardware, no P1 UI entry */
    SCR_SETTINGS, SCR_CAL, SCR_TOUCHDIAG, SCR_BATT   /* P1_r1 touch UI */
} screen_t;
typedef enum { CONF_NONE = 0, CONF_SOC, CONF_MODEL, CONF_ARM, CONF_CHG_START, CONF_CLEAR_PROFILES } confirm_kind_t;
typedef enum { LINK_DISABLED = 0, LINK_IDLE, LINK_ACTIVE, LINK_ERROR } link_state_t;

#define SPARK_N 22

typedef struct {
    /* measured telemetry */
    telem_quality_t tq;
    float v, a, w;
    bool sensor_uncalibrated;
    /* emulated battery SOC (CR-003): requested = user-confirmed preset; applied = value the
     * protocol backend actually publishes (-1 = none); soc_result = soc_result_t */
    int soc_requested, soc_applied, soc_result;
    /* protocol */
    link_state_t link;
    uint8_t addr; bool addr_verified; bool bus_enabled;
    uint32_t txns, rx, tx, unknown, ignored, overflows;
    uint32_t replay_frames,replay_total,replay_errors;
    int last_reg;
    uint8_t spark[SPARK_N];   /* transactions per second history */
    /* profile */
    int prof_idx;
    /* output / safety */
    out_state_t out;
    fault_t fault; float fault_value;
    float lim_ov, lim_oc;
    /* CHARGE TEST mode. Last good sample is kept for the STALE view only. */
    bool t_have; uint32_t t_age_ms;      /* time since last fresh conversion */
    float lg_v, lg_a, lg_w;              /* last good values */
    chg_flow_t chg_flow;
    chg_checks_t chg_checks;
    bool chg_start_ok;
    bool chg_sink_fitted, chg_input_fitted;
    /* navigation */
    screen_t screen;
    int menu_cur, model_cur, soc_cur, chg_page, chg_menu_cur;
    int soc_pick;             /* OWNER r7: SOC value being edited on the picker (1..100) */
    confirm_kind_t confirm; int confirm_cur, confirm_arg, confirm_from;
    char toast[32]; uint32_t toast_until;
    uint32_t now_ms;
    uint8_t boot_progress;    /* 0..100 */
    /* P1_r1 */
    bool chg_ui;              /* CHARGE TEST screens reachable (CHG_UI_ENABLED; off while no sink hardware exists) */
    bool soc_capable;         /* active profile has a SOC backend; false = SOC control locked */
    uint8_t rot180;           /* display rotation in use */
    bool touch_cal_valid;     /* calibration loaded/performed */
    bool ina_present;         /* INA226 answered at boot */
    bool bus_stop_reason_set; /* a STOP/fault was requested by the user or a fault */
    bool settings_saved;      /* last settings write succeeded (false = RAM only) */
    const char *fw;
    const char *disp;         /* V02-04 build identity, e.g. "A \x80 GC9A01 BGR INV" */
    /* AUTO_LEARN build: controller state (auto_ctl.h). soc_requested above is OUR value sent in replies; the phone's own SOC
     * is never read back on this bus, so there is no "phone SOC" field. */
    bool auto_build;
    uint8_t auto_state, auto_bus;      /* auto_state_t, auto_bus_t */
    bool auto_have, auto_reused, auto_partial;
    uint32_t auto_fp, auto_gen;
    uint8_t auto_saved, auto_dist, auto_cov, auto_unc;
    uint32_t auto_reads, auto_drops;
    char auto_note[48];
} ui_model_t;

#endif
