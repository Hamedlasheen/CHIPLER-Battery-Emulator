#include "ui_nav.h"
#include "../batt/soc_ctrl.h"

void nav_init(ui_nav_t *n, int n_models) {
    n->screen = SCR_STARTUP; n->menu_cur = 0; n->model_cur = 0; n->soc_cur = 1;
    n->confirm = CONF_NONE; n->confirm_cur = 0; n->confirm_arg = -1; n->confirm_from = -1;
    n->confirm_at = 0; n->n_models = n_models; n->chg_ui = false;
    n->chg_page = 0; n->chg_menu_cur = 0; n->soc_ret = SCR_MENU; n->soc_pick = 50;
}

const char *action_name(ui_action_t a) {
    static const char *s[] = {"NONE", "OUTPUT_OFF", "ARM_REQUEST", "ARM_CONFIRM", "ARM_CANCEL",
                              "LOAD_PROFILE", "SOC_APPLY", "ACK_FAULT", "CLEAR_COUNTERS",
                              "CHG_START", "CHG_STOP", "ROTATE", "AUTO_ON", "RELEARN", "CLEAR_PROFILES"};
    return (unsigned)a < sizeof s / sizeof s[0] ? s[a] : "?";
}

void nav_open_confirm(ui_nav_t *n, confirm_kind_t k, int arg, int from, uint32_t now) {
    n->screen = SCR_CONFIRM; n->confirm = k; n->confirm_cur = 0;   /* CANCEL highlighted */
    n->confirm_arg = arg; n->confirm_from = from; n->confirm_at = now;
}

static void close_confirm(ui_nav_t *n, screen_t back) { n->confirm = CONF_NONE; n->screen = back; }

static int __attribute__((unused)) soc_index(int pct) {   /* default build only */
    for (int i = 0; i < SOC_N_PRESETS; i++) if (k_soc_presets[i] == pct) return i;
    return 1;
}

static int __attribute__((unused)) clamp_soc(int v) {   /* owner build only */ return v < 1 ? 1 : (v > 100 ? 100 : v); }

/* Entering SCR_SOC: presets index for the default build, picker value for the owner build. */
static void open_soc(ui_nav_t *n, const nav_ctx_t *c, screen_t ret) {
    n->screen = SCR_SOC; n->soc_ret = ret;
#if CHIPLER_OWNER_PHONE_TEST
    n->soc_cur = OWNER_SOC_APPLY; n->soc_pick = clamp_soc(c->soc_current);
#else
    n->soc_cur = soc_index(c->soc_current);
#endif
}

static void leave_soc(ui_nav_t *n) {
    if (n->soc_ret == SCR_CHG) { n->screen = SCR_CHG_MENU; n->chg_menu_cur = CMI_SOC; }
    else n->screen = SCR_MENU;
}

static screen_t confirm_home(confirm_kind_t k) { return k == CONF_CHG_START ? SCR_CHG : SCR_DASH; }

static ui_action_t cancel_action(confirm_kind_t k) { return k == CONF_ARM ? ACT_ARM_CANCEL : ACT_NONE; }

ui_action_t nav_handle(ui_nav_t *n, btn_event_t ev, const nav_ctx_t *c, int *arg, const char **toast) {
    *toast = 0;
    if (ev == BTN_EV_NONE) return ACT_NONE;
    if (ev == BTN_EV_OK_LONG) {                          /* global, never confirms */
        if (n->screen == SCR_CONFIRM) {
            close_confirm(n, confirm_home(n->confirm));
        }
        if (n->screen == SCR_STARTUP) n->screen = SCR_DASH;
        return ACT_OUTPUT_OFF;
    }
    bool up = ev == BTN_EV_UP, down = ev == BTN_EV_DOWN, ok = ev == BTN_EV_OK;
    switch (n->screen) {
    case SCR_STARTUP:
        n->screen = SCR_DASH; return ACT_NONE;
    case SCR_DASH:
        if (up || down) n->screen = SCR_PROTO;
        else if (ok) { n->screen = SCR_MENU; n->menu_cur = 0; }
        return ACT_NONE;
    case SCR_PROTO:
        if (up || down) n->screen = SCR_DASH;
        else if (ok) { n->screen = SCR_MENU; n->menu_cur = 0; }
        return ACT_NONE;
    case SCR_MENU:
        if (up) { n->menu_cur = (n->menu_cur + MI_COUNT - 1) % MI_COUNT; return ACT_NONE; }
        if (down) { n->menu_cur = (n->menu_cur + 1) % MI_COUNT; return ACT_NONE; }
        switch ((menu_item_t)n->menu_cur) {
        case MI_SOC:   open_soc(n, c, SCR_MENU); return ACT_NONE;
        case MI_CHARGE:
            if (!n->chg_ui) { *toast = "NOT AVAILABLE"; return ACT_NONE; }   /* DS-UI-11 D-02: no path around the hidden tile */
            n->screen = SCR_CHG; n->chg_page = 0; return ACT_NONE;
#if CHIPLER_AUTO_LEARN
        /* AUTO_LEARN: there is no phone-model page. These items drive AUTO; SCR_MODELS is unreachable (ui_ls_tests BFS). */
        case MI_MODEL:  n->screen = SCR_DASH; return ACT_AUTO_ON;
        case MI_ENABLE: n->screen = SCR_DASH; return ACT_RELEARN;
        case MI_CLEAR:  nav_open_confirm(n, CONF_CLEAR_PROFILES, 0, 0, c->now); return ACT_NONE;
#else
        case MI_MODEL:
            if (c->out != OUT_OFF) { *toast = "TURN OUTPUT OFF FIRST"; return ACT_NONE; }
            n->screen = SCR_MODELS; n->model_cur = c->active_prof + 1; return ACT_NONE;
#endif
#if !CHIPLER_AUTO_LEARN
        case MI_ENABLE:
            if (c->out != OUT_OFF) { *toast = "OUTPUT NOT OFF"; return ACT_NONE; }
            n->screen = SCR_DASH; return ACT_ARM_REQUEST;  /* controller opens CONFIRM only if accepted */
#endif
        case MI_OFF:   n->screen = SCR_DASH; return ACT_OUTPUT_OFF;
        case MI_PROTO: n->screen = SCR_PROTO; return ACT_NONE;
#if !CHIPLER_AUTO_LEARN
        case MI_CLEAR: n->screen = SCR_PROTO; return ACT_CLEAR_COUNTERS;
#endif
        case MI_BATT:  n->screen = SCR_BATT; return ACT_NONE;
        case MI_SETTINGS: n->screen = SCR_SETTINGS; return ACT_NONE;
        default:       n->screen = SCR_DASH; return ACT_NONE;
        }
    case SCR_SOC:
#if CHIPLER_OWNER_PHONE_TEST
        /* OWNER r7 picker: UP/DOWN step the value by 1, OK applies it (through CONFIRM), BACK leaves. */
        if (up) { n->soc_pick = clamp_soc(n->soc_pick + 1); return ACT_NONE; }
        if (down) { n->soc_pick = clamp_soc(n->soc_pick - 1); return ACT_NONE; }
        if (n->soc_cur == OWNER_SOC_BACK) { leave_soc(n); return ACT_NONE; }
        if (n->soc_pick == c->soc_current) { *toast = "ALREADY SELECTED"; return ACT_NONE; }
        nav_open_confirm(n, CONF_SOC, n->soc_pick, c->soc_current, c->now);
        return ACT_NONE;
#else
        if (up) { n->soc_cur = (n->soc_cur + SOC_ITEMS - 1) % SOC_ITEMS; return ACT_NONE; }
        if (down) { n->soc_cur = (n->soc_cur + 1) % SOC_ITEMS; return ACT_NONE; }
        if (n->soc_cur >= SOC_N_PRESETS) { leave_soc(n); return ACT_NONE; }                 /* Back */
        if (k_soc_presets[n->soc_cur] == c->soc_current) { *toast = "ALREADY SELECTED"; return ACT_NONE; }
        nav_open_confirm(n, CONF_SOC, k_soc_presets[n->soc_cur], c->soc_current, c->now);
        return ACT_NONE;
#endif
    case SCR_MODELS: {
        int items = n->n_models + 1;
        if (up) { n->model_cur = (n->model_cur + items - 1) % items; return ACT_NONE; }
        if (down) { n->model_cur = (n->model_cur + 1) % items; return ACT_NONE; }
        if (n->model_cur == 0) { n->screen = SCR_MENU; return ACT_NONE; }                  /* Back */
        if (c->out != OUT_OFF) { *toast = "TURN OUTPUT OFF FIRST"; n->screen = SCR_DASH; return ACT_NONE; }
        nav_open_confirm(n, CONF_MODEL, n->model_cur - 1, c->active_prof, c->now);
        return ACT_NONE;
    }
    case SCR_CONFIRM: {
        if (up || down) {
            n->confirm_at = c->now;
            /* a blocked charge-test start can never be highlighted */
            if (n->confirm == CONF_CHG_START && !c->chg_start_ok) { n->confirm_cur = 0; *toast = "START BLOCKED"; return ACT_NONE; }
            n->confirm_cur ^= 1; return ACT_NONE;
        }
        confirm_kind_t k = n->confirm;
        int a = n->confirm_arg;
        if (n->confirm_cur == 0) {                      /* CANCEL */
            close_confirm(n, k == CONF_SOC ? SCR_SOC : (k == CONF_MODEL ? SCR_MODELS :
                             (k == CONF_CHG_START ? SCR_CHG_MENU : SCR_DASH)));
            return cancel_action(k);
        }
        if (k == CONF_CHG_START) {
            close_confirm(n, SCR_CHG); n->chg_page = 0;
            if (!c->chg_start_ok) { *toast = "START BLOCKED"; return ACT_NONE; }
            return ACT_CHG_START;
        }
        if (k == CONF_SOC) {
            close_confirm(n, n->soc_ret == SCR_CHG ? SCR_CHG : SCR_DASH);
            *arg = a; return ACT_SOC_APPLY;
        }
        close_confirm(n, SCR_DASH);
        if (k == CONF_MODEL) {
            if (c->out != OUT_OFF) { *toast = "TURN OUTPUT OFF FIRST"; return ACT_NONE; }
            *arg = a; return ACT_LOAD_PROFILE;
        }
        if (k == CONF_ARM) return ACT_ARM_CONFIRM;
#if CHIPLER_AUTO_LEARN
        if (k == CONF_CLEAR_PROFILES) return ACT_CLEAR_PROFILES;
#endif
        return ACT_NONE;
    }
    case SCR_FAULT:
        if (ok) return ACT_ACK_FAULT;
        return ACT_NONE;
    case SCR_SETTINGS: case SCR_CAL: case SCR_TOUCHDIAG: case SCR_BATT:
        return ACT_NONE;   /* touch-only screens; BACK/select handled in nav_back()/nav_select() */
    case SCR_CHG:
        if (up) { n->chg_page = (n->chg_page + CHG_PAGES - 1) % CHG_PAGES; return ACT_NONE; }
        if (down) { n->chg_page = (n->chg_page + 1) % CHG_PAGES; return ACT_NONE; }
        n->screen = SCR_CHG_MENU; n->chg_menu_cur = CMI_START; return ACT_NONE;
    case SCR_CHG_MENU:
        if (up) { n->chg_menu_cur = (n->chg_menu_cur + CMI_COUNT - 1) % CMI_COUNT; return ACT_NONE; }
        if (down) { n->chg_menu_cur = (n->chg_menu_cur + 1) % CMI_COUNT; return ACT_NONE; }
        switch ((chg_menu_item_t)n->chg_menu_cur) {
        case CMI_START:  nav_open_confirm(n, CONF_CHG_START, 0, 0, c->now); return ACT_NONE;
        case CMI_STOP:   n->screen = SCR_CHG; n->chg_page = 0; return ACT_CHG_STOP;
        case CMI_SOC:    open_soc(n, c, SCR_CHG); return ACT_NONE;
        case CMI_RESULT: n->screen = SCR_CHG; n->chg_page = CHG_PAGES - 1; return ACT_NONE;
        case CMI_OFF:    n->screen = SCR_CHG; n->chg_page = 0; return ACT_OUTPUT_OFF;
        case CMI_EXIT:   n->screen = SCR_MENU; n->menu_cur = MI_CHARGE; return ACT_NONE;
        default:         n->screen = SCR_CHG; return ACT_NONE;
        }
    }
    return ACT_NONE;
}

ui_action_t nav_tick(ui_nav_t *n, const nav_ctx_t *c) {
    if (n->screen == SCR_CONFIRM && (uint32_t)(c->now - n->confirm_at) > CONFIRM_TIMEOUT_MS) {
        confirm_kind_t k = n->confirm;
        close_confirm(n, confirm_home(k));
        return cancel_action(k);
    }
    return ACT_NONE;
}

void nav_sync(ui_nav_t *n, out_state_t out) {
    if (out == OUT_FAULT) { if (n->screen == SCR_CONFIRM) n->confirm = CONF_NONE; n->screen = SCR_FAULT; return; }
    if (n->screen == SCR_FAULT) n->screen = SCR_DASH;
    if (n->screen == SCR_CONFIRM && n->confirm == CONF_ARM && out != OUT_ARMED) close_confirm(n, SCR_DASH);
    if (n->screen == SCR_CONFIRM && n->confirm == CONF_MODEL && out != OUT_OFF) close_confirm(n, SCR_DASH);
    if (n->screen == SCR_MODELS && out != OUT_OFF) n->screen = SCR_DASH;
}

/* ---------------- P1_r1: touch entry points ---------------- */
ui_action_t nav_select(ui_nav_t *n, int idx, const nav_ctx_t *c, int *arg, const char **toast) {
    *toast = 0;
    switch (n->screen) {
    case SCR_DASH:
        if (idx == 0) { n->screen = SCR_MENU; n->menu_cur = 0; }
        else if (idx == 1) n->screen = SCR_PROTO;
        return ACT_NONE;
    case SCR_PROTO:
        return idx == 0 ? ACT_CLEAR_COUNTERS : ACT_NONE;
    case SCR_MENU:
        if (idx < 0 || idx >= MI_COUNT) return ACT_NONE;
        if (idx == MI_CHARGE && !n->chg_ui) { *toast = "NOT AVAILABLE"; return ACT_NONE; }   /* no sink hardware in P1 */
        n->menu_cur = idx;
        return nav_handle(n, BTN_EV_OK, c, arg, toast);
    case SCR_SOC:
#if CHIPLER_OWNER_PHONE_TEST
        {   static const int step[4] = {-10, -1, 1, 10};
            static const int preset[3] = {25, 50, 100};
            if (idx >= 0 && idx < 4) { n->soc_pick = clamp_soc(n->soc_pick + step[idx]); return ACT_NONE; }
            if (idx >= OWNER_SOC_PRESET0 && idx < OWNER_SOC_TOUCH_ITEMS) { n->soc_pick = preset[idx - OWNER_SOC_PRESET0]; return ACT_NONE; }
            if (idx != OWNER_SOC_APPLY) return ACT_NONE;
            n->soc_cur = OWNER_SOC_APPLY;
            return nav_handle(n, BTN_EV_OK, c, arg, toast);
        }
#else
        if (idx < 0 || idx >= SOC_ITEMS) return ACT_NONE;
        n->soc_cur = idx;
        return nav_handle(n, BTN_EV_OK, c, arg, toast);
#endif
    case SCR_MODELS:
        if (idx < 0 || idx > n->n_models) return ACT_NONE;
        n->model_cur = idx;
        return nav_handle(n, BTN_EV_OK, c, arg, toast);
    case SCR_CONFIRM:
        if ((idx != 0 && idx != 1) || (uint32_t)(c->now - n->confirm_at) < CONFIRM_ARM_DELAY_MS) return ACT_NONE;
        n->confirm_cur = idx;                        /* idx 1 = APPLY only when the user tapped APPLY */
        return nav_handle(n, BTN_EV_OK, c, arg, toast);
    case SCR_FAULT:
        return idx == 0 ? nav_handle(n, BTN_EV_OK, c, arg, toast) : ACT_NONE;
    case SCR_CHG:                                       /* only the CHARGE MENU button selects */
        return idx == 0 ? nav_handle(n, BTN_EV_OK, c, arg, toast) : ACT_NONE;
    case SCR_CHG_MENU:
        if (idx < 0 || idx >= CMI_COUNT) return ACT_NONE;
        n->chg_menu_cur = idx;
        return nav_handle(n, BTN_EV_OK, c, arg, toast);
    case SCR_SETTINGS:
        if (idx == 0) return ACT_ROTATE;
        if (idx == 1) { n->screen = SCR_CAL; return ACT_NONE; }
        if (idx == 2) { n->screen = SCR_TOUCHDIAG; return ACT_NONE; }
        return ACT_NONE;
    default:
        return ACT_NONE;
    }
}

ui_action_t nav_back(ui_nav_t *n, const nav_ctx_t *c, int *arg, const char **toast) {
    *toast = 0;
    switch (n->screen) {
    case SCR_STARTUP: n->screen = SCR_DASH; return ACT_NONE;
    case SCR_DASH: return ACT_NONE;
    case SCR_PROTO: n->screen = SCR_DASH; return ACT_NONE;
    case SCR_MENU: n->screen = SCR_DASH; return ACT_NONE;
#if CHIPLER_OWNER_PHONE_TEST
    case SCR_SOC: n->soc_cur = OWNER_SOC_BACK; return nav_handle(n, BTN_EV_OK, c, arg, toast);
#else
    case SCR_SOC: n->soc_cur = SOC_ITEMS - 1; return nav_handle(n, BTN_EV_OK, c, arg, toast);
#endif
    case SCR_MODELS: n->model_cur = 0; return nav_handle(n, BTN_EV_OK, c, arg, toast);
    case SCR_CONFIRM:                                     /* BACK always cancels */
        n->confirm_cur = 0; return nav_handle(n, BTN_EV_OK, c, arg, toast);
    case SCR_FAULT: return ACT_NONE;                      /* acknowledgement is explicit */
    case SCR_SETTINGS: case SCR_BATT: n->screen = SCR_MENU; return ACT_NONE;
    case SCR_CAL: case SCR_TOUCHDIAG: n->screen = SCR_SETTINGS; return ACT_NONE;
    case SCR_CHG: n->screen = SCR_MENU; return ACT_NONE;
    case SCR_CHG_MENU: n->screen = SCR_CHG; return ACT_NONE;
    }
    return ACT_NONE;
}
