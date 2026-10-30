/* AUTO_LEARN_P1_RC1 UI contract (host, built with -DCHIPLER_AUTO_LEARN=1):
 *  - breadth-first search over every button event and every touch index (nav_handle / nav_select / nav_back) from the
 *    startup screen: SCR_MODELS and CONF_MODEL are unreachable, so no phone model can be selected by any path;
 *  - STOP (OK long) gives ACT_OUTPUT_OFF from every reachable screen except a confirm (where it only closes it, as before);
 *  - CLEAR SAVED needs a confirm, and CANCEL is the default;
 *  - the touch layout of every reachable screen has no tile named like the old model entry. */
#include <stdio.h>
#include <string.h>
#include "ui/ui_nav.h"
#include "ui/ui_ls.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

#define MAXQ 4096
static ui_nav_t q[MAXQ]; static int nq;
static bool same(const ui_nav_t *a, const ui_nav_t *b) {
    return a->screen == b->screen && a->menu_cur == b->menu_cur && a->confirm == b->confirm && a->confirm_cur == b->confirm_cur &&
           a->soc_cur == b->soc_cur && a->soc_pick == b->soc_pick && a->chg_page == b->chg_page && a->chg_menu_cur == b->chg_menu_cur &&
           a->model_cur == b->model_cur;
}
static void push(const ui_nav_t *in) {
    ui_nav_t nn = *in; nn.soc_pick = 50;   /* the SOC value being edited never changes which screens are reachable */
    const ui_nav_t *n = &nn;
    for (int i = 0; i < nq; i++) if (same(&q[i], n)) return;
    if (nq < MAXQ) q[nq++] = *n;
}

int main(void) {
    ui_nav_t n0; nav_init(&n0, 33); n0.chg_ui = false;
    nav_ctx_t c = {OUT_OFF, 0, 50, 100000u, false};
    push(&n0);
    bool seen_models = false, seen_conf_model = false, seen_clear_confirm = false, seen_auto_on = false, seen_relearn = false, seen_clear_act = false;
    for (int i = 0; i < nq; i++) {
        for (int ev = BTN_EV_UP; ev <= BTN_EV_OK_LONG; ev++) {
            ui_nav_t n = q[i]; int arg = -1; const char *t = 0;
            ui_action_t a = nav_handle(&n, (btn_event_t)ev, &c, &arg, &t);
            if (ev == BTN_EV_OK_LONG && q[i].screen != SCR_CONFIRM) CHECK(a == ACT_OUTPUT_OFF);
            if (a == ACT_AUTO_ON) seen_auto_on = true;
            if (a == ACT_RELEARN) seen_relearn = true;
            if (a == ACT_CLEAR_PROFILES) { seen_clear_act = true; CHECK(q[i].screen == SCR_CONFIRM && q[i].confirm == CONF_CLEAR_PROFILES); }
            CHECK(a != ACT_LOAD_PROFILE);
            push(&n);
        }
        for (int idx = -1; idx < 40; idx++) {
            ui_nav_t n = q[i]; int arg = -1; const char *t = 0;
            ui_action_t a = nav_select(&n, idx, &c, &arg, &t);
            if (a == ACT_CLEAR_PROFILES) { seen_clear_act = true; CHECK(q[i].screen == SCR_CONFIRM); }
            CHECK(a != ACT_LOAD_PROFILE);
            push(&n);
        }
        { ui_nav_t n = q[i]; int arg = -1; const char *t = 0; (void)nav_back(&n, &c, &arg, &t); push(&n); }
    }
    for (int i = 0; i < nq; i++) {
        if (q[i].screen == SCR_MODELS) seen_models = true;
        if (q[i].screen == SCR_CONFIRM && q[i].confirm == CONF_MODEL) seen_conf_model = true;
        if (q[i].screen == SCR_CONFIRM && q[i].confirm == CONF_CLEAR_PROFILES) { seen_clear_confirm = true; }
        /* every reachable layout: no model tile */
        ui_model_t m; memset(&m, 0, sizeof m);
        m.screen = q[i].screen; m.menu_cur = q[i].menu_cur; m.confirm = q[i].confirm; m.model_cur = q[i].model_cur; m.auto_build = true;
        ls_layout_t l; ui_ls_layout(&m, &l);
        for (int k = 0; k < l.n; k++) if (l.r[k].label) {
            CHECK(!strstr(l.r[k].label, "MODEL"));
            CHECK(!strstr(l.r[k].label, "LOAD PROFILE"));
        }
    }
    /* a freshly opened CLEAR confirm starts on CANCEL */
    { ui_nav_t n = n0; n.screen = SCR_MENU; n.menu_cur = MI_CLEAR; int arg; const char *t;
      nav_handle(&n, BTN_EV_OK, &c, &arg, &t);
      CHECK(n.screen == SCR_CONFIRM && n.confirm == CONF_CLEAR_PROFILES && n.confirm_cur == 0);
      CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &t) == ACT_NONE && n.screen == SCR_DASH); }
    printf("reachable nav states: %d\n", nq);
    CHECK(nq < MAXQ);
    CHECK(!seen_models);
    CHECK(!seen_conf_model);
    CHECK(seen_clear_confirm && seen_clear_act && seen_auto_on && seen_relearn);
    printf("auto_nav_tests: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
