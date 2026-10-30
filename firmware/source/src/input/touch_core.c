#include "touch_core.h"
#include <math.h>
#include <string.h>

static uint16_t med3(uint16_t a, uint16_t b, uint16_t c) {
    if (a > b) { uint16_t t = a; a = b; b = t; }
    if (b > c) { b = c; }
    return a > b ? a : b;
}

void touch_core_init(touch_core_t *t, uint16_t w, uint16_t h) {
    memset(t, 0, sizeof *t);
    t->w = w; t->h = h;
}

void touch_core_set_cal(touch_core_t *t, const touch_cal_t *cal) {
    if (cal) t->cal = *cal; else memset(&t->cal, 0, sizeof t->cal);
}

static void reset_press(touch_core_t *t) {
    t->down = false; t->down_cnt = 0; t->up_cnt = 0; t->nh = 0; t->moved = false; t->long_sent = false;
}

/* DS-TOUCH-12: changing the mapping under a live contact must not leave a press that will be released
 * with the old orientation's coordinates. Cancel silently and wait for a real pen-up. */
void touch_core_set_rotation(touch_core_t *t, uint8_t rot180) {
    rot180 = rot180 ? 1 : 0;
    if (t->rot180 != rot180) {
        if (t->down || t->down_cnt) { reset_press(t); t->canceled++; t->stuck_lock = true; }
        t->nh = 0; t->out_run = 0;
    }
    t->rot180 = rot180;
}

/* DS-TOUCH-12: called by main.c when the screen changes while a contact is held. AUTOCAL is the
 * reachable case: the dashboard press that triggers "calibration offered" is still down when SCR_CAL
 * appears, so its release would otherwise be consumed as calibration point 1. */
void touch_core_cancel(touch_core_t *t) {
    if (!t->down && !t->down_cnt) return;
    reset_press(t);
    t->canceled++;
    t->stuck_lock = true;      /* swallow the rest of this contact: the release must not become a TAP */
}

bool touch_core_map(const touch_core_t *t, uint16_t rx, uint16_t ry, int16_t *sx, int16_t *sy) {
    if (!t->cal.valid) return false;
    float x = t->cal.a * rx + t->cal.b * ry + t->cal.c;
    float y = t->cal.d * rx + t->cal.e * ry + t->cal.f;
    /* calibration was taken with rotation cal.rot180; a different current rotation is a 180 deg flip */
    if (t->cal.rot180 != t->rot180) { x = (float)(t->w - 1) - x; y = (float)(t->h - 1) - y; }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > t->w - 1) x = (float)(t->w - 1);
    if (y > t->h - 1) y = (float)(t->h - 1);
    *sx = (int16_t)(x + 0.5f); *sy = (int16_t)(y + 0.5f);
    return true;
}

tch_event_t touch_core_feed(touch_core_t *t, uint32_t now, const touch_raw_t *raw) {
    tch_event_t ev = {TCH_EV_NONE, 0, 0};
    t->polls++;
    /* DS-TOUCH-12: a failed read is a bus error, not "no pen". The frozen code treats raw == NULL as
     * pen-up, so 3 consecutive SPI failures fabricated a TAP at the press point. */
    if (!raw) { t->bus_err++; return ev; }
    bool pen = raw->pen;
    /* AG-TOUCH: record what the controller reported BEFORE rail rejection so TOUCH DIAG shows it */
    if (raw->z) t->last_rz = raw->z;   /* sub-threshold pressure is visible too */
    if (pen) { t->last_rx = raw->x; t->last_ry = raw->y; }
    if (pen) {
        if (raw->x < TCH_RAW_MIN || raw->x > TCH_RAW_MAX || raw->y < TCH_RAW_MIN || raw->y > TCH_RAW_MAX) {
            t->rejected_raw++; pen = false;    /* ADC rail = floating/invalid, treat as no contact */
        }
    }
    /* DS-TOUCH-12: after a forced release / cancel, swallow the rest of this contact */
    if (t->stuck_lock) {
        if (!pen) {
            t->stuck_lock = false; t->down = false; t->down_cnt = 0; t->up_cnt = 0;
            t->nh = 0; t->moved = false; t->long_sent = false; t->out_run = 0;
        }
        return ev;
    }
    if (pen) {
        t->ever_pen = true;
        t->last_rx = raw->x; t->last_ry = raw->y; t->last_rz = raw->z;
        bool resynced = false;
        /* single-poll outlier rejection once a median history exists */
        if (t->nh >= 3) {
            int dx = (int)raw->x - (int)med3(t->hx[0], t->hx[1], t->hx[2]);
            int dy = (int)raw->y - (int)med3(t->hy[0], t->hy[1], t->hy[2]);
            if (dx > TCH_OUTLIER_RAW || dx < -TCH_OUTLIER_RAW || dy > TCH_OUTLIER_RAW || dy < -TCH_OUTLIER_RAW) {
                t->outliers++;
                if (++t->out_run < TCH_OUTLIER_RESYNC) return ev;
                /* a jump that persists for several polls is a move, not a spike: re-seed the filter so
                 * a fast drag keeps tracking instead of freezing the position until release */
                t->out_run = 0; t->resync++; resynced = true;
                t->hx[0] = t->hx[1] = t->hx[2] = raw->x;
                t->hy[0] = t->hy[1] = t->hy[2] = raw->y;
                t->nh = 3;
            } else {
                t->out_run = 0;
            }
        }
        if (!resynced) {
            t->hx[t->nh % 3] = raw->x; t->hy[t->nh % 3] = raw->y;
            if (t->nh < 255) t->nh++;
        }
        t->up_cnt = 0;
        if (t->down_cnt < 255) t->down_cnt++;
        if (!t->down && t->down_cnt >= TCH_PRESS_POLLS && t->nh >= 3) {
            uint16_t mx = med3(t->hx[0], t->hx[1], t->hx[2]), my = med3(t->hy[0], t->hy[1], t->hy[2]);
            int16_t sx, sy;
            if (!touch_core_map(t, mx, my, &sx, &sy)) {   /* uncalibrated: report raw/2 so diag still moves; flagged by cal */
                sx = (int16_t)(mx >> 4); sy = (int16_t)(my >> 4);
            }
            t->press_rx = mx; t->press_ry = my;
            t->down = true; t->down_at = now; t->x0 = t->x = sx; t->y0 = t->y = sy;
            t->moved = false; t->long_sent = false; t->presses++;
            t->last_sx = sx; t->last_sy = sy;
            ev.kind = TCH_EV_DOWN; ev.x = sx; ev.y = sy;
            return ev;
        }
        if (t->down) {
            /* DS-TOUCH-12: a contact that never lifts is a stuck panel / wrong controller. Release it
             * once (as UP, never TAP) and ignore the rest of that contact until a real pen-up. */
            if ((uint32_t)(now - t->down_at) >= TCH_STUCK_MS) {
                t->stuck++;
                ev.kind = TCH_EV_UP; ev.x = t->x; ev.y = t->y;
                reset_press(t);
                t->stuck_lock = true;
                return ev;
            }
            uint16_t mx = med3(t->hx[0], t->hx[1], t->hx[2]), my = med3(t->hy[0], t->hy[1], t->hy[2]);
            int16_t sx, sy;
            if (!touch_core_map(t, mx, my, &sx, &sy)) { sx = (int16_t)(mx >> 4); sy = (int16_t)(my >> 4); }
            t->x = sx; t->y = sy; t->last_sx = sx; t->last_sy = sy;
            int mdx = sx - t->x0, mdy = sy - t->y0;
            if (mdx * mdx + mdy * mdy > TCH_TAP_MOVE_PX * TCH_TAP_MOVE_PX) t->moved = true;
            if (!t->moved && !t->long_sent && (uint32_t)(now - t->down_at) >= TCH_LONG_MS) {
                t->long_sent = true; t->longs++;
                ev.kind = TCH_EV_LONG; ev.x = t->x; ev.y = t->y;
                return ev;
            }
        }
        return ev;
    }
    /* no pen */
    t->down_cnt = 0;
    t->out_run = 0;
    if (t->down) {
        if (t->up_cnt < 255) t->up_cnt++;
        if (t->up_cnt >= TCH_RELEASE_POLLS) {
            bool tap = !t->moved && !t->long_sent && (uint32_t)(now - t->down_at) <= TCH_TAP_MAX_MS;
            ev.x = t->x0; ev.y = t->y0;   /* a tap is reported where it started */
            ev.kind = tap ? TCH_EV_TAP : TCH_EV_UP;
            if (tap) t->taps++;
            reset_press(t);
            return ev;
        }
    } else {
        t->nh = 0;
    }
    return ev;
}

/* ---- calibration ---- */
static bool solve3(double m[3][4]) {
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++) if (fabs(m[r][c]) > fabs(m[p][c])) p = r;
        if (fabs(m[p][c]) < 1e-9) return false;
        if (p != c) for (int k = 0; k < 4; k++) { double t = m[c][k]; m[c][k] = m[p][k]; m[p][k] = t; }
        for (int r = 0; r < 3; r++) {
            if (r == c) continue;
            double f = m[r][c] / m[c][c];
            for (int k = c; k < 4; k++) m[r][k] -= f * m[c][k];
        }
    }
    for (int r = 0; r < 3; r++) m[r][3] /= m[r][r];
    return true;
}

bool touch_cal_solve(const tch_pt_t *raw, const tch_pt_t *scr, unsigned n, uint16_t w, uint16_t h,
                     uint8_t rot180, touch_cal_t *out, float *worst_err) {
    if (!raw || !scr || !out || n < 3 || n > TCH_CAL_MAX_POINTS) return false;
    /* normal equations for [x y 1] * [a b c]^T = sx and = sy */
    double sxx = 0, sxy = 0, sx = 0, syy = 0, sy = 0, s1 = n;
    double bx0 = 0, bx1 = 0, bx2 = 0, by0 = 0, by1 = 0, by2 = 0;
    for (unsigned i = 0; i < n; i++) {
        double x = raw[i].x, y = raw[i].y;
        sxx += x * x; sxy += x * y; sx += x; syy += y * y; sy += y;
        bx0 += x * scr[i].x; bx1 += y * scr[i].x; bx2 += scr[i].x;
        by0 += x * scr[i].y; by1 += y * scr[i].y; by2 += scr[i].y;
    }
    double mx[3][4] = {{sxx, sxy, sx, bx0}, {sxy, syy, sy, bx1}, {sx, sy, s1, bx2}};
    double my[3][4] = {{sxx, sxy, sx, by0}, {sxy, syy, sy, by1}, {sx, sy, s1, by2}};
    if (!solve3(mx) || !solve3(my)) return false;
    touch_cal_t c;
    memset(&c, 0, sizeof c);
    c.a = (float)mx[0][3]; c.b = (float)mx[1][3]; c.c = (float)mx[2][3];
    c.d = (float)my[0][3]; c.e = (float)my[1][3]; c.f = (float)my[2][3];
    c.w = w; c.h = h; c.rot180 = rot180 ? 1 : 0;
    float worst = 0;
    for (unsigned i = 0; i < n; i++) {
        float px = c.a * raw[i].x + c.b * raw[i].y + c.c, py = c.d * raw[i].x + c.e * raw[i].y + c.f;
        float er = hypotf(px - scr[i].x, py - scr[i].y);
        if (er > worst) worst = er;
    }
    if (worst_err) *worst_err = worst;
    /* sanity: the mapping must span the screen (a degenerate or collapsed fit is rejected) */
    float span_x = fabsf(c.a) + fabsf(c.b), span_y = fabsf(c.d) + fabsf(c.e);
    if (!(span_x > 0.01f) || !(span_y > 0.01f) || span_x > 1.0f || span_y > 1.0f) return false;
    if (worst > TCH_CAL_MAX_ERR_PX) return false;
    c.valid = true;
    *out = c;
    return true;
}

void touch_cal_targets(unsigned n, uint16_t w, uint16_t h, tch_pt_t *out) {
    const float mx = 28.0f, my = 24.0f;
    if (n == 3) {
        out[0] = (tch_pt_t){mx, my};
        out[1] = (tch_pt_t){(float)w - mx, h / 2.0f};
        out[2] = (tch_pt_t){w / 2.0f, (float)h - my};
        return;
    }
    out[0] = (tch_pt_t){mx, my};
    out[1] = (tch_pt_t){(float)w - mx, my};
    out[2] = (tch_pt_t){(float)w - mx, (float)h - my};
    out[3] = (tch_pt_t){mx, (float)h - my};
    out[4] = (tch_pt_t){w / 2.0f, h / 2.0f};
}
