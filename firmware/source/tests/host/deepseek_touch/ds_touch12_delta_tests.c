/* DS-TOUCH-12 DELTA tests â€” commit f698c76 (touch_core.c/settings_store.c unchanged vs 385c87d).
 *
 * Focus: the new main.c failsafe (TCH_EV_LONG on any non-CAL/non-STARTUP screen -> UIC_STOP) and how it
 * interacts with the touch release path, the stuck-touch path and the DS-TOUCH-12 patch.
 *
 * Two modes, same as the main suite:
 *   default       -> frozen touch_core.c of f698c76 (identical to 385c87d)
 *   -DDS_PATCHED  -> patched/touch_core.c (proposed DS-TOUCH-12 fix)
 *
 * The failsafe itself lives in main.c and is NOT executed here (main.c is not host-buildable); these tests
 * verify the EVENTS main.c consumes: LONG must be emitted exactly once per stationary hold, must not be
 * emitted for a drag, and must not be duplicated by the patch's forced release / cancel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "touch_core.h"

static unsigned checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __func__, __LINE__, #c); } } while (0)
#define NOTE(id, s) printf("NOTE %s: %s\n", id, s)

static void synth_raw(float sx, float sy, float *rx, float *ry) {
    *rx = 3900.0f - sy * 15.0f + sx * 0.8f;
    *ry = 180.0f + sx * 11.5f - sy * 0.6f;
}
static touch_raw_t pen_at(float sx, float sy) {
    float rx, ry; synth_raw(sx, sy, &rx, &ry);
    touch_raw_t r; r.x = (uint16_t)(rx + 0.5f); r.y = (uint16_t)(ry + 0.5f); r.z = 900; r.pen = true;
    return r;
}
static touch_raw_t NONE_RAW = {0, 0, 0, false};
static const touch_raw_t *no_pen(void) { return &NONE_RAW; }

static bool calibrate(touch_core_t *t, uint16_t w, uint16_t h) {
    tch_pt_t tgt[5], raw[5];
    touch_cal_targets(5, w, h, tgt);
    for (int i = 0; i < 5; i++) synth_raw(tgt[i].x, tgt[i].y, &raw[i].x, &raw[i].y);
    touch_cal_t c; float worst = 0;
    if (!touch_cal_solve(raw, tgt, 5, w, h, 0, &c, &worst)) return false;
    touch_core_set_cal(t, &c);
    return true;
}

typedef struct { unsigned down, up, tap, longp, none; tch_event_t last; } ev_t;
static void feed(touch_core_t *t, uint32_t *now, const touch_raw_t *r, unsigned n, ev_t *c) {
    for (unsigned i = 0; i < n; i++) {
        *now += 10;
        tch_event_t e = touch_core_feed(t, *now, r);
        switch (e.kind) {
        case TCH_EV_DOWN: c->down++; break;
        case TCH_EV_UP: c->up++; break;
        case TCH_EV_TAP: c->tap++; break;
        case TCH_EV_LONG: c->longp++; break;
        default: c->none++; break;
        }
        if (e.kind != TCH_EV_NONE) c->last = e;
    }
}

/* 1. the failsafe trigger: a stationary hold emits LONG exactly once, at >= TCH_LONG_MS */
static void t_long_trigger(void) {
    printf("\n-- D1 failsafe trigger: stationary hold -> exactly one TCH_EV_LONG --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));
    uint32_t now = 1000; ev_t c = {0};
    touch_raw_t p = pen_at(160, 120);
    feed(&t, &now, &p, 3, &c);                 /* press accepted at poll 3 (~30 ms) */
    CHECK(c.down == 1 && c.longp == 0);
    feed(&t, &now, &p, 130, &c);               /* hold past 1200 ms measured from the accepted press */
    CHECK(c.longp == 1);
    CHECK(c.last.x == 160 || abs(c.last.x - 160) <= 2);
    feed(&t, &now, &p, 100, &c);               /* still held: the failsafe must not repeat */
    CHECK(c.longp == 1);
    feed(&t, &now, no_pen(), 3, &c);
    CHECK(c.tap == 0);                          /* a long hold is not a tap: no navigation command */
    NOTE("main.c:150-153", "failsafe = UIC_STOP pushed on the single TCH_EV_LONG; LONG is consumed here, so the STOP fires once per hold");
}

/* 2. a drag must not fire the failsafe */
static void t_drag_no_long(void) {
    printf("\n-- D2 drag must NOT trigger the failsafe --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));
    uint32_t now = 1000; ev_t c = {0};
    touch_raw_t p0 = pen_at(60, 60);
    feed(&t, &now, &p0, 3, &c);
    CHECK(c.down == 1);
    for (int i = 0; i < 150; i++) {             /* slow drag over 1.5 s, 17 px total (below the tap-move limit) */
        touch_raw_t pi = pen_at(60 + i * 0.4f, 60);
        feed(&t, &now, &pi, 1, &c);
    }
    CHECK(c.longp == 0);                        /* "stationary" is enforced by touch_core's moved flag */
    NOTE("safety", "the failsafe cannot fire while the finger is moving (TCH_TAP_MOVE_PX 18): no accidental STOP during a drag");
}

/* 3. under the patch: forced release at 8 s does not duplicate the LONG/STOP and leaves no TAP */
static void t_stuck_after_long(void) {
    printf("\n-- D3 patch: stuck hold -> LONG once, forced UP once, no TAP, no second STOP trigger --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));
    uint32_t now = 1000; ev_t c = {0};
    touch_raw_t p = pen_at(160, 120);
    feed(&t, &now, &p, 3, &c);
    feed(&t, &now, &p, 900, &c);                /* 9 s stuck */
#ifdef DS_PATCHED
    CHECK(c.longp == 1);                        /* failsafe trigger still fires exactly once */
    CHECK(c.up == 1);                           /* forced release after TCH_STUCK_MS */
    CHECK(c.tap == 0);                          /* and it is never a TAP */
    CHECK(t.stuck >= 1 && t.down == false);
    feed(&t, &now, &p, 100, &c);
    CHECK(c.longp == 1 && c.up == 1);           /* stuck_lock: no repeat, no new press */
    NOTE("H1-fixed", "the failsafe STOP still fires once, and a stuck panel now releases instead of freezing the UI");
#else
    CHECK(c.longp == 1);
    CHECK(c.up == 0 && c.tap == 0);             /* frozen: never releases */
    NOTE("H1", "frozen build: failsafe STOP fires once at 1.2 s, but the held contact never ends (input stays dead)");
#endif
}

/* 4. cancel (screen change / rotation) must not create a LONG or a TAP afterwards */
static void t_cancel_no_long(void) {
    printf("\n-- D4 cancel on screen change: no stray LONG/TAP --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));
    uint32_t now = 1000; ev_t c = {0};
    touch_raw_t p = pen_at(100, 200);
    feed(&t, &now, &p, 3, &c);
    CHECK(c.down == 1);
#ifdef DS_PATCHED
    touch_core_cancel(&t);                      /* what main.c does when the screen changes */
#endif
    feed(&t, &now, &p, 200, &c);                /* keep holding >2 s after the change */
    feed(&t, &now, no_pen(), 3, &c);
#ifdef DS_PATCHED
    CHECK(c.longp == 0);                        /* cancelled contact cannot fire the failsafe afterwards */
    CHECK(c.tap == 0 && c.up == 0);
    NOTE("H3c-fixed", "a contact that began on the previous screen cannot trigger STOP or a TAP on the new one");
#else
    CHECK(c.longp == 1);                        /* frozen: the held contact becomes a LONG -> UIC_STOP on the NEW screen */
    CHECK(c.tap == 0);
    NOTE("H3c", "frozen build: after a screen change the still-held contact fires LONG -> the failsafe pushes STOP with the NEW screen id (safe action, but not user-initiated)");
#endif
}

/* 5. LONG must be re-armable: second hold on a new screen fires again (failsafe is not one-shot per boot) */
static void t_failsafe_rearm(void) {
    printf("\n-- D5 failsafe is per-hold, not one-shot --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));
    uint32_t now = 1000; ev_t c = {0};
    for (int hold = 0; hold < 3; hold++) {
        touch_raw_t ph = pen_at(160, 120);
        feed(&t, &now, &ph, 130, &c);              /* press + 1.3 s hold */
        feed(&t, &now, no_pen(), 4, &c);              /* release */
    }
    CHECK(c.longp == 3);                        /* one LONG per hold */
    CHECK(c.tap == 0);
    NOTE("failsafe", "each new stationary hold re-arms the failsafe; the LONG is per-contact");
}

/* 6. NEW INTERACTION (f698c76): the failsafe cannot cover the H4 fast-drag defect. A swipe faster than
 * TCH_OUTLIER_RAW per poll is rejected, the position freezes, and because the frozen code returns early
 * from the outlier branch, the LONG evaluation is never reached -> the failsafe long-press STOP is also
 * unreachable while the contact is stuck in that state. */
static void t_drag_rejected_then_stop(void) {
    printf("\n-- D6 fast swipe rejected -> failsafe unreachable in that state (coverage gap) --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));
    uint32_t now = 1000; ev_t c = {0};
    touch_raw_t p = pen_at(160, 120);
    feed(&t, &now, &p, 6, &c);                  /* press; history filled */
    CHECK(c.down == 1);
    touch_raw_t far = pen_at(160, 170);          /* 50 px swipe => 750 raw counts per poll (> 700) */
    feed(&t, &now, &far, 20, &c);                /* 200 ms of real movement, all rejected as "outliers" */
    feed(&t, &now, &far, 140, &c);               /* finger now rests at the new place for 1.4 s */
#ifdef DS_PATCHED
    CHECK(t.resync >= 1);                        /* the move is accepted as a new baseline */
    CHECK(t.moved == true);
    CHECK(c.longp == 0);                         /* a real swipe is never a "stationary hold" */
    NOTE("N1-fixed", "with the patch a fast swipe is tracked as a move, so it cannot trigger the failsafe STOP");
#else
    CHECK(t.moved == false);                     /* position frozen at the press point */
    CHECK(c.longp == 0);                         /* the outlier early-return bypasses the LONG evaluation */
    NOTE("N1", "frozen build + f698c76 failsafe coverage gap: while every sample is an outlier (frozen median) the LONG is never evaluated, so neither input nor the long-press STOP works until the finger is lifted (H4 impact = dead input, not STOP)");
#endif
}
int main(void) {
#ifdef DS_PATCHED
    printf("DS-TOUCH-12 DELTA tests â€” f698c76 + DS-TOUCH-12 patch\n");
#else
    printf("DS-TOUCH-12 DELTA tests â€” frozen f698c76 (touch_core identical to 385c87d)\n");
#endif
    t_long_trigger();
    t_drag_no_long();
    t_stuck_after_long();
    t_cancel_no_long();
    t_failsafe_rearm();
    t_drag_rejected_then_stop();
    printf("\nRESULT: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}




