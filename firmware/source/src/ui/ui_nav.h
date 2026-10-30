/* Three-button navigation (CR-003-r1). Pure logic, host-tested.
 *
 *  Global:  OK long (1.5 s) = OUTPUT OFF, on every screen. It never confirms anything.
 *  DASH  :  UP/DOWN -> PROTO page          OK -> MENU
 *  PROTO :  UP/DOWN -> DASH page           OK -> MENU
 *  MENU  :  UP/DOWN move, OK selects:  Emulated SOC | Phone model | Enable output |
 *           Output OFF | Protocol status | Clear I2C counters | Back
 *  SOC   :  25% | 50% | 100% | Back   (cursor starts on the applied/requested value)
 *           OK on a value != current -> CONFIRM "Apply emulated SOC a% -> b%?"
 *  MODELS:  Back | <33 profiles>      OK on a profile -> CONFIRM "Load profile?" (output OFF only)
 *  CONFIRM: [CANCEL] [APPLY]; cursor ALWAYS starts on CANCEL; UP/DOWN toggle; OK executes the
 *           highlighted choice; 10 s without input -> cancel. Needs a fresh OK press (buttons.c).
 *  FAULT :  OK -> acknowledge (output stays OFF). Shown automatically on any fault.
 *  CHG   :  (MENU -> CHARGE TEST) UP/DOWN = previous/next of 6 pages, OK -> CHARGE MENU
 *  CHG_MENU: Start charge test | Stop test | Emulated SOC | View result | Output OFF |
 *           Exit to menu | Back.   Start -> CONFIRM (checklist). While any check fails the
 *           cursor cannot leave CANCEL. Firmware has no sink: start is always refused.
 * Changing SOC and enabling output are separate actions; SOC never enables output.
 */
#ifndef CHIPLER_UI_NAV_H
#define CHIPLER_UI_NAV_H
#include <stdint.h>
#include "../input/buttons.h"
#include "ui_model.h"

typedef enum {
    MI_SOC = 0, MI_MODEL, MI_CHARGE, MI_ENABLE, MI_OFF, MI_PROTO, MI_CLEAR, MI_BATT, MI_SETTINGS, MI_BACK, MI_COUNT
} menu_item_t;
#define SOC_ITEMS 4            /* 25, 50, 100, Back */
/* OWNER r7 SOC picker (touch indices on SCR_SOC): 0 -10, 1 -1, 2 +1, 3 +10, 4 APPLY, 5 25 %, 6 50 %, 7 100 %.
 * soc_cur = OWNER_SOC_BACK leaves the screen. */
#define OWNER_SOC_APPLY 4
#define OWNER_SOC_PRESET0 5
#define OWNER_SOC_TOUCH_ITEMS 8
#define OWNER_SOC_BACK 99
typedef enum {
    CMI_START = 0, CMI_STOP, CMI_SOC, CMI_RESULT, CMI_OFF, CMI_EXIT, CMI_BACK, CMI_COUNT
} chg_menu_item_t;
#define CONFIRM_TIMEOUT_MS 10000u

typedef enum {
    ACT_NONE = 0, ACT_OUTPUT_OFF, ACT_ARM_REQUEST, ACT_ARM_CONFIRM, ACT_ARM_CANCEL,
    ACT_LOAD_PROFILE, ACT_SOC_APPLY, ACT_ACK_FAULT, ACT_CLEAR_COUNTERS,
    ACT_CHG_START, ACT_CHG_STOP,
    ACT_ROTATE,                 /* P1_r1: flip the display 180 degrees (touch mapping follows) */
    ACT_AUTO_ON, ACT_RELEARN, ACT_CLEAR_PROFILES   /* AUTO_LEARN build only (menu items replace PHONE MODEL / ENABLE / CLEAR) */
} ui_action_t;
#define CONFIRM_ARM_DELAY_MS 450u  /* a tap on CANCEL/APPLY earlier than this after the dialog opened is ignored */
#define SETTINGS_ITEMS 3           /* rotate | calibrate | touch diagnostic */

typedef struct {
    screen_t screen;
    int menu_cur, model_cur, soc_cur;      /* model_cur: 0 = Back, 1..n = profile n-1 */
    confirm_kind_t confirm;
    int confirm_cur;                       /* 0 = CANCEL, 1 = APPLY */
    int confirm_arg, confirm_from;
    uint32_t confirm_at;
    int n_models;
    bool chg_ui;                           /* CHARGE TEST reachable by touch (CHG_UI_ENABLED), set by main */
    int chg_page, chg_menu_cur;
    screen_t soc_ret;                      /* where SOC Back/apply returns: SCR_MENU or SCR_CHG */
    int soc_pick;                          /* OWNER r7: value being edited on SCR_SOC (1..100) */
} ui_nav_t;

typedef struct {
    out_state_t out;
    int active_prof;
    int soc_current;     /* requested SOC (what the user last confirmed) */
    uint32_t now;
    bool chg_start_ok;   /* every charge-test readiness check PASS */
} nav_ctx_t;

void nav_init(ui_nav_t *n, int n_models);
ui_action_t nav_handle(ui_nav_t *n, btn_event_t ev, const nav_ctx_t *c, int *arg, const char **toast);
void nav_open_confirm(ui_nav_t *n, confirm_kind_t k, int arg, int from, uint32_t now);
/* periodic: confirm timeout + keep screen consistent with safety state */
ui_action_t nav_tick(ui_nav_t *n, const nav_ctx_t *c);
void nav_sync(ui_nav_t *n, out_state_t out);
const char *action_name(ui_action_t a);
/* P1_r1 touch entry points. idx meaning depends on the screen (see ui_nav.c). Stale/invalid taps
 * return ACT_NONE without side effects. Same state machine as the button events. */
ui_action_t nav_select(ui_nav_t *n, int idx, const nav_ctx_t *c, int *arg, const char **toast);
ui_action_t nav_back(ui_nav_t *n, const nav_ctx_t *c, int *arg, const char **toast);
#endif
