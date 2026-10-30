/* DS-TOUCH-12 host tests â€” production touch_core.c / settings_store.c from commit 385c87d.
 *
 * Two build modes:
 *   default        -> verifies the CURRENT behaviour of the frozen commit. The defects are expected
 *                     and printed as "DEFECT <id>" lines; the checks pass when the defect is present.
 *   -DDS_PATCHED   -> links patched/touch_core.c (proposed DS-TOUCH-12 patch) and verifies the FIXED
 *                     behaviour instead. Every fix is proven by the same check ids.
 *
 * Compile (MSVC, C mode â€” the production files use C99 compound literals):
 *   cl /nologo /TC /W3 /I <src>\input /I <src>\settings  tests\ds_touch12_tests.c  <touch_core.c> <settings_store.c>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "touch_core.h"
#include "settings_store.h"

static unsigned checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __func__, __LINE__, #c); } } while (0)
#define DEFECT(id, desc) printf("DEFECT %s: %s\n", id, desc)

/* ---------- synthetic resistive panel: raw follows screen with swapped/inverted axes + skew ---------- */
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
static touch_raw_t raw_xy(uint16_t x, uint16_t y) { touch_raw_t r; r.x = x; r.y = y; r.z = 900; r.pen = true; return r; }

static bool calibrate(touch_core_t *t, uint16_t w, uint16_t h) {
    tch_pt_t tgt[5], raw[5];
    touch_cal_targets(5, w, h, tgt);
    for (int i = 0; i < 5; i++) synth_raw(tgt[i].x, tgt[i].y, &raw[i].x, &raw[i].y);
    touch_cal_t c; float worst = 0;
    if (!touch_cal_solve(raw, tgt, 5, w, h, 0, &c, &worst)) return false;
    touch_core_set_cal(t, &c);
    return true;
}

typedef struct { uint32_t down, up, tap, longp, none; tch_event_t last; } evcount_t;
static tch_event_t feed_n(touch_core_t *t, uint32_t *now, const touch_raw_t *r, unsigned n, evcount_t *c) {
    tch_event_t first = {TCH_EV_NONE, 0, 0};
    for (unsigned i = 0; i < n; i++) {
        *now += 10;
        tch_event_t e = touch_core_feed(t, *now, r);
        if (c) {
            switch (e.kind) {
            case TCH_EV_DOWN: c->down++; break;
            case TCH_EV_UP: c->up++; break;
            case TCH_EV_TAP: c->tap++; break;
            case TCH_EV_LONG: c->longp++; break;
            default: c->none++; break;
            }
            if (e.kind != TCH_EV_NONE) c->last = e;
        }
        if (e.kind != TCH_EV_NONE && first.kind == TCH_EV_NONE) first = e;
    }
    return first;
}

/* ================================================================================================ */
static void test_stuck_touch(void) {
    printf("\n-- T1 stuck touch / no-pen recovery --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));
    uint32_t now = 1000; evcount_t c = {0};
    touch_raw_t p = pen_at(160, 120);

    feed_n(&t, &now, &p, 3, &c);                 /* press accepted on poll 3 */
    CHECK(c.down == 1);
    feed_n(&t, &now, &p, 900, &c);               /* 9 s of a pen that is reported as permanently down */
#ifdef DS_PATCHED
    CHECK(c.longp == 1);                         /* long press still reported once */
    CHECK(c.up == 1);                            /* forced release after TCH_STUCK_MS */
    CHECK(t.down == false);
    CHECK(t.stuck >= 1);
    DEFECT("H1-fixed", "stuck press is force-released, panel is not locked out");
    uint32_t presses = t.presses;
    feed_n(&t, &now, &p, 100, &c);               /* pen still stuck: must stay silent until a real pen-up */
    CHECK(t.presses == presses);
    CHECK(c.tap == 0);
    feed_n(&t, &now, no_pen(), 3, &c);          /* real pen-up clears the lock */
    CHECK(c.tap == 0 || c.tap == 0);
    feed_n(&t, &now, &p, 3, &c);                 /* and input works again */
    CHECK(t.presses == presses + 1);
#else
    CHECK(c.longp == 1);
    CHECK(c.up == 0 && c.tap == 0);              /* no release of any kind while the pen is reported down */
    CHECK(t.down == true);
    DEFECT("H1", "no stuck-touch timeout: a pen reported down forever keeps the UI in a held press (only power cycle recovers)");
#endif
    /* a normal press/release still classifies as TAP after the sequence */
    touch_core_t t2; touch_core_init(&t2, 320, 240); CHECK(calibrate(&t2, 320, 240));
    uint32_t n2 = 1000; evcount_t c2 = {0};
    touch_raw_t q = pen_at(60, 40);
    feed_n(&t2, &n2, &q, 3, &c2); feed_n(&t2, &n2, no_pen(), 3, &c2);
    CHECK(c2.down == 1 && c2.tap == 1 && t2.down == false);
}

static void test_bus_error_is_not_release(void) {
    printf("\n-- T2 failed read (NULL sample) must not fabricate a release --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));
    uint32_t now = 1000; evcount_t c = {0};
    touch_raw_t p = pen_at(160, 120);

    /* no press yet: NULL samples are not contact */
    feed_n(&t, &now, 0, 10, &c);
    CHECK(c.down == 0 && c.up == 0 && c.tap == 0);
#ifdef DS_PATCHED
    CHECK(t.bus_err == 10);
#else
    /* frozen struct has no bus_err counter */
#endif

    feed_n(&t, &now, &p, 4, &c);                 /* press established */
    CHECK(c.down == 1 && t.down == true);
    evcount_t c2 = {0};
    feed_n(&t, &now, 0, 5, &c2);                 /* 5 failed SPI reads in a row (bus refused) */
#ifdef DS_PATCHED
    CHECK(c2.tap == 0 && c2.up == 0);            /* no fake release */
    CHECK(t.down == true);                       /* the press survives the read error */
    CHECK(t.bus_err >= 15);
    DEFECT("H2-fixed", "a failed read freezes the press instead of turning it into a TAP");
    /* the real release is still classified correctly */
    feed_n(&t, &now, no_pen(), 3, &c2);
    CHECK(c2.tap == 1);
#else
    CHECK(c2.tap >= 1);                          /* 3 NULL feeds == 30 ms of "no pen" -> TAP at the press point */
    CHECK(t.down == false);
    DEFECT("H2", "3 consecutive failed reads are treated as pen-up: a transient SPI/bus error during a press emits a TAP and can activate the button under the finger");
#endif
}

static void test_rotation_mid_press(void) {
    printf("\n-- T3 release after rotation / screen change while a press is held --\n");

    /* (a) rotation changes while the contact is still held and further samples arrive:
       the mapped point mirrors, so the press is already classified as a move -> UP, not TAP.
       (This is why the current UI cannot mis-navigate on rotation: it only rotates from a TAP.) */
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));               /* calibration taken at rot 0 */
    uint32_t now = 1000; evcount_t c = {0};
    touch_raw_t p = pen_at(60, 40);               /* deliberately not the centre: mirror is (259,199) */
    feed_n(&t, &now, &p, 3, &c);
    CHECK(c.down == 1 && abs(c.last.x - 60) <= 2 && abs(c.last.y - 40) <= 2);
    touch_core_set_rotation(&t, 1);
    feed_n(&t, &now, &p, 2, &c);
    evcount_t a = {0};
    feed_n(&t, &now, no_pen(), 3, &a);
#ifdef DS_PATCHED
    CHECK(a.tap == 0 && a.up == 0);               /* cancelled at the rotation, nothing is delivered */
    CHECK(t.canceled >= 1 && t.down == false);
    DEFECT("H3a-fixed", "rotation cancels the held press; a stale-orientation contact cannot produce a TAP");
#else
    CHECK(a.tap == 0 && a.up == 1);               /* coerced to UP by the mirrored mapping: no navigation */
    DEFECT("ok", "rotation mid-press is already classified as a move (UP, no navigation) in the frozen build");
#endif

    /* (b) the hazard window: the rotation happens and the release is classified before any further
       pen sample is mapped. The TAP then carries pre-rotation coordinates, which on the rotated
       screen are the mirrored region (259,199), i.e. a different button. */
    touch_core_t u; touch_core_init(&u, 320, 240); CHECK(calibrate(&u, 320, 240));
    uint32_t n2 = 1000; evcount_t c2 = {0};
    touch_raw_t q = pen_at(60, 40);
    feed_n(&u, &n2, &q, 3, &c2);
    CHECK(c2.down == 1);
    touch_core_set_rotation(&u, 1);
    evcount_t b = {0};
    feed_n(&u, &n2, no_pen(), 3, &b);
#ifdef DS_PATCHED
    CHECK(b.tap == 0 && b.up == 0);
#else
    CHECK(b.tap == 1);
    CHECK(abs(b.last.x - 60) <= 2 && abs(b.last.y - 40) <= 2);  /* stale orientation coordinates */
    DEFECT("H3b", "a release classified in the same gap as a rotation keeps the pre-rotation coordinates; on the rotated screen the mirrored point (259,199) is a different target. Not reachable from the current UI (rotation needs a TAP) but there is no cancel hook");
#endif

    /* (c) the reachable variant: the screen changes while a contact is held (AUTOCAL: the dashboard
       press that triggers "calibration offered" is still down when SCR_CAL appears). The touch layer
       emits a TAP for that release; main.c then consumes it as calibration point 1. */
    touch_core_t v; touch_core_init(&v, 320, 240); CHECK(calibrate(&v, 320, 240));
    uint32_t n3 = 1000; evcount_t c3 = {0};
    touch_raw_t r = pen_at(100, 200);
    feed_n(&v, &n3, &r, 3, &c3);
    CHECK(c3.down == 1);
#ifdef DS_PATCHED
    touch_core_cancel(&v);                        /* what main.c must call when the screen changes */
    evcount_t d = {0};
    feed_n(&v, &n3, no_pen(), 3, &d);
    CHECK(d.tap == 0 && d.up == 0);
    CHECK(v.canceled >= 1);
    DEFECT("H3c-fixed", "a screen change can cancel the held contact; the triggering release is not consumed by the new screen");
#else
    evcount_t d = {0};
    feed_n(&v, &n3, no_pen(), 3, &d);
    CHECK(d.tap == 1);
    DEFECT("H3c", "no cancel-on-screen-change API exists: the release of the press that triggered AUTOCAL is a TAP on SCR_CAL and main.c stores it as calibration point 1 (wrong raw point, no target check)");
#endif
}
static void test_rotation_mapping_180(void) {
    printf("\n-- T4 180-degree mapping --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));              /* cal.rot180 = 0 */
    float rx, ry; synth_raw(60, 40, &rx, &ry);
    int16_t ax, ay, bx, by, cx, cy;
    touch_core_set_rotation(&t, 0);
    CHECK(touch_core_map(&t, (uint16_t)(rx + 0.5f), (uint16_t)(ry + 0.5f), &ax, &ay));
    CHECK(abs(ax - 60) <= 2 && abs(ay - 40) <= 2);
    touch_core_set_rotation(&t, 1);
    CHECK(touch_core_map(&t, (uint16_t)(rx + 0.5f), (uint16_t)(ry + 0.5f), &bx, &by));
    CHECK(abs(bx - (319 - ax)) <= 1 && abs(by - (239 - ay)) <= 1);
    CHECK(abs(bx - 259) <= 3 && abs(by - 199) <= 3);
    touch_core_set_rotation(&t, 0);
    CHECK(touch_core_map(&t, (uint16_t)(rx + 0.5f), (uint16_t)(ry + 0.5f), &cx, &cy));
    CHECK(cx == ax && cy == ay);
    /* a calibration taken at rot 180 used at rot 0 is the same exact mirror, both directions */
    touch_core_t u; touch_core_init(&u, 320, 240);
    tch_pt_t tgt[5], raw[5];
    touch_cal_targets(5, 320, 240, tgt);
    for (int i = 0; i < 5; i++) synth_raw(tgt[i].x, tgt[i].y, &raw[i].x, &raw[i].y);
    touch_cal_t c; float w = 0;
    CHECK(touch_cal_solve(raw, tgt, 5, 320, 240, 1, &c, &w));
    CHECK(c.rot180 == 1);
    touch_core_set_cal(&u, &c);
    touch_core_set_rotation(&u, 1);
    int16_t dx, dy;
    CHECK(touch_core_map(&u, (uint16_t)(rx + 0.5f), (uint16_t)(ry + 0.5f), &dx, &dy));
    CHECK(abs(dx - 60) <= 2 && abs(dy - 40) <= 2);
    touch_core_set_rotation(&u, 0);
    CHECK(touch_core_map(&u, (uint16_t)(rx + 0.5f), (uint16_t)(ry + 0.5f), &dx, &dy));
    CHECK(abs(dx - 259) <= 3 && abs(dy - 199) <= 3);
}

static void test_outliers(void) {
    printf("\n-- T5 outlier rejection vs a real move --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));
    uint32_t now = 1000; evcount_t c = {0};
    touch_raw_t p = pen_at(160, 120);
    feed_n(&t, &now, &p, 6, &c);                 /* press established, median history filled */
    CHECK(t.down == true);

    /* (a) a single-poll spike is dropped and does not break the press */
    touch_raw_t spike = p; spike.x = (uint16_t)(p.x > 2000 ? p.x - 1500 : p.x + 1500);
    uint32_t o0 = t.outliers; evcount_t cs = {0};
    feed_n(&t, &now, &spike, 1, &cs);
    CHECK(t.outliers == o0 + 1 && t.down == true && cs.tap == 0 && cs.up == 0);
    DEFECT("ok", "single-poll spike is rejected (intended filter behaviour)");

    /* (b) 9 s of a *real* fast drag (>TCH_OUTLIER_RAW per poll) while the press is held */
    touch_raw_t far = pen_at(160, 170);          /* 50 px in screen Y == 750 raw counts on raw X */
    uint32_t o1 = t.outliers;
    evcount_t cd = {0};
    feed_n(&t, &now, &far, 20, &cd);
#ifdef DS_PATCHED
    CHECK(t.resync >= 1);                        /* persistent move is accepted as a new baseline */
    CHECK(abs(t.last_sy - 170) <= 3);
    CHECK(t.moved == true);
    feed_n(&t, &now, no_pen(), 3, &cd);
    CHECK(cd.tap == 0 && cd.up == 1);            /* classified as a drag, not a tap */
    DEFECT("H4-fixed", "a sustained fast drag is re-seeded and classified as a move (UP), not a TAP");
#else
    CHECK(t.outliers == o1 + 20);                /* every sample rejected */
    CHECK(abs(t.last_sy - 120) <= 3);            /* coordinates frozen at the old position */
    CHECK(t.moved == false);
    feed_n(&t, &now, no_pen(), 3, &cd);
    CHECK(cd.tap == 1);                          /* released => TAP at the ORIGINAL position */
    CHECK(abs(cd.last.x - 160) <= 2 && abs(cd.last.y - 120) <= 2);
    DEFECT("H4", "a fast drag (>700 raw/poll) is rejected forever: the finger position never updates, so the release is a TAP at the old position -> accidental activation under a sliding finger");
#endif

    /* (c) a sustained jump to a new position (finger moved and held) is eventually tracked */
    touch_core_t u; touch_core_init(&u, 320, 240); CHECK(calibrate(&u, 320, 240));
    uint32_t n2 = 1000; evcount_t c2 = {0};
    touch_raw_t q = pen_at(160, 120);
    feed_n(&u, &n2, &q, 6, &c2);
    touch_raw_t q2 = pen_at(60, 120);            /* raw Y jumps by -1150 counts */
    feed_n(&u, &n2, &q2, 10, &c2);
#ifdef DS_PATCHED
    CHECK(abs(u.last_sx - 60) <= 3);
#else
    CHECK(abs(u.last_sx - 160) <= 3);
#endif
}

static void test_rails_and_debounce(void) {
    printf("\n-- T6 ADC-rail boundaries and debounce timing --\n");
    touch_core_t t; touch_core_init(&t, 320, 240);
    CHECK(calibrate(&t, 320, 240));
    uint32_t now = 1000;
    touch_raw_t r39 = raw_xy(39, 200), r40 = raw_xy(40, 200);
    touch_raw_t hi = raw_xy(4055, 200), hi2 = raw_xy(4056, 200);
    evcount_t c = {0};
    feed_n(&t, &now, &r39, 10, &c);
    CHECK(c.down == 0 && t.rejected_raw >= 10);   /* 39 < TCH_RAW_MIN -> rejected */
    feed_n(&t, &now, &r40, 5, &c);
    CHECK(c.down == 1);                            /* 40 is the first accepted value */
    feed_n(&t, &now, no_pen(), 5, &c);
    evcount_t c2 = {0}; uint32_t pr = t.presses;
    feed_n(&t, &now, &hi, 5, &c2);
    CHECK(c2.down == 1);
    feed_n(&t, &now, no_pen(), 5, &c2);
    feed_n(&t, &now, &hi2, 10, &c2);
    CHECK(t.presses == pr + 1);                    /* 4056 rejected, no new press */
    /* debounce counts polls, not time: 3 down polls 100 ms apart still take 3 polls (300 ms) to press */
    touch_core_t u; touch_core_init(&u, 320, 240); CHECK(calibrate(&u, 320, 240));
    uint32_t n2 = 1000; evcount_t c3 = {0}; touch_raw_t p = pen_at(100, 100);
    for (int i = 0; i < 2; i++) { n2 += 100; tch_event_t e = touch_core_feed(&u, n2, &p); CHECK(e.kind == TCH_EV_NONE); }
    n2 += 100; CHECK(touch_core_feed(&u, n2, &p).kind == TCH_EV_DOWN);
    DEFECT("T1", "press/release debounce is 3 polls of a nominal 10 ms; a 65 ms LCD frame push stretches it to ~200-300 ms");
    /* a 2-poll contact is not a press at all */
    touch_core_t v; touch_core_init(&v, 320, 240); CHECK(calibrate(&v, 320, 240));
    uint32_t n3 = 1000; evcount_t c4 = {0};
    feed_n(&v, &n3, &p, 2, &c4); feed_n(&v, &n3, no_pen(), 5, &c4);
    CHECK(c4.down == 0 && c4.tap == 0);
}

static void test_calibration(void) {
    printf("\n-- T7 calibration validity --\n");
    /* 3-point fit: residual is zero by construction, so TCH_CAL_MAX_ERR_PX cannot reject a
       systematically shifted 3-point calibration. Production main.c uses CAL_POINTS=5. */
    tch_pt_t tgt[3], raw[3];
    touch_cal_targets(3, 320, 240, tgt);
    for (int i = 0; i < 3; i++) synth_raw(tgt[i].x, tgt[i].y, &raw[i].x, &raw[i].y);
    for (int i = 0; i < 3; i++) { raw[i].x += 40; raw[i].y -= 40; }   /* user tapped 40 raw counts off, consistently */
    touch_cal_t c; float worst = -1;
    bool ok = touch_cal_solve(raw, tgt, 3, 320, 240, 0, &c, &worst);
    CHECK(ok);
    CHECK(worst < 1e-3f);            /* exact 3-point fit: residual ~ 0 by construction */
    DEFECT("H5", "a 3-point calibration always has residual 0: it cannot be validated (acceptance: keep CAL_POINTS>=5 and document the 3-point API as unvalidated)");
    /* 5-point fit with a bad point IS rejected by the residual limit */
    tch_pt_t t5[5], r5[5];
    touch_cal_targets(5, 320, 240, t5);
    for (int i = 0; i < 5; i++) synth_raw(t5[i].x, t5[i].y, &r5[i].x, &r5[i].y);
    r5[3].x += 900; r5[3].y -= 700;
    touch_cal_t c5; float w5 = 0;
    CHECK(!touch_cal_solve(r5, t5, 5, 320, 240, 0, &c5, &w5));
    CHECK(w5 > TCH_CAL_MAX_ERR_PX);
    /* degenerate / invalid inputs */
    touch_cal_t cz;
    { tch_pt_t a[3] = {{100,100},{200,200},{300,300}}, s[3] = {{10,10},{20,20},{30,30}};
      CHECK(!touch_cal_solve(a, s, 3, 320, 240, 0, &cz, 0)); }
    { tch_pt_t one[3] = {{500,500},{500,500},{500,500}};
      CHECK(!touch_cal_solve(one, one, 3, 320, 240, 0, &cz, 0)); }
    CHECK(!touch_cal_solve(0, 0, 5, 320, 240, 0, &cz, 0));
    /* mapping clamps to the screen and refuses without calibration */
    touch_core_t t; touch_core_init(&t, 320, 240);
    int16_t x, y;
    CHECK(!touch_core_map(&t, 2000, 2000, &x, &y));
    CHECK(calibrate(&t, 320, 240));
    CHECK(touch_core_map(&t, 0, 0, &x, &y) && x >= 0 && x <= 319 && y >= 0 && y <= 239);
    CHECK(touch_core_map(&t, 4095, 4095, &x, &y) && x >= 0 && x <= 319 && y >= 0 && y <= 239);
}

/* ---------------- settings persistence: CRC / version / corruption ---------------- */
static void test_settings_crc(void) {
    printf("\n-- T8 settings CRC / version / corruption --\n");
    /* known-answer vector for CRC32/ISO-HDLC, independent of the implementation under test */
    CHECK(settings_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);

    ui_settings_t s; settings_defaults(&s);
    s.rot180 = 1;
    s.cal.valid = true; s.cal.rot180 = 1; s.cal.w = 320; s.cal.h = 240;
    s.cal.a = 0.0773f; s.cal.b = 0.0011f; s.cal.c = -12.5f;
    s.cal.d = -0.0009f; s.cal.e = -0.0862f; s.cal.f = 287.3f;
    uint8_t blob[SETTINGS_BLOB_LEN], good[SETTINGS_BLOB_LEN];
    settings_pack(&s, blob);
    memcpy(good, blob, sizeof blob);
    ui_settings_t b;
    CHECK(settings_unpack(blob, &b));
    CHECK(b.rot180 == 1 && b.cal.valid && b.cal.rot180 == 1 && b.cal.w == 320 && b.cal.h == 240);
    CHECK(memcmp(&b.cal.a, &s.cal.a, 4) == 0 && memcmp(&b.cal.f, &s.cal.f, 4) == 0);  /* float round-trip */
    CHECK(settings_crc32(good, 60) == (uint32_t)(good[60] | (good[61] << 8) | (good[62] << 16) | ((uint32_t)good[63] << 24)));

    /* every single-byte corruption of the covered area must be rejected */
    unsigned rejected = 0, tried = 0;
    for (unsigned i = 0; i < 60; i++) {
        memcpy(blob, good, sizeof blob);
        blob[i] ^= 0x01u;
        tried++;
        ui_settings_t tmp;
        if (!settings_unpack(blob, &tmp)) rejected++;
    }
    printf("   single-byte corruption: %u/%u rejected\n", rejected, tried);
    CHECK(rejected == tried);
    /* corruption of the stored CRC itself */
    memcpy(blob, good, sizeof blob); blob[62] ^= 0x80u;
    { ui_settings_t tmp; CHECK(!settings_unpack(blob, &tmp)); }
    /* wrong magic / version / length / reserved flag bytes */
    memcpy(blob, good, sizeof blob); blob[0] ^= 0xFFu; { ui_settings_t tmp; CHECK(!settings_unpack(blob, &tmp)); }
    memcpy(blob, good, sizeof blob); blob[4] = 2; { ui_settings_t tmp; CHECK(!settings_unpack(blob, &tmp)); }
    memcpy(blob, good, sizeof blob); blob[6] = 63; { ui_settings_t tmp; CHECK(!settings_unpack(blob, &tmp)); }
    memcpy(blob, good, sizeof blob); blob[8] = 2; { ui_settings_t tmp; CHECK(!settings_unpack(blob, &tmp)); }
    memcpy(blob, good, sizeof blob); blob[9] = 2; { ui_settings_t tmp; CHECK(!settings_unpack(blob, &tmp)); }
    memcpy(blob, good, sizeof blob); blob[10] = 2; { ui_settings_t tmp; CHECK(!settings_unpack(blob, &tmp)); }
    /* semantic range checks on a CRC-valid blob: NaN / Inf / huge coefficient / zero dimensions */
    for (int which = 0; which < 4; which++) {
        ui_settings_t bad = s; uint8_t bb[SETTINGS_BLOB_LEN]; ui_settings_t tmp;
        if (which == 0) bad.cal.a = NAN;
        if (which == 1) bad.cal.e = INFINITY;
        if (which == 2) bad.cal.c = 5000.0f;
        if (which == 3) bad.cal.w = 0;
        settings_pack(&bad, bb);
        CHECK(!settings_unpack(bb, &tmp));
    }
    /* erased flash sector (power loss after erase) and a torn mix of two blobs */
    memset(blob, 0xFF, sizeof blob);
    { ui_settings_t tmp; CHECK(!settings_unpack(blob, &tmp)); }
    { ui_settings_t old; settings_defaults(&old); old.rot180 = 0; uint8_t ob[SETTINGS_BLOB_LEN]; settings_pack(&old, ob);
      memcpy(blob, good, sizeof blob); memcpy(blob + 40, ob + 40, 24);   /* header new, tail old */
      ui_settings_t tmp; CHECK(!settings_unpack(blob, &tmp)); }
    /* a blob that says "not calibrated" is accepted and yields defaults for the cal fields */
    ui_settings_t n; settings_defaults(&n); n.rot180 = 1;
    uint8_t nb[SETTINGS_BLOB_LEN]; settings_pack(&n, nb);
    ui_settings_t out; CHECK(settings_unpack(nb, &out));
    CHECK(out.rot180 == 1 && out.cal.valid == false);
    /* unpack failure never leaves partially-applied values behind */
    ui_settings_t pre; settings_defaults(&pre); pre.rot180 = 1; pre.cal.valid = true;
    memcpy(blob, good, sizeof blob); blob[20] ^= 0xFFu;   /* break a coefficient + its CRC */
    CHECK(!settings_unpack(blob, &pre));
    CHECK(pre.rot180 == 0 && pre.cal.valid == false);     /* defaults applied on failure */
}

int main(void) {
#ifdef DS_PATCHED
    printf("DS-TOUCH-12 host tests â€” PATCHED touch_core.c (proposed fix)\n");
#else
    printf("DS-TOUCH-12 host tests â€” FROZEN commit 385c87d (defects expected)\n");
#endif
    printf("touch_core.h defines: PRESS=%u RELEASE=%u TAP_MAX=%ums MOVE=%upx LONG=%ums RAW=[%u..%u] OUTLIER=%u CAL_ERR=%.1fpx\n",
           (unsigned)TCH_PRESS_POLLS, (unsigned)TCH_RELEASE_POLLS, (unsigned)TCH_TAP_MAX_MS, (int)TCH_TAP_MOVE_PX,
           (unsigned)TCH_LONG_MS, (unsigned)TCH_RAW_MIN, (unsigned)TCH_RAW_MAX, (unsigned)TCH_OUTLIER_RAW,
           (double)TCH_CAL_MAX_ERR_PX);
    test_stuck_touch();
    test_bus_error_is_not_release();
    test_rotation_mid_press();
    test_rotation_mapping_180();
    test_outliers();
    test_rails_and_debounce();
    test_calibration();
    test_settings_crc();
    printf("\nRESULT: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}



