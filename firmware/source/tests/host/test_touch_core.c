/* P1_r1 touch input layer: calibration fit, mapping/rotation, filter, debounce, tap/long/move classification. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../src/input/touch_core.h"

static unsigned fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* a synthetic panel: raw = f(screen) with swapped/inverted axes and a slight skew, like a real resistive panel */
static void synth_raw(float sx, float sy, float *rx, float *ry) {
    *rx = 3900.0f - sy * 15.0f + sx * 0.8f;      /* raw X follows screen Y (axes swapped), inverted */
    *ry = 180.0f + sx * 11.5f - sy * 0.6f;
}

static touch_raw_t pen(float sx, float sy, float noise) {
    float rx, ry; synth_raw(sx, sy, &rx, &ry);
    touch_raw_t r = {(uint16_t)(rx + noise), (uint16_t)(ry - noise), 900, true};
    return r;
}

static unsigned rng_state = 12345;
static float rnd(float amp) { rng_state = rng_state * 1664525u + 1013904223u; return ((int)(rng_state >> 16) % 2001 - 1000) / 1000.0f * amp; }

static bool do_cal(touch_core_t *t, unsigned n, float noise_raw, float *worst) {
    tch_pt_t tgt[5], raw[5]; touch_cal_targets(n, 320, 240, tgt);
    for (unsigned i = 0; i < n; i++) { float rx, ry; synth_raw(tgt[i].x, tgt[i].y, &rx, &ry); raw[i].x = rx + rnd(noise_raw); raw[i].y = ry + rnd(noise_raw); }
    touch_cal_t c;
    if (!touch_cal_solve(raw, tgt, n, 320, 240, 0, &c, worst)) return false;
    touch_core_set_cal(t, &c);
    return true;
}

/* feed N polls of a held touch, returns the first non-NONE event kind seen and records last event */
static tch_event_t run(touch_core_t *t, uint32_t *now, const touch_raw_t *r, unsigned polls, tch_event_t *last_nonnone) {
    tch_event_t first = {TCH_EV_NONE, 0, 0};
    for (unsigned i = 0; i < polls; i++) {
        *now += 10;
        tch_event_t e = touch_core_feed(t, *now, r);
        if (e.kind != TCH_EV_NONE) { if (first.kind == TCH_EV_NONE) first = e; if (last_nonnone) *last_nonnone = e; }
    }
    return first;
}

int main(void) {
    touch_core_t t; touch_core_init(&t, 320, 240);
    float worst = 0;

    /* ---- calibration: exact, noisy, degenerate ---- */
    CHECK(do_cal(&t, 3, 0.0f, &worst) && worst < 0.5f);
    CHECK(do_cal(&t, 5, 6.0f, &worst) && worst < TCH_CAL_MAX_ERR_PX);
    /* mapping accuracy across the screen with the 5-point fit */
    { float maxerr = 0;
      for (int sx = 10; sx < 310; sx += 20) for (int sy = 10; sy < 230; sy += 20) {
          float rx, ry; synth_raw((float)sx, (float)sy, &rx, &ry);
          int16_t mx, my; CHECK(touch_core_map(&t, (uint16_t)rx, (uint16_t)ry, &mx, &my));
          float e = hypotf(mx - sx, my - sy); if (e > maxerr) maxerr = e;
      }
      printf("  5-point fit max mapping error over grid: %.2f px\n", (double)maxerr);
      CHECK(maxerr < 8.0f); }
    /* collinear points => degenerate */
    { tch_pt_t raw[3] = {{100, 100}, {200, 200}, {300, 300}}, scr[3] = {{10, 10}, {20, 20}, {30, 30}}; touch_cal_t c;
      CHECK(!touch_cal_solve(raw, scr, 3, 320, 240, 0, &c, 0)); }
    /* inconsistent taps (user hit the wrong places) => rejected by the residual limit */
    { tch_pt_t tgt[5], raw[5]; touch_cal_targets(5, 320, 240, tgt);
      for (int i = 0; i < 5; i++) { float rx, ry; synth_raw(tgt[i].x, tgt[i].y, &rx, &ry); raw[i].x = rx; raw[i].y = ry; }
      raw[2].x += 900; raw[2].y -= 700;
      touch_cal_t c; float w; CHECK(!touch_cal_solve(raw, tgt, 5, 320, 240, 0, &c, &w) && w > TCH_CAL_MAX_ERR_PX); }
    CHECK(!touch_cal_solve(0, 0, 3, 320, 240, 0, &(touch_cal_t){0}, 0));
    { tch_pt_t a[2] = {{1, 1}, {2, 2}}; touch_cal_t c; CHECK(!touch_cal_solve(a, a, 2, 320, 240, 0, &c, 0)); }

    /* ---- rotation: a calibration taken at rot 0 used at rot 180 is the exact 180-degree flip ---- */
    do_cal(&t, 5, 0.0f, &worst);
    { float rx, ry; synth_raw(60, 40, &rx, &ry); int16_t a, b, c2, d;
      touch_core_set_rotation(&t, 0); touch_core_map(&t, (uint16_t)rx, (uint16_t)ry, &a, &b);
      touch_core_set_rotation(&t, 1); touch_core_map(&t, (uint16_t)rx, (uint16_t)ry, &c2, &d);
      CHECK(abs(a - 60) <= 1 && abs(b - 40) <= 1);
      CHECK(abs(c2 - (319 - a)) <= 1 && abs(d - (239 - b)) <= 1);
      touch_core_set_rotation(&t, 0); }
    /* uncalibrated: never maps */
    { touch_core_t u; touch_core_init(&u, 320, 240); int16_t a, b; CHECK(!touch_core_map(&u, 2000, 2000, &a, &b)); }
    /* clamping */
    { int16_t a, b; touch_core_map(&t, 50, 50, &a, &b); CHECK(a >= 0 && a <= 319 && b >= 0 && b <= 239);
      touch_core_map(&t, 4000, 4000, &a, &b); CHECK(a >= 0 && a <= 319 && b >= 0 && b <= 239); }

    /* ---- events ---- */
    uint32_t now = 1000; tch_event_t last = {0};
    touch_raw_t none = {0, 0, 0, false};
    touch_raw_t p = pen(160, 120, 0);
    /* clean tap: DOWN after 3 polls, TAP after 3 released polls, reported where it started */
    tch_event_t e = run(&t, &now, &p, 2, &last); CHECK(e.kind == TCH_EV_NONE);     /* not yet debounced */
    e = run(&t, &now, &p, 1, &last); CHECK(e.kind == TCH_EV_DOWN && abs(e.x - 160) <= 2 && abs(e.y - 120) <= 2);
    run(&t, &now, &p, 4, &last);
    e = run(&t, &now, &none, 3, &last); CHECK(e.kind == TCH_EV_TAP && abs(e.x - 160) <= 2 && abs(e.y - 120) <= 2);
    CHECK(t.taps == 1 && t.presses == 1);
    /* 2-poll glitch (too short): nothing */
    uint32_t taps0 = t.taps;
    run(&t, &now, &p, 2, &last); e = run(&t, &now, &none, 5, &last);
    CHECK(e.kind == TCH_EV_NONE && t.taps == taps0);
    /* contact lost for 2 polls inside a press does NOT end it (release needs 3) */
    run(&t, &now, &p, 5, &last); run(&t, &now, &none, 2, &last); e = run(&t, &now, &p, 3, &last); CHECK(e.kind == TCH_EV_NONE);
    e = run(&t, &now, &none, 3, &last); CHECK(e.kind == TCH_EV_TAP);
    /* long press: LONG once, then UP (not TAP) */
    { touch_raw_t q = pen(100, 100, 0); int longs = 0; int up = 0, tap = 0;
      for (int i = 0; i < 160; i++) { now += 10; tch_event_t x = touch_core_feed(&t, now, &q); longs += x.kind == TCH_EV_LONG; }
      for (int i = 0; i < 4; i++) { now += 10; tch_event_t x = touch_core_feed(&t, now, &none); up += x.kind == TCH_EV_UP; tap += x.kind == TCH_EV_TAP; }
      CHECK(longs == 1 && up == 1 && tap == 0); }
    /* long hold that is slow but under TAP limit, then release = TAP */
    { touch_raw_t q = pen(200, 60, 0); int tap = 0;
      for (int i = 0; i < 60; i++) { now += 10; touch_core_feed(&t, now, &q); }
      for (int i = 0; i < 4; i++) { now += 10; tap += touch_core_feed(&t, now, &none).kind == TCH_EV_TAP; }
      CHECK(tap == 1); }
    /* drag beyond tolerance is UP, never TAP (no accidental button under a sliding finger) */
    { int tap = 0, up = 0;
      for (int i = 0; i < 40; i++) { touch_raw_t q = pen(100 + i * 3.0f, 100, 0); now += 10; touch_core_feed(&t, now, &q); }
      for (int i = 0; i < 4; i++) { now += 10; tch_event_t x = touch_core_feed(&t, now, &none); tap += x.kind == TCH_EV_TAP; up += x.kind == TCH_EV_UP; }
      CHECK(tap == 0 && up == 1); }
    /* ADC-rail readings are rejected (floating input) and counted */
    { touch_raw_t rail = {4095, 4095, 900, true}; uint32_t rej = t.rejected_raw, pr = t.presses;
      run(&t, &now, &rail, 10, &last); CHECK(t.rejected_raw >= rej + 10 && t.presses == pr);
      touch_raw_t zero = {0, 0, 900, true}; run(&t, &now, &zero, 10, &last); CHECK(t.presses == pr); }
    /* single-poll outlier inside a press is dropped without ending it */
    { touch_raw_t q = pen(160, 120, 0); run(&t, &now, &q, 6, &last);
      touch_raw_t bad = pen(160, 120, 0); bad.x = (uint16_t)(bad.x > 2000 ? bad.x - 1500 : bad.x + 1500);
      uint32_t o = t.outliers; run(&t, &now, &bad, 1, &last); CHECK(t.outliers == o + 1);
      e = run(&t, &now, &none, 3, &last); CHECK(e.kind == TCH_EV_TAP); }
    /* noise on a held touch never produces extra presses */
    { uint32_t pr = t.presses; for (int i = 0; i < 100; i++) { touch_raw_t q = pen(160, 120, rnd(8)); now += 10; touch_core_feed(&t, now, &q); }
      run(&t, &now, &none, 4, &last); CHECK(t.presses == pr + 1); }
    /* NULL sample (bus refused) behaves like no contact */
    { uint32_t pr = t.presses; for (int i = 0; i < 10; i++) { now += 10; touch_core_feed(&t, now, 0); } CHECK(t.presses == pr); }

    printf("touch_core: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
