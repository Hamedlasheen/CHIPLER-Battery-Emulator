/* Host unit tests for pure-logic modules (no hardware). Build: see tests/host/CMakeLists.txt */
#include <stdio.h>
#include <string.h>
#include "../../src/input/buttons.h"
#include "../../src/ui/ui_nav.h"
#include "../../src/safety/safety.h"
#include "../../src/batt/profile.h"
#include "../../src/batt/target_engine.h"
#include "../../src/batt/soc_ctrl.h"
#include "../../src/power/telem_fresh.h"
#include "../../src/charge/charge_diag.h"
#include "../../config/board_config.h"
#include "owner_policy.h"
/* owner build: DATA TEST profiles are armable (owner unlock, NOT verified); every other profile stays locked */
static bool expect_armable(const batt_profile_t *p) { return p && (p->status == PROF_VERIFIED || (OWNER && p->status == PROF_PHONE_DATA_TEST)); }
static int expect_armable_count(void) { int n = 0; for (int i = 0; i < profile_count(); i++) n += expect_armable(profile_get(i)); return n; }

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

/* drive buttons for dur ms; returns the number of events of kind `want` and the first event */
typedef struct { btn_event_t first; int count[5]; } evs_t;
static evs_t run(buttons_t *b, uint32_t *t, uint32_t dur, bool up, bool dn, bool ok) {
    evs_t r; memset(&r, 0, sizeof r);
    for (uint32_t i = 0; i < dur; i++) {
        btn_event_t e = buttons_update(b, (*t)++, up, dn, ok);
        if (e != BTN_EV_NONE) { if (r.first == BTN_EV_NONE) r.first = e; r.count[e]++; }
    }
    return r;
}
#define IDLE(ms) run(&b, &t, ms, false, false, false)

static void test_buttons(void) {
    buttons_t b; uint32_t t = 0; buttons_init(&b, 0);
    IDLE(100);
    /* short UP / DOWN / OK fire on release */
    CHECK(run(&b, &t, 150, true, false, false).first == BTN_EV_NONE);
    CHECK(IDLE(50).first == BTN_EV_UP);
    run(&b, &t, 150, false, true, false); CHECK(IDLE(50).first == BTN_EV_DOWN);
    run(&b, &t, 150, false, false, true); CHECK(IDLE(50).first == BTN_EV_OK);
    /* bounce shorter than debounce -> nothing */
    run(&b, &t, 12, false, false, true); CHECK(IDLE(100).first == BTN_EV_NONE);
    /* OK long: exactly one OK_LONG at 1.5 s, nothing on release, even if held 5 s */
    evs_t r = run(&b, &t, 5000, false, false, true);
    CHECK(r.first == BTN_EV_OK_LONG && r.count[BTN_EV_OK_LONG] == 1 && r.count[BTN_EV_OK] == 0);
    CHECK(IDLE(100).first == BTN_EV_NONE);
    /* no hold-through: OK held from a previous screen never produces a 2nd OK without release */
    r = run(&b, &t, 1200, false, false, true);     /* held 1.2 s (< long) */
    CHECK(r.count[BTN_EV_OK] == 0);                /* nothing while held */
    r = IDLE(50);
    CHECK(r.count[BTN_EV_OK] == 1);                /* exactly one OK on release */
    r = IDLE(1000);
    CHECK(r.count[BTN_EV_OK] == 0);                /* never a second one */
    /* UP held: first repeat at 600 ms then every 200 ms; no extra event on release */
    r = run(&b, &t, 1450, true, false, false);
    CHECK(r.count[BTN_EV_UP] == 5);                /* 600, 800, 1000, 1200, 1400 ms */
    CHECK(IDLE(100).count[BTN_EV_UP] == 0);
    /* simultaneous UP+OK: nothing at all, also not on staggered release */
    r = run(&b, &t, 50, true, false, false);
    r = run(&b, &t, 300, true, false, true);
    CHECK(r.first == BTN_EV_NONE);
    r = run(&b, &t, 100, true, false, false);      /* OK released first */
    CHECK(r.first == BTN_EV_NONE);
    CHECK(IDLE(100).first == BTN_EV_NONE);         /* UP released later: still nothing */
    /* chord held 3 s never yields OK_LONG */
    r = run(&b, &t, 3000, false, true, true);
    CHECK(r.first == BTN_EV_NONE);
    CHECK(IDLE(100).first == BTN_EV_NONE);
    /* decoder recovers after a chord */
    run(&b, &t, 100, false, false, true); CHECK(IDLE(50).first == BTN_EV_OK);
}

static void test_nav(void) {
    ui_nav_t n; int arg = -1; const char *toast;
    int np = profile_count(), p12 = profile_find("P12PM-r0");
    nav_ctx_t c = {OUT_OFF, p12, 50, 0, false};
    nav_init(&n, np);
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);               /* leave startup */
    CHECK(n.screen == SCR_DASH);
    nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast); CHECK(n.screen == SCR_PROTO);
    nav_handle(&n, BTN_EV_UP, &c, &arg, &toast);   CHECK(n.screen == SCR_DASH);
    /* OK opens MENU; every menu item reachable; Back returns */
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);   CHECK(n.screen == SCR_MENU && n.menu_cur == MI_SOC);
    nav_handle(&n, BTN_EV_UP, &c, &arg, &toast);   CHECK(n.menu_cur == MI_BACK);   /* wraps */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_DASH);
    /* OK long = OUTPUT OFF from every screen, never a confirm */
    screen_t scr[] = {SCR_DASH, SCR_PROTO, SCR_MENU, SCR_SOC, SCR_MODELS, SCR_FAULT};
    for (unsigned i = 0; i < sizeof scr / sizeof scr[0]; i++) {
        n.screen = scr[i];
        CHECK(nav_handle(&n, BTN_EV_OK_LONG, &c, &arg, &toast) == ACT_OUTPUT_OFF);
    }
    nav_open_confirm(&n, CONF_SOC, 100, 50, 0); n.confirm_cur = 1;          /* even with APPLY highlighted */
    CHECK(nav_handle(&n, BTN_EV_OK_LONG, &c, &arg, &toast) == ACT_OUTPUT_OFF && n.screen == SCR_DASH && n.confirm == CONF_NONE);
    /* menu: Output OFF item */
    n.screen = SCR_MENU; n.menu_cur = MI_OFF;
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_OUTPUT_OFF);
    /* model select: blocked while output ON */
    c.out = OUT_ON; n.screen = SCR_MENU; n.menu_cur = MI_MODEL;
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && toast && n.screen == SCR_MENU);
    c.out = OUT_OFF;
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MODELS && n.model_cur == p12 + 1);
    nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast);               /* next profile */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CONFIRM && n.confirm == CONF_MODEL);
    CHECK(n.confirm_cur == 0);                                   /* CANCEL highlighted by default */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MODELS);   /* cancel */
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);                 /* re-open confirm */
    nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast);               /* -> APPLY */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_LOAD_PROFILE && arg == p12 + 1);
    /* models Back item */
    n.screen = SCR_MODELS; n.model_cur = 0;
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU);
    /* models list wraps through Back */
    n.screen = SCR_MODELS; n.model_cur = 0; nav_handle(&n, BTN_EV_UP, &c, &arg, &toast); CHECK(n.model_cur == np);
    /* enable output -> ARM_REQUEST; controller opens confirm; cancel/apply */
    n.screen = SCR_MENU; n.menu_cur = MI_ENABLE;
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_ARM_REQUEST);
    nav_open_confirm(&n, CONF_ARM, 0, 0, 100);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_ARM_CANCEL);      /* default = CANCEL */
    nav_open_confirm(&n, CONF_ARM, 0, 0, 100); nav_handle(&n, BTN_EV_UP, &c, &arg, &toast);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_ARM_CONFIRM);
    /* confirm timeout = cancel */
    nav_open_confirm(&n, CONF_ARM, 0, 0, 1000);
    c.now = 1000 + CONFIRM_TIMEOUT_MS - 1; CHECK(nav_tick(&n, &c) == ACT_NONE && n.screen == SCR_CONFIRM);
    c.now = 1000 + CONFIRM_TIMEOUT_MS + 1; CHECK(nav_tick(&n, &c) == ACT_ARM_CANCEL && n.screen == SCR_DASH);
    c.now = 0;
    /* fault forces screen; OK acknowledges */
    nav_sync(&n, OUT_FAULT); CHECK(n.screen == SCR_FAULT);
    CHECK(nav_handle(&n, BTN_EV_UP, &c, &arg, &toast) == ACT_NONE);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_ACK_FAULT);
    nav_sync(&n, OUT_OFF); CHECK(n.screen == SCR_DASH);
    /* a confirm screen is dropped when a fault appears */
    nav_open_confirm(&n, CONF_SOC, 100, 50, 0); nav_sync(&n, OUT_FAULT);
    CHECK(n.screen == SCR_FAULT && n.confirm == CONF_NONE);
    nav_sync(&n, OUT_OFF);
}

#if CHIPLER_OWNER_PHONE_TEST
/* owner r7 SOC picker with the hardware keys: same confirm contract as CR-003 (separate confirm, cancel preserves the
 * value, same value refused, SOC never produces an output action), value stepped by UP/DOWN, BACK by touch/back */
static void test_soc_menu(void) {
    ui_nav_t n; int arg = -1; const char *toast = 0;
    nav_ctx_t c = {OUT_OFF, profile_find("P12PM-r0"), 50, 0, false};
    nav_init(&n, profile_count()); n.screen = SCR_MENU; n.menu_cur = MI_SOC;
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);
    CHECK(n.screen == SCR_SOC && n.soc_pick == 50 && n.soc_cur == OWNER_SOC_APPLY);   /* starts on current 50 % */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && toast && n.screen == SCR_SOC); /* same value */
    nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast); CHECK(n.soc_pick == 49);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE);       /* opens confirm only */
    CHECK(n.screen == SCR_CONFIRM && n.confirm == CONF_SOC && n.confirm_arg == 49 && n.confirm_from == 50 && n.confirm_cur == 0);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_SOC && n.soc_pick == 49);   /* CANCEL keeps */
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);
    nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast);                 /* APPLY */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_SOC_APPLY && arg == 49 && n.screen == SCR_DASH);
    n.screen = SCR_SOC; n.soc_pick = 1; nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast); CHECK(n.soc_pick == 1);       /* clamp */
    n.soc_pick = 100; nav_handle(&n, BTN_EV_UP, &c, &arg, &toast); CHECK(n.soc_pick == 100);
    CHECK(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU);
    /* SOC is selectable while output is ON (owner flow) and never produces an output action */
    c.out = OUT_ON; n.screen = SCR_SOC; n.soc_pick = 25; n.soc_cur = OWNER_SOC_APPLY;
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast); nav_handle(&n, BTN_EV_UP, &c, &arg, &toast);
    ui_action_t a = nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);
    CHECK(a == ACT_SOC_APPLY && arg == 25);
}
#else
/* CR-003 SOC menu: only 25/50/100, separate confirm, cancel preserves value */
static void test_soc_menu(void) {
    ui_nav_t n; int arg = -1; const char *toast;
    nav_ctx_t c = {OUT_OFF, profile_find("P12PM-r0"), 50, 0, false};
    nav_init(&n, profile_count()); n.screen = SCR_MENU; n.menu_cur = MI_SOC;
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);
    CHECK(n.screen == SCR_SOC && n.soc_cur == 1);                  /* starts on current 50 % */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && toast && n.screen == SCR_SOC); /* same value */
    nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast); CHECK(n.soc_cur == 2);   /* 100 % */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE);       /* opens confirm only */
    CHECK(n.screen == SCR_CONFIRM && n.confirm == CONF_SOC && n.confirm_arg == 100 && n.confirm_from == 50 && n.confirm_cur == 0);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_SOC);   /* CANCEL */
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);
    nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast);                 /* APPLY */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_SOC_APPLY && arg == 100 && n.screen == SCR_DASH);
    /* all three presets reachable, plus Back; nothing else */
    n.screen = SCR_SOC; n.soc_cur = 0;
    int seen25 = 0, seen50 = 0, seen100 = 0, back = 0;
    for (int i = 0; i < SOC_ITEMS; i++) {
        if (n.soc_cur == 3) back++; else { int v = k_soc_presets[n.soc_cur]; seen25 += v == 25; seen50 += v == 50; seen100 += v == 100; }
        nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast);
    }
    CHECK(seen25 == 1 && seen50 == 1 && seen100 == 1 && back == 1 && n.soc_cur == 0);
    n.soc_cur = 3; CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU);
    /* SOC is selectable while output is ON (owner flow) and never produces an output action */
    c.out = OUT_ON; n.screen = SCR_SOC; n.soc_cur = 0;
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast); nav_handle(&n, BTN_EV_UP, &c, &arg, &toast);
    ui_action_t a = nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);
    CHECK(a == ACT_SOC_APPLY && arg == 25);
}
#endif

/* soc_ctrl: presets, unsupported profile, synthetic backend, failure, profile change */
typedef struct { bool accept; int calls; uint8_t base, len, data[8]; } fake_pub_t;
static bool fake_publish(void *ctx, uint8_t base, const uint8_t *d, uint8_t len) {
    fake_pub_t *f = ctx; f->calls++;
    if (!f->accept) return false;
    f->base = base; f->len = len; memcpy(f->data, d, len);
    return true;
}
static void test_soc_ctrl(void) {
    soc_ctrl_t s; soc_init(&s);
    fake_pub_t f; memset(&f, 0, sizeof f); f.accept = true;
    const batt_profile_t *p12 = profile_get(profile_find("P12PM-r0"));
    const batt_profile_t *bench = profile_get(profile_find("BENCH-r0"));
    CHECK(s.requested == 50 && s.applied == -1);                 /* reset default, nothing applied */
    /* default: only 25/50/100; owner r7: any integer 1..100 */
#if CHIPLER_OWNER_PHONE_TEST
    int bad[] = {0, 101, 200, -25, -1};
#else
    int bad[] = {0, 1, 24, 26, 49, 51, 75, 99, 101, 200, -25};
#endif
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(soc_apply(&s, bench, bad[i], fake_publish, &f) == SOC_RES_REJECTED);
        CHECK(s.requested == 50 && s.applied == -1);
    }
    CHECK(f.calls == 0);
    /* real-phone profile (incl. 12 Pro Max): no capability -> NOT SENT, applied unchanged */
    CHECK(soc_apply(&s, p12, 100, fake_publish, &f) == SOC_RES_NOT_SUPPORTED);
    CHECK(s.requested == 100 && s.applied == -1 && f.calls == 0);
    for (int i = 0; i < profile_count(); i++) {
        const batt_profile_t *p = profile_get(i);
        if (p->status != PROF_BENCH_TEST) CHECK(p->soc_build == 0);   /* no invented phone SOC builder */
    }
    /* bench synthetic backend: image published, consistent */
    soc_init(&s);
    CHECK(soc_apply(&s, bench, 100, fake_publish, &f) == SOC_RES_APPLIED_SYNTHETIC);
    CHECK(s.applied == 100 && f.base == 0x20 && f.len == 4 && f.data[0] == 100);
    CHECK((f.data[1] | f.data[2] << 8) == 4000 && f.data[3] == (uint8_t)(f.data[0] + f.data[1] + f.data[2]));
    /* publish failure never becomes success; retried by service */
    f.accept = false;
    CHECK(soc_apply(&s, bench, 25, fake_publish, &f) == SOC_RES_BUSY_RETRY);
    CHECK(s.requested == 25 && s.applied == 100 && s.retry_pending);
    CHECK(soc_service(&s, bench, fake_publish, &f) == SOC_RES_BUSY_RETRY && s.applied == 100);
    f.accept = true;
    CHECK(soc_service(&s, bench, fake_publish, &f) == SOC_RES_APPLIED_SYNTHETIC && s.applied == 25 && !s.retry_pending);
    CHECK(soc_service(&s, bench, fake_publish, &f) == SOC_RES_NONE);
    /* profile change resets to 50 % */
    CHECK(soc_on_profile_change(&s, p12, fake_publish, &f) == SOC_RES_NOT_SUPPORTED && s.requested == 50 && s.applied == -1);
    /* SOC never touches safety: output stays locked with 100 % on every registry profile */
    safety_limits_t L = {4.45f, 3.30f, 3.50f, 10000, 500};
    safety_inputs_t good = {TQ_MEASURED, 4.0f, 0.2f, 4.05f, true, false};
    for (int i = 0; i < profile_count(); i++) {
        const batt_profile_t *p = profile_get(i);
        safety_t sf; safety_init(&sf, &L);
        soc_apply(&s, p, 100, fake_publish, &f);
        CHECK(sf.state == OUT_OFF);
        CHECK((safety_request_arm(&sf, 0, &good, p) == ARM_REFUSED_PROFILE) == !expect_armable(p));
        CHECK(!safety_output_gate(&sf, p));                       /* not ON: SOC alone never enables output */
    }
}

/* engine snapshot: a transaction never mixes old and new SOC images */
static void test_snapshot_atomic(void) {
    target_engine_t e; const batt_profile_t *bench = profile_get(profile_find("BENCH-r0"));
    te_init(&e, bench);
    uint8_t img[8], base, len;
    bench->soc_build(50, img, &base, &len);
    CHECK(te_publish(&e, base, img, len));
    /* read 0x20.. : start txn, read 2 bytes, publish 100 % mid-transaction, read the rest */
    te_on_receive(&e, 0x20, 1); te_on_finish(&e);
    uint8_t r[4];
    r[0] = te_on_request(&e, 2); r[1] = te_on_request(&e, 3);
    bench->soc_build(100, img, &base, &len);
    CHECK(te_publish(&e, base, img, len));                       /* goes to the other buffer */
    r[2] = te_on_request(&e, 4); r[3] = te_on_request(&e, 5); te_on_finish(&e);
    CHECK(r[0] == 50 && (r[1] | r[2] << 8) == 2000 && r[3] == (uint8_t)(r[0] + r[1] + r[2]));   /* all old */
    /* second publish during the same old transaction must be refused (buffer still in use) */
    te_on_receive(&e, 0x20, 6); te_on_finish(&e);
    (void)te_on_request(&e, 7);                                   /* new txn latched = 100 % buffer */
    bench->soc_build(25, img, &base, &len);
    CHECK(te_publish(&e, base, img, len));                        /* writes the free (old 50 %) buffer */
    CHECK(!te_publish(&e, base, img, len));                       /* free one is now latched by this txn */
    te_on_finish(&e);
    /* next transaction sees the newest complete image */
    te_on_receive(&e, 0x20, 8); te_on_finish(&e);
    r[0] = te_on_request(&e, 9); r[1] = te_on_request(&e, 10); r[2] = te_on_request(&e, 11); r[3] = te_on_request(&e, 12);
    te_on_finish(&e);
    CHECK(r[0] == 25 && (r[1] | r[2] << 8) == 1000 && r[3] == (uint8_t)(r[0] + r[1] + r[2]));
    /* snapshot region is read-only, static bench blocks still work */
    te_on_receive(&e, 0x20, 13); te_on_receive(&e, 0x99, 14); te_on_finish(&e);
    CHECK(e.ignored_writes == 1);
    te_on_receive(&e, 0x00, 15); te_on_finish(&e);
    CHECK(te_on_request(&e, 16) == 0xA5); te_on_finish(&e);
}

/* Codex R-04: freshness */
static void test_freshness(void) {
    telem_fresh_t t; tf_init(&t);
    CHECK(!tf_update(&t, 0, true, false));                        /* no sample yet */
    CHECK(tf_update(&t, 10, true, true));
    CHECK(tf_update(&t, 10 + TF_MAX_AGE_MS, true, false));        /* at limit: still fresh */
    CHECK(!tf_update(&t, 11 + TF_MAX_AGE_MS, true, false));       /* conversions stopped, I2C ok */
    CHECK(t.stale_events == 1);
    CHECK(tf_update(&t, 200, true, true));
    CHECK(!tf_update(&t, 210, false, true));                      /* read failure never fresh */
    CHECK(t.read_failures == 1);
}

/* Codex R-02: EN gate is false for a forced ON state with an unverified profile */
static void test_output_gate(void) {
    safety_limits_t L = {4.45f, 3.30f, 3.50f, 10000, 500};
    for (int i = 0; i < profile_count(); i++) {
        safety_t s; safety_init(&s, &L);
        s.state = OUT_ON;                                         /* corrupted / forced state */
        CHECK(safety_output_gate(&s, profile_get(i)) == expect_armable(profile_get(i)));
    }
    safety_t s; safety_init(&s, &L);
    CHECK(!safety_output_gate(&s, 0));
}

/* TEST-ONLY synthetic qualified profile. It exists only in this host test binary to exercise
 * the ON-path logic (trips, timeouts). It is NOT in the firmware profile registry. */
static const batt_profile_t k_test_verified = {
    "TEST-VERIFIED", "Test", "Host-test verified profile", 0x55, ADDR_VERIFIED, PROF_VERIFIED,
    "host test only", "n/a", 0, 0, 0 };

static void test_safety(void) {
    safety_limits_t L = {4.45f, 3.30f, 3.50f, 10000, 500};
    safety_t s; safety_init(&s, &L);
    safety_inputs_t in = {TQ_MEASURED, 4.0f, 0.2f, 4.05f, true, false};
    const batt_profile_t *tv = &k_test_verified;
    /* T1: boot = OFF */
    CHECK(s.state == OUT_OFF && !safety_output_enabled(&s));
    /* no monitor / simulated telemetry -> refuse even for a qualified profile */
    safety_inputs_t nm = in; nm.q = TQ_SIMULATED;
    CHECK(safety_request_arm(&s, 0, &nm, tv) == ARM_REFUSED_MONITOR);
    nm.q = TQ_ABSENT;
    CHECK(safety_request_arm(&s, 0, &nm, tv) == ARM_REFUSED_MONITOR);
    nm.q = TQ_SIMULATED;
    /* arm + confirm (qualified test profile only) */
    CHECK(safety_request_arm(&s, 0, &in, tv) == ARM_OK && s.state == OUT_ARMED);
    CHECK(!safety_output_enabled(&s));
    CHECK(safety_confirm(&s, 100, tv) && safety_output_enabled(&s));
    CHECK(safety_output_gate(&s, tv));                        /* qualified profile: EN may go high */
    /* overcurrent trips */
    safety_inputs_t oc = in; oc.i_out = 4.0f;
    safety_update(&s, 200, &oc, tv);
    CHECK(s.state == OUT_FAULT && s.fault == FLT_OVERCURRENT && !safety_output_enabled(&s));
    /* T5: ack returns to OFF, never ON */
    CHECK(safety_ack(&s, &in) && s.state == OUT_OFF);
    /* arm timeout */
    CHECK(safety_request_arm(&s, 1000, &in, tv) == ARM_OK);
    CHECK(!safety_confirm(&s, 1000 + 10001, tv) && s.state == OUT_OFF);
    /* T4: input OV is a fault even while OFF; ack refused while still present */
    safety_inputs_t ov = in; ov.v_in = 4.62f;
    safety_update(&s, 2000, &ov, tv);
    CHECK(s.state == OUT_FAULT && s.fault == FLT_OVERVOLTAGE);
    CHECK(!safety_ack(&s, &ov));
    CHECK(safety_ack(&s, &in));
    /* monitor lost / read failure while ON -> fault immediately (no stale values kept) */
    safety_request_arm(&s, 3000, &in, tv); safety_confirm(&s, 3001, tv);
    safety_update(&s, 3002, &nm, tv);
    CHECK(s.state == OUT_FAULT && s.fault == FLT_NO_MONITOR);
    safety_ack(&s, &in);
    /* ALERT pin while ON */
    safety_request_arm(&s, 3500, &in, tv); safety_confirm(&s, 3501, tv);
    safety_inputs_t al = in; al.alert_pin = true;
    safety_update(&s, 3502, &al, tv);
    CHECK(s.state == OUT_FAULT && s.fault == FLT_ALERT_PIN);
    CHECK(!safety_ack(&s, &al));
    safety_ack(&s, &in);
    /* UV needs to persist 500 ms */
    safety_request_arm(&s, 4000, &in, tv); safety_confirm(&s, 4001, tv);
    safety_inputs_t uv = in; uv.v_out = 3.1f;
    safety_update(&s, 4100, &uv, tv); safety_update(&s, 4400, &uv, tv);
    CHECK(s.state == OUT_ON);
    safety_update(&s, 4700, &uv, tv);
    CHECK(s.state == OUT_FAULT && s.fault == FLT_UNDERVOLTAGE);
    /* output off is always one action (except FAULT stays FAULT) */
    safety_ack(&s, &in); safety_request_arm(&s, 5000, &in, tv); safety_confirm(&s, 5001, tv);
    safety_off(&s);
    CHECK(s.state == OUT_OFF);
}

/* FW-REVIEW-001 #2 regression: EVERY profile in the firmware registry that is not VERIFIED
 * (all 33 today, including iPhone 12 Pro Max IN_DEVELOPMENT and the BENCH profile) must refuse
 * ARM and must never reach ON, with perfect measured telemetry. The API has no override
 * parameter, so this holds for every build variant that links this code. */
static void test_unverified_lock(void) {
    safety_limits_t L = {4.45f, 3.30f, 3.50f, 10000, 500};
    safety_inputs_t good = {TQ_MEASURED, 4.0f, 0.2f, 4.05f, true, false};
    int refused = 0;
    for (int i = 0; i < profile_count(); i++) {
        const batt_profile_t *p = profile_get(i);
        if (p->status == PROF_VERIFIED) continue;
        safety_t s; safety_init(&s, &L);
        bool ok = safety_request_arm(&s, 0, &good, p) == ARM_REFUSED_PROFILE && s.state == OUT_OFF;
        ok = ok && !safety_confirm(&s, 1, p) && !safety_output_enabled(&s);
        /* defence in depth: even if state were forced to ARMED/ON, the next update trips it */
        s.state = OUT_ON;
        safety_update(&s, 2, &good, p);
        ok = ok && s.state == OUT_FAULT && s.fault == FLT_UNSUPPORTED_MODEL && !safety_output_enabled(&s);
        s.state = OUT_OFF; s.fault = FLT_NONE;
        s.state = OUT_ARMED; s.armed_at = 0;
        ok = ok && !safety_confirm(&s, 3, p) && s.state == OUT_OFF;
        if (expect_armable(p)) { CHECK(!ok && profile_armable(p)); continue; }   /* owner unlock: DATA TEST only */
        if (!ok) printf("  profile %s NOT locked\n", p->id);
        CHECK(ok);
        if (ok) refused++;
    }
    CHECK(refused == profile_count() - expect_armable_count());   /* every other registry profile is locked */
    CHECK(profile_count_armable() == expect_armable_count());
    /* owner: 19 phone models of families X..14 (X, XS, XS Max, XR, 11 x3, 12 B2 x4, 13 x4, 14 x4; review 106) + the two
     * 12 Pro capture references; default: none */
    CHECK(expect_armable_count() == (OWNER ? 19 + 2 * CHIPLER_12P_REFERENCE : 0));
    CHECK(!profile_armable(profile_get(profile_find("P12PM-r0"))));
    CHECK(!profile_armable(profile_get(profile_find("BENCH-r0"))));
    CHECK(!profile_armable(0));
}

static void test_engine(void) {
    target_engine_t e;
    const batt_profile_t *bench = profile_get(profile_find("BENCH-r0"));
    const batt_profile_t *p12 = profile_get(profile_find("P12PM-r0"));
    te_init(&e, bench);
    /* write pointer 0x03, repeated start, read 3 bytes */
    te_on_receive(&e, 0x03, 10); te_on_finish(&e);
    uint8_t r0 = te_on_request(&e, 20), r1 = te_on_request(&e, 21), r2 = te_on_request(&e, 22); te_on_finish(&e);
    CHECK(r0 == (0x03 ^ 0xA5) && r1 == (0x04 ^ 0xA5) && r2 == (0x05 ^ 0xA5));
    /* write to scratch then read back */
    te_on_receive(&e, 0x80, 30); te_on_receive(&e, 0x11, 31); te_on_receive(&e, 0x22, 32); te_on_finish(&e);
    te_on_receive(&e, 0x80, 40); te_on_finish(&e);
    CHECK(te_on_request(&e, 41) == 0x11 && te_on_request(&e, 42) == 0x22); te_on_finish(&e);
    /* write to read-only reg is ignored + counted */
    te_on_receive(&e, 0x01, 50); te_on_receive(&e, 0x99, 51); te_on_finish(&e);
    CHECK(e.ignored_writes == 1);
    /* log ring */
    txn_rec_t r; int n = 0; while (te_pop(&e, &r)) n++;
    CHECK(n == 6 && e.txns == 6);
    /* real-phone profile: no fabricated data -> 0xFF + counted */
    te_init(&e, p12);
    te_on_receive(&e, 0x08, 1); te_on_finish(&e);
    CHECK(te_on_request(&e, 2) == 0xFF && e.unknown_reads == 1); te_on_finish(&e);
    CHECK(te_pop(&e, &r) && r.reg == 0x08 && (r.flags & TXN_PTR));
    CHECK(te_pop(&e, &r) && (r.flags & TXN_UNKNOWN) && r.nr == 1);

    /* pointer behaviour across repeated START vs STOP (documented model, same as bq27xxx-style
     * gauges: the pointer persists until a new pointer byte is written). pico_i2c_slave raises
     * FINISH on both STOP and RESTART, so both paths go through te_on_finish(). */
    te_init(&e, bench);
    te_on_receive(&e, 0x02, 1); te_on_finish(&e);                 /* write ptr, then STOP */
    CHECK(te_on_request(&e, 2) == (0x02 ^ 0xA5)); te_on_finish(&e); /* new txn after STOP */
    CHECK(te_on_request(&e, 3) == (0x03 ^ 0xA5)); te_on_finish(&e); /* auto-increment persists */
    te_on_receive(&e, 0x0E, 4); te_on_finish(&e);
    CHECK(te_on_request(&e, 5) == (0x0E ^ 0xA5) && te_on_request(&e, 6) == (0x0F ^ 0xA5));
    CHECK(te_on_request(&e, 7) == 0xFF);                            /* past block end: unknown, 0xFF */
    te_on_finish(&e);
    CHECK(e.unknown_reads == 1);
    /* FINISH without data (address-only probe) does not create a record */
    uint32_t before = e.txns; te_on_finish(&e); CHECK(e.txns == before);
    /* ring overflow is counted, never blocks */
    te_init(&e, bench);
    for (int i = 0; i < TXN_RING + 5; i++) { te_on_receive(&e, 0x00, (uint32_t)i); te_on_finish(&e); }
    CHECK(e.overflows == 5);
}

static void test_profiles(void) {
    int verified = 0, with_regs = 0;
    for (int i = 0; i < profile_count(); i++) {
        const batt_profile_t *p = profile_get(i);
        if (p->status == PROF_VERIFIED) verified++;
        if (p->regs && p->status != PROF_BENCH_TEST) with_regs++;
    }
    CHECK(verified == 0);     /* nothing is phone-verified yet */
    CHECK(with_regs == 0);    /* no invented iPhone register data */
    CHECK(profile_count() == 36 + 2 * CHIPLER_12P_REFERENCE);   /* +P12P-C1/C2 recorded references (DeepSeek 12P) */
}

/* CHARGE TEST (CP-09 merged UI): direction classification, readiness, never-start, navigation */
static void test_charge_logic(void) {
    /* direction / availability */
    CHECK(chg_flow(TQ_MEASURED, true, 0, 0.500f) == CHF_SUPPLYING);
    CHECK(chg_flow(TQ_MEASURED, true, 0, -0.500f) == CHF_RECEIVING);
    CHECK(chg_flow(TQ_MEASURED, true, 0, 0.010f) == CHF_NO_NET);
    CHECK(chg_flow(TQ_MEASURED, true, 0, -0.010f) == CHF_NO_NET);
    CHECK(chg_flow(TQ_MEASURED, true, 0, CHG_I_THRESH_A) == CHF_SUPPLYING);
    CHECK(chg_flow(TQ_SIMULATED, true, 0, 0.3f) == CHF_SUPPLYING);          /* UI labels it SIMULATED */
    CHECK(chg_flow(TQ_ABSENT, false, 0, 0.5f) == CHF_UNAVAILABLE);          /* never a number for "no sensor" */
    CHECK(chg_flow(TQ_ABSENT, true, 1800, 0.5f) == CHF_STALE);
    CHECK(chg_flow(TQ_ABSENT, true, CHG_STALE_SHOW_MS + 1, 0.5f) == CHF_UNAVAILABLE);
    float nan = 0.0f / 0.0f;
    CHECK(chg_flow(TQ_MEASURED, true, 0, nan) == CHF_UNAVAILABLE);
    /* readiness: every check must PASS; UNKNOWN is not ready; simulated sensors never count */
    chg_checks_t k;
    chg_eval_checks(&k, true, true, TQ_MEASURED, true, true, true);  CHECK(chg_start_allowed(&k));
    chg_eval_checks(&k, false, true, TQ_MEASURED, true, true, true); CHECK(!chg_start_allowed(&k) && k.c[CHK_PROFILE] == CHK_FAIL);
    chg_eval_checks(&k, true, false, TQ_MEASURED, true, true, true); CHECK(!chg_start_allowed(&k) && k.c[CHK_SINK] == CHK_FAIL);
    chg_eval_checks(&k, true, true, TQ_SIMULATED, true, true, true); CHECK(!chg_start_allowed(&k) && k.c[CHK_SENSORS] == CHK_FAIL);
    chg_eval_checks(&k, true, true, TQ_MEASURED, false, true, true); CHECK(!chg_start_allowed(&k) && k.c[CHK_CHARGER] == CHK_UNKNOWN);
    chg_eval_checks(&k, true, true, TQ_MEASURED, true, false, true); CHECK(!chg_start_allowed(&k) && k.c[CHK_CHARGER] == CHK_FAIL);
    chg_eval_checks(&k, true, true, TQ_MEASURED, true, true, false); CHECK(!chg_start_allowed(&k) && k.c[CHK_RECIPE] == CHK_FAIL);
    /* this firmware: no sink, no input sensor -> start is impossible for every registry profile */
    CHECK(CHG_SINK_FITTED == 0 && CHG_INPUT_SENSOR_FITTED == 0);
    for (int i = 0; i < profile_count(); i++) {
        chg_eval_checks(&k, profile_armable(profile_get(i)), CHG_SINK_FITTED != 0, TQ_MEASURED,
                        CHG_INPUT_SENSOR_FITTED != 0, false, false);
        CHECK(!chg_start_allowed(&k));
    }
}

static void test_charge_nav(void) {
    ui_nav_t n; int arg = -1; const char *toast;
    nav_ctx_t c = {OUT_OFF, profile_find("P12PM-r0"), 50, 0, false};
    nav_init(&n, profile_count()); n.chg_ui = true; n.screen = SCR_MENU; n.menu_cur = MI_SOC;
    nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast); nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast);
    CHECK(n.menu_cur == MI_CHARGE);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CHG && n.chg_page == 0);
    /* six pages, UP/DOWN wrap */
    for (int i = 1; i <= CHG_PAGES; i++) { nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast); CHECK(n.chg_page == i % CHG_PAGES); }
    nav_handle(&n, BTN_EV_UP, &c, &arg, &toast); CHECK(n.chg_page == CHG_PAGES - 1);
    /* OK on any page -> charge menu at START */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CHG_MENU && n.chg_menu_cur == CMI_START);
    /* START opens the checklist confirm on CANCEL; blocked start can never be highlighted */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CONFIRM && n.confirm == CONF_CHG_START && n.confirm_cur == 0);
    CHECK(nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast) == ACT_NONE && n.confirm_cur == 0 && toast);
    CHECK(nav_handle(&n, BTN_EV_UP, &c, &arg, &toast) == ACT_NONE && n.confirm_cur == 0);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CHG_MENU);   /* cancel */
    /* even a forced APPLY on a blocked start yields no action */
    nav_open_confirm(&n, CONF_CHG_START, 0, 0, 0); n.confirm_cur = 1;
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && toast && n.screen == SCR_CHG);
    /* only an all-PASS context could request a start (firmware main refuses it again: no sink) */
    nav_ctx_t ready = c; ready.chg_start_ok = true;
    nav_open_confirm(&n, CONF_CHG_START, 0, 0, 0);
    nav_handle(&n, BTN_EV_DOWN, &ready, &arg, &toast); CHECK(n.confirm_cur == 1);
    CHECK(nav_handle(&n, BTN_EV_OK, &ready, &arg, &toast) == ACT_CHG_START);
    /* hold OK on every charge screen = OUTPUT OFF; on the confirm it closes back to the charge pages */
    screen_t scr[] = {SCR_CHG, SCR_CHG_MENU};
    for (unsigned i = 0; i < 2; i++) { n.screen = scr[i]; CHECK(nav_handle(&n, BTN_EV_OK_LONG, &c, &arg, &toast) == ACT_OUTPUT_OFF && n.screen == scr[i]); }
    nav_open_confirm(&n, CONF_CHG_START, 0, 0, 0); n.confirm_cur = 1;
    CHECK(nav_handle(&n, BTN_EV_OK_LONG, &c, &arg, &toast) == ACT_OUTPUT_OFF && n.screen == SCR_CHG && n.confirm == CONF_NONE);
    /* confirm timeout returns to the charge pages, no action */
    nav_open_confirm(&n, CONF_CHG_START, 0, 0, 100);
    c.now = 100 + CONFIRM_TIMEOUT_MS + 1; CHECK(nav_tick(&n, &c) == ACT_NONE && n.screen == SCR_CHG); c.now = 0;
    /* charge menu items */
    n.screen = SCR_CHG_MENU; n.chg_menu_cur = CMI_STOP;   CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_CHG_STOP && n.screen == SCR_CHG);
    n.screen = SCR_CHG_MENU; n.chg_menu_cur = CMI_RESULT; CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.chg_page == CHG_PAGES - 1);
    n.screen = SCR_CHG_MENU; n.chg_menu_cur = CMI_OFF;    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_OUTPUT_OFF && n.screen == SCR_CHG);
    n.screen = SCR_CHG_MENU; n.chg_menu_cur = CMI_EXIT;   CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU && n.menu_cur == MI_CHARGE);
    n.screen = SCR_CHG_MENU; n.chg_menu_cur = CMI_BACK;   CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CHG);
    n.screen = SCR_CHG_MENU; n.chg_menu_cur = CMI_START;  nav_handle(&n, BTN_EV_UP, &c, &arg, &toast); CHECK(n.chg_menu_cur == CMI_BACK);
#if CHIPLER_OWNER_PHONE_TEST
    /* SOC from charge menu (owner picker): same confirm flow, returns to charge mode; cancel keeps value */
    n.screen = SCR_CHG_MENU; n.chg_menu_cur = CMI_SOC;
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast); CHECK(n.screen == SCR_SOC && n.soc_ret == SCR_CHG && n.soc_pick == c.soc_current);
    nav_handle(&n, BTN_EV_UP, &c, &arg, &toast); nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);
    CHECK(n.screen == SCR_CONFIRM && n.confirm == CONF_SOC && n.confirm_cur == 0);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_SOC);     /* cancel */
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast); nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_SOC_APPLY && arg == c.soc_current + 1 && n.screen == SCR_CHG);
    n.screen = SCR_SOC;
    CHECK(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CHG_MENU && n.chg_menu_cur == CMI_SOC);
    n.screen = SCR_MENU; n.menu_cur = MI_SOC; nav_handle(&n, BTN_EV_OK, &c, &arg, &toast); CHECK(n.soc_ret == SCR_MENU);
    CHECK(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU);
#else
    /* SOC from charge menu: same CR-003 flow, returns to charge mode; cancel keeps value */
    n.screen = SCR_CHG_MENU; n.chg_menu_cur = CMI_SOC;
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast); CHECK(n.screen == SCR_SOC && n.soc_cur == 1 && n.soc_ret == SCR_CHG);
    nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast); nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);
    CHECK(n.screen == SCR_CONFIRM && n.confirm == CONF_SOC && n.confirm_cur == 0);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_SOC);     /* cancel */
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast); nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast);
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_SOC_APPLY && arg == 100 && n.screen == SCR_CHG);
    n.screen = SCR_SOC; n.soc_cur = 3;                                                        /* Back */
    CHECK(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CHG_MENU && n.chg_menu_cur == CMI_SOC);
    /* SOC from the main menu still returns to MENU / DASH as before */
    n.screen = SCR_MENU; n.menu_cur = MI_SOC; nav_handle(&n, BTN_EV_OK, &c, &arg, &toast); CHECK(n.soc_ret == SCR_MENU);
    n.soc_cur = 3; nav_handle(&n, BTN_EV_OK, &c, &arg, &toast); CHECK(n.screen == SCR_MENU);
#endif
    /* a fault overrides charge screens */
    n.screen = SCR_CHG; nav_sync(&n, OUT_FAULT); CHECK(n.screen == SCR_FAULT); nav_sync(&n, OUT_OFF);
    /* charge-mode actions never touch the safety state machine */
    CHECK(ACT_CHG_START != ACT_ARM_REQUEST && ACT_CHG_START != ACT_ARM_CONFIRM);
}

/* CR-005 (Gate 2 runs 1-4: torn=1 of 3000 every time): a read before the FIRST publish returns
 * 0xFF x4 (no snapshot, 0x20 unknown) -> the bench counted it as torn. Reproduce + show the fix. */
static void test_snapshot_before_publish(void) {
    target_engine_t e; const batt_profile_t *bench = profile_get(profile_find("BENCH-r0"));
    te_init(&e, bench);
    uint8_t r[4];
    te_on_receive(&e, 0x20, 1); te_on_finish(&e);
    for (int i = 0; i < 4; i++) r[i] = te_on_request(&e, 2 + i);
    te_on_finish(&e);
    CHECK(r[0] == 0xFF && r[1] == 0xFF && r[2] == 0xFF && r[3] == 0xFF && e.unknown_reads == 4);
    uint16_t units = r[1] | (r[2] << 8);
    CHECK(!(soc_valid(r[0]) && units == r[0] * 40));          /* exactly what the bench flagged */
    /* fix: publish before the first read -> first read is consistent */
    uint8_t img[8], base, len; bench->soc_build(25, img, &base, &len);
    CHECK(te_publish(&e, base, img, len));
    te_on_receive(&e, 0x20, 10); te_on_finish(&e);
    for (int i = 0; i < 4; i++) r[i] = te_on_request(&e, 11 + i);
    te_on_finish(&e);
    CHECK(r[0] == 25 && (r[1] | r[2] << 8) == 1000 && r[3] == (uint8_t)(r[0] + r[1] + r[2]));
}

#include "test_hal.c"
#include "test_ina226.c"
#include "test_smbus.c"

int main(void) {
    test_buttons(); test_nav(); test_soc_menu(); test_soc_ctrl(); test_snapshot_atomic(); test_freshness();
    test_output_gate(); test_safety(); test_unverified_lock(); test_engine(); test_profiles();
    test_charge_logic(); test_charge_nav();
    test_snapshot_before_publish(); test_ina228_init(); test_log_ring(); test_ctrl_cycle_order(); test_bus_log_budget(); test_ina226_id_verdict(); test_ina226_core(); test_smbus(); test_target_prepare();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
