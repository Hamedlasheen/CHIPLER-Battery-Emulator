/* P1_r1 landscape touch UI: layout invariants for EVERY screen/state, hit-testing, and the touch->nav path.
 * Built with GFX_W=320 GFX_H=240 (text-width measurement uses the real firmware fonts). */
#include "owner_policy.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/gfx/gfx.h"
#include "../../src/gfx/fonts_data.h"
#include "../../src/ui/ui_ls.h"
#include "../../src/ui/ui_nav.h"
#include "../../src/ui/ui_cmd_queue.h"
#include "../../src/input/touch_core.h"
#include "../../src/batt/profile.h"
#include "../../src/batt/soc_ctrl.h"

static unsigned fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static ui_model_t base_model(void) {
    ui_model_t m; memset(&m, 0, sizeof m);
    m.fw = "P1_r1"; m.disp = "P1"; m.lim_ov = 4.45f; m.lim_oc = 3.5f; m.now_ms = 1000;
    m.prof_idx = profile_find("P12PM-r0"); m.last_reg = -1; m.addr = 0x0B; m.addr_verified = 1;
    m.ina_present = true; m.tq = TQ_MEASURED; m.v = 4.012f; m.a = 1.284f; m.w = 5.15f;
    m.soc_requested = 50; m.soc_applied = -1; m.soc_result = SOC_RES_NOT_SUPPORTED;
    return m;
}

static bool overlap(const ls_region_t *a, const ls_region_t *b) {
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}
static int gap(const ls_region_t *a, const ls_region_t *b) {
    int dx = a->x1 <= b->x0 ? b->x0 - a->x1 : (b->x1 <= a->x0 ? a->x0 - b->x1 : 0);
    int dy = a->y1 <= b->y0 ? b->y0 - a->y1 : (b->y1 <= a->y0 ? a->y0 - b->y1 : 0);
    return dx > dy ? dx : dy;
}

static unsigned total_regions;
static void check_layout(const ui_model_t *m, const char *what) {
    ls_layout_t l; ui_ls_layout(m, &l);
    total_regions += (unsigned)l.n;
    for (int i = 0; i < l.n; i++) {
        const ls_region_t *r = &l.r[i];
        if (!(r->x0 >= 0 && r->y0 >= 0 && r->x1 <= LS_W && r->y1 <= LS_H)) { printf("  %s region %d out of bounds\n", what, i); fails++; }
        if (!(r->x1 - r->x0 >= LS_MIN_TARGET_W && r->y1 - r->y0 >= LS_MIN_TARGET_H)) { printf("  %s region %d '%s' too small\n", what, i, r->label); fails++; }
        if (r->style == LS_ST_LISTROW && gfx_text_width(&font_sb9, r->label, 0) > (r->x1 - r->x0) * 0.5f - 12) { printf("  %s list label '%s' too long for the status column\n", what, r->label); fails++; }
        if (!ui_ls_label_fits(r)) { printf("  %s region %d label '%s' does not fit\n", what, i, r->label); fails++; }
        if (r->label) {
            /* never imply the external supply is cut by the screen */
            if (strstr(r->label, "POWER OFF") || strstr(r->label, "DISCONNECT") || strstr(r->label, "CUT")) { printf("  %s misleading label %s\n", what, r->label); fails++; }
        }
        ls_region_t h;
        int cx = (r->x0 + r->x1) / 2, cy = (r->y0 + r->y1) / 2;
        if (!ui_ls_hit(&l, cx, cy, &h) || h.cmd != r->cmd || h.idx != r->idx) { printf("  %s centre of %d does not hit itself\n", what, i); fails++; }
        if (ui_ls_hit(&l, r->x0 - 1, cy, &h) && h.cmd == r->cmd && h.idx == r->idx && r->x0 > 0) { printf("  %s left edge leaks %d\n", what, i); fails++; }
        for (int j = i + 1; j < l.n; j++) {
            if (overlap(r, &l.r[j])) { printf("  %s regions %d/%d overlap\n", what, i, j); fails++; }
            else if (gap(r, &l.r[j]) < 4) { printf("  %s regions %d/%d closer than 4 px\n", what, i, j); fails++; }
        }
    }
    checks++;
}

/* review 111: PHONE MODEL rows, name vs status, every registry profile in this build */
static void model_rows(void) {
    unsigned bad = 0;
    for (int i = 0; i < profile_count(); i++) {
        const batt_profile_t *p = profile_get(i);
        if (!ui_ls_model_row_fits(p)) { printf("  model row '%s' overlaps its status '%s'\n", p->name, ui_ls_model_status(p)); bad++; }
    }
    if (bad) fails++;
    printf("model list rows: %d profiles, %u overlapping name/status\n", profile_count(), bad);
}
int main(void) {
    model_rows();
    ui_model_t m = base_model();
    screen_t all[] = {SCR_STARTUP, SCR_DASH, SCR_PROTO, SCR_MENU, SCR_MODELS, SCR_SOC, SCR_CONFIRM, SCR_FAULT,
                      SCR_SETTINGS, SCR_CAL, SCR_TOUCHDIAG, SCR_BATT};
    for (unsigned s = 0; s < sizeof all / sizeof all[0]; s++)
        for (int cap = 0; cap < 2; cap++) {
            m = base_model(); m.screen = all[s]; m.soc_capable = cap;
            m.touch_cal_valid = (s & 1);
            check_layout(&m, "screen");
        }
    /* CHARGE TEST screens (reachable only with chg_ui): every page / menu / confirm state keeps the layout rules */
    for (int pg = 0; pg < CHG_PAGES; pg++) { m = base_model(); m.chg_ui = true; m.screen = SCR_CHG; m.chg_page = pg; check_layout(&m, "chg page"); }
    for (int k = 0; k < CMI_COUNT; k++) { m = base_model(); m.chg_ui = true; m.screen = SCR_CHG_MENU; m.chg_menu_cur = k; check_layout(&m, "chg menu"); }
    for (int ok = 0; ok < 2; ok++) { m = base_model(); m.chg_ui = true; m.screen = SCR_CONFIRM; m.confirm = CONF_CHG_START; m.chg_start_ok = ok; m.confirm_cur = ok; check_layout(&m, "chg confirm"); }
    m = base_model(); m.chg_ui = true; m.screen = SCR_MENU; check_layout(&m, "menu with charge tile");
    { ls_layout_t l; ui_ls_layout(&m, &l); int c = 0; for (int i = 0; i < l.n; i++) c += l.r[i].idx == MI_CHARGE && l.r[i].cmd == LS_CMD_SELECT; CHECK(c == 1); }
    m = base_model(); m.chg_ui = true; m.screen = SCR_CONFIRM; m.confirm = CONF_CHG_START; m.chg_start_ok = false; m.confirm_cur = 0;
    { ls_layout_t l; ui_ls_layout(&m, &l); CHECK(l.r[1].label[0] == 'B'); }       /* BLOCKED label while checks fail */
    /* model list windows over the whole registry */
    for (int cur = 0; cur <= profile_count(); cur++) {
        m = base_model(); m.screen = SCR_MODELS; m.model_cur = cur; check_layout(&m, "models");
        ls_layout_t l; ui_ls_layout(&m, &l);
        int rows = 0; for (int i = 0; i < l.n; i++) if (l.r[i].cmd == LS_CMD_SELECT) rows++;
        CHECK(rows >= 1 && rows <= 4);
    }
    /* confirm: CANCEL is the default highlight */
    m = base_model(); m.screen = SCR_CONFIRM; m.confirm = CONF_SOC; m.confirm_cur = 0; m.confirm_from = 50; m.confirm_arg = 100;
    { ls_layout_t l; ui_ls_layout(&m, &l);
      CHECK(l.r[0].idx == 0 && l.r[0].highlight && l.r[1].idx == 1 && !l.r[1].highlight);
      int cancel_w = l.r[0].x1 - l.r[0].x0, apply_w = l.r[1].x1 - l.r[1].x0;
      CHECK(cancel_w == apply_w && l.r[0].x1 < l.r[1].x0); }
    /* SOC locked: no preset buttons, only footer */
    m = base_model(); m.screen = SCR_SOC; m.soc_capable = false;
    { ls_layout_t l; ui_ls_layout(&m, &l);
      for (int i = 0; i < l.n; i++) CHECK(l.r[i].cmd != LS_CMD_SELECT); }
    m.soc_capable = true;
    { ls_layout_t l; ui_ls_layout(&m, &l); int sel = 0; for (int i = 0; i < l.n; i++) sel += l.r[i].cmd == LS_CMD_SELECT; CHECK(sel == (OWNER ? OWNER_SOC_TOUCH_ITEMS : 3)); }   /* owner r7 picker: 4 steps + 3 presets + APPLY */
    /* MENU never offers CHARGE TEST (no hardware); every tile maps to a distinct nav item */
    m = base_model(); m.screen = SCR_MENU;
    { ls_layout_t l; ui_ls_layout(&m, &l); int seen[MI_COUNT] = {0};
      for (int i = 0; i < l.n; i++) if (l.r[i].cmd == LS_CMD_SELECT) { CHECK(l.r[i].idx != MI_CHARGE && l.r[i].idx != MI_BACK); seen[l.r[i].idx]++; CHECK(seen[l.r[i].idx] == 1); } }
    /* STOP is present on every interactive screen except calibration/startup */
    for (unsigned s = 0; s < sizeof all / sizeof all[0]; s++) {
        m = base_model(); m.screen = all[s]; m.soc_capable = 1;
        ls_layout_t l; ui_ls_layout(&m, &l); int stop = 0; for (int i = 0; i < l.n; i++) stop += l.r[i].cmd == LS_CMD_STOP;
        if (all[s] == SCR_STARTUP || all[s] == SCR_CAL) CHECK(stop == 0); else CHECK(stop == 1);
        if (all[s] == SCR_CAL) { /* cancel exists only with a working calibration */
            ui_model_t c2 = m; c2.touch_cal_valid = false; ls_layout_t l2; ui_ls_layout(&c2, &l2); CHECK(l2.n == 0);
            c2.touch_cal_valid = true; ui_ls_layout(&c2, &l2); CHECK(l2.n == 1 && l2.r[0].cmd == LS_CMD_BACK);
            tch_pt_t tg[5]; touch_cal_targets(5, 320, 240, tg);     /* cancel must not cover any calibration target */
            for (int k = 0; k < 5; k++) CHECK(!ui_ls_hit(&l2, (int)tg[k].x, (int)tg[k].y, 0));
        }
    }

    /* ---- touch -> nav (the same state machine as the key) ---- */
    ui_nav_t n; nav_init(&n, profile_count());
    nav_ctx_t c = {OUT_OFF, profile_find("P12PM-r0"), 50, 1000, false};
    int arg = -1; const char *toast = 0;
    n.screen = SCR_DASH;
    CHECK(nav_select(&n, 0, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU);
    CHECK(nav_select(&n, MI_CHARGE, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU && toast);   /* refused */
    CHECK(nav_select(&n, MI_SOC, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_SOC);
    if (OWNER) {   /* owner r7 picker: preset 100 %, then APPLY opens the same confirm */
        CHECK(nav_select(&n, OWNER_SOC_PRESET0 + 2, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_SOC && n.soc_pick == 100);
        CHECK(nav_select(&n, OWNER_SOC_APPLY, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CONFIRM && n.confirm == CONF_SOC && n.confirm_cur == 0);
    } else
        CHECK(nav_select(&n, 2, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CONFIRM && n.confirm == CONF_SOC && n.confirm_cur == 0);
    /* the dialog just opened: an immediate APPLY tap (double-tap carry-over) is ignored */
    c.now = n.confirm_at + 100;
    CHECK(nav_select(&n, 1, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CONFIRM);
    c.now = n.confirm_at + CONFIRM_ARM_DELAY_MS + 1;
    CHECK(nav_select(&n, 1, &c, &arg, &toast) == ACT_SOC_APPLY && arg == 100 && n.screen == SCR_DASH);
    /* BACK on a confirm dialog always cancels */
    nav_open_confirm(&n, CONF_MODEL, 3, 0, 5000); c.now = 5001;
    CHECK(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MODELS);
    /* invalid / stale indices do nothing */
    n.screen = SCR_SOC; CHECK(nav_select(&n, 9, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_SOC);
    n.screen = SCR_MODELS; CHECK(nav_select(&n, profile_count() + 5, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MODELS);
    n.screen = SCR_MODELS; CHECK(nav_select(&n, 1, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CONFIRM && n.confirm == CONF_MODEL);
    /* model change is refused unless output state is OFF */
    c.out = OUT_ON; n.screen = SCR_MENU; CHECK(nav_select(&n, MI_MODEL, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU && toast);
    c.out = OUT_OFF;
    /* CHARGE TEST by touch: refused while hidden, reachable (and still blocked) when the UI flag is on */
    n.screen = SCR_MENU; n.chg_ui = false; CHECK(nav_select(&n, MI_CHARGE, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU && toast);
    n.chg_ui = true; CHECK(nav_select(&n, MI_CHARGE, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CHG);
    CHECK(nav_select(&n, 0, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CHG_MENU);
    CHECK(nav_select(&n, CMI_START, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CONFIRM && n.confirm == CONF_CHG_START && n.confirm_cur == 0);
    c.now = n.confirm_at + CONFIRM_ARM_DELAY_MS + 1; c.chg_start_ok = false;
    CHECK(nav_select(&n, 1, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CHG);          /* START is refused: no action issued */
    n.screen = SCR_CHG_MENU; CHECK(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CHG);
    n.screen = SCR_CHG_MENU; CHECK(nav_select(&n, CMI_OFF, &c, &arg, &toast) == ACT_OUTPUT_OFF);
    n.chg_ui = false; c.now = 1000;
    /* STOP = same as long OK */
    n.screen = SCR_PROTO; CHECK(nav_handle(&n, BTN_EV_OK_LONG, &c, &arg, &toast) == ACT_OUTPUT_OFF);
    /* settings, fault, back chain */
    n.screen = SCR_SETTINGS;
    CHECK(nav_select(&n, 0, &c, &arg, &toast) == ACT_ROTATE);
    CHECK(nav_select(&n, 2, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_TOUCHDIAG);
    CHECK(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_SETTINGS);
    CHECK(nav_select(&n, 1, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CAL);
    CHECK(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_SETTINGS);
    CHECK(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU);
    CHECK(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_DASH);
    n.screen = SCR_FAULT; CHECK(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_FAULT);   /* ack is explicit */
    CHECK(nav_select(&n, 0, &c, &arg, &toast) == ACT_ACK_FAULT);
    n.screen = SCR_PROTO; CHECK(nav_select(&n, 0, &c, &arg, &toast) == ACT_CLEAR_COUNTERS);

    /* ---- command ring ---- */
    uicq_t q; uicq_init(&q);
    for (unsigned i = 0; i < UICQ_N; i++) CHECK(uicq_push(&q, (uic_t){UIC_SELECT, SCR_MENU, (int16_t)i}));
    CHECK(!uicq_push(&q, (uic_t){UIC_STOP, SCR_MENU, 0}) && q.dropped == 1);   /* full: dropped + counted, never blocks */
    uic_t o; for (unsigned i = 0; i < UICQ_N; i++) CHECK(uicq_pop(&q, &o) && o.idx == (int16_t)i);
    CHECK(!uicq_pop(&q, &o));

    /* priority STOP flag: survives a full ring, is consumed once */
    { uicq_t q2; uicq_init(&q2); for (unsigned i = 0; i < UICQ_N + 3; i++) uicq_push(&q2, (uic_t){UIC_SELECT, SCR_MENU, 0});
      CHECK(!uicq_take_stop(&q2)); uicq_request_stop(&q2); uicq_request_stop(&q2); CHECK(uicq_take_stop(&q2)); CHECK(!uicq_take_stop(&q2)); }
    /* MI_CHARGE through the legacy nav_handle path is refused while the charge UI is hidden */
    { ui_nav_t n3; nav_init(&n3, profile_count()); n3.screen = SCR_MENU; n3.menu_cur = MI_CHARGE; nav_ctx_t c3 = {OUT_OFF, 0, 50, 1000, false}; int a3 = -1; const char *t3 = 0;
      CHECK(nav_handle(&n3, BTN_EV_OK, &c3, &a3, &t3) == ACT_NONE && n3.screen == SCR_MENU && t3); n3.chg_ui = true;
      CHECK(nav_handle(&n3, BTN_EV_OK, &c3, &a3, &t3) == ACT_NONE && n3.screen == SCR_CHG); }
    printf("P1 landscape UI: %u checks, %u regions examined, %u failures: %s\n", checks, total_regions, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
