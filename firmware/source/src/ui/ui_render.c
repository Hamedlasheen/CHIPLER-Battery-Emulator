/* Option A "Industrial diagnostic" UI (owner-selected 2026-10-15).
 * Coordinates follow mockup_gen.py / UI spec v0.1 (native 240x240, round, centre 120,120). */
#include "ui_render.h"
#include "../gfx/gfx.h"
#include "../gfx/fonts_data.h"
#include "../batt/profile.h"
#include "../batt/soc_ctrl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define CX 120.0f
/* palette (Option A) */
#define C_BG     RGB(0x06080B)
#define C_PANEL  RGB(0x0E131A)
#define C_LINE   RGB(0x1B232E)
#define C_TRACK  RGB(0x141B24)
#define C_TXT    RGB(0xF3F6F9)
#define C_SUB    RGB(0x8C98A8)
#define C_DIM    RGB(0x4B5665)
#define C_MEAS   RGB(0x38D6F5)
#define C_AMP    RGB(0xFFB224)
#define C_SIM    RGB(0xA98BFF)
#define C_OK     RGB(0x2FD39A)
#define C_WARN   RGB(0xFFB224)
#define C_ERR    RGB(0xFF4D4F)
#define C_OK_BG  RGB(0x0F2A22)
#define C_WARN_BG RGB(0x2A2110)
#define C_ERR_BG RGB(0x2A0E10)
#define C_SIM_BG RGB(0x16122A)
#define C_SIM_LN RGB(0x2B2350)

#define MID true
#define BASE false

static void brand_mark(float x, float y, float s, rgb_t col, rgb_t bg) {
    gfx_round_rect(x - s / 2, y - s / 2, x + s / 2, y + s / 2, s * 0.22f, col, true, 0, 0);
    for (int i = -1; i <= 1; i++) {
        float o = i * s * 0.26f;
        float p[4][4] = {{-s * 0.5f - s * 0.12f, o, s * 0.07f, s * 0.05f}, {s * 0.5f + s * 0.12f, o, s * 0.07f, s * 0.05f},
                         {o, -s * 0.5f - s * 0.12f, s * 0.05f, s * 0.07f}, {o, s * 0.5f + s * 0.12f, s * 0.05f, s * 0.07f}};
        for (int k = 0; k < 4; k++) {
            float dx = p[k][0], dy = p[k][1], hw = p[k][2], hh = p[k][3];
            gfx_round_rect(x + dx - hw, y + dy - hh, x + dx + hw, y + dy + hh, fminf(hw, hh) * 0.6f, col, true, 0, 0);
        }
    }
    float r = s * 0.24f;
    gfx_arc_at(x, y, r, s * 0.11f, 45, 315, bg, false);
}

static void header(void) {
    brand_mark(92, 24, 7.5f, C_SUB, C_BG);
    gfx_text(&font_sb9, 100, 24.5f, "CHIPLER", C_SUB, ALIGN_L, MID, 1.6f);
}

static void warn_tri(float x, float y, float s, rgb_t col, rgb_t bg) {
    float h = s * 0.88f;
    gfx_triangle(x, y - h / 2, x + s / 2, y + h / 2, x - s / 2, y + h / 2, col);
    gfx_round_rect(x - s * 0.055f, y - h * 0.18f, x + s * 0.055f, y + h * 0.2f, s * 0.05f, bg, true, 0, 0);
    gfx_circle(x, y + h * 0.33f, s * 0.065f, bg);
}

static void battery_icon(float x, float y, float w, float h, float frac, rgb_t col) {
    gfx_round_rect(x, y, x + w, y + h, 1.6f, 0, false, col, 1.2f);
    gfx_round_rect(x + w + 0.6f, y + h * 0.3f, x + w + 2.2f, y + h * 0.7f, 0.6f, col, true, 0, 0);
    float fw = (w - 3) * frac;
    if (fw > 0.3f) gfx_round_rect(x + 1.5f, y + 1.5f, x + 1.5f + fw, y + h - 1.5f, 0.8f, col, true, 0, 0);
}

static void pill(float cx, float y0, float y1, const char *t, const font_t *f, rgb_t fg, rgb_t bg,
                 bool dot, float trk) {
    float tw = gfx_text_width(f, t, trk);
    float pw = tw + (dot ? 22 : 14);
    float x0 = cx - pw / 2;
    gfx_round_rect(x0, y0, x0 + pw, y1, (y1 - y0) / 2, bg, true, 0, 0);
    float ty = (y0 + y1) / 2 + 0.3f;
    if (dot) {
        gfx_circle(x0 + 9, (y0 + y1) / 2, 2.6f, fg);
        gfx_text(f, x0 + 15, ty, t, fg, ALIGN_L, MID, trk);
    } else gfx_text(f, cx, ty, t, fg, ALIGN_C, MID, trk);
}

static void toast(const ui_model_t *m) {
    if (!m->toast[0] || (int32_t)(m->toast_until - m->now_ms) <= 0) return;
    float tw = gfx_text_width(&font_sb8, m->toast, 0.5f);
    gfx_round_rect(CX - tw / 2 - 10, 192, CX + tw / 2 + 10, 212, 10, C_WARN_BG, true, C_WARN, 1);
    gfx_text(&font_sb8, CX, 202.3f, m->toast, C_WARN, ALIGN_C, MID, 0.5f);
}

static void fmt_num(char *b, size_t n, float v, int dec, bool valid) {
    if (!valid) { snprintf(b, n, dec == 3 ? "-.---" : "-.--"); return; }
    snprintf(b, n, "%.*f", dec, (double)v);
}

/* ---------------- screens ---------------- */
static void scr_dashboard(const ui_model_t *m) {
    const batt_profile_t *p = profile_get(m->prof_idx);
    bool valid = m->tq != TQ_ABSENT;
    const float a0 = 135, a1 = 405;
    gfx_arc(114, 6, a0, a1, C_TRACK, true);
    if (valid) {
        float frac = fabsf(m->a) / 3.0f; if (frac > 1) frac = 1;
        if (frac > 0.003f) gfx_arc(114, 6, a0, a0 + (a1 - a0) * frac, m->a > 3.0f ? C_ERR : C_AMP, true);
    }
    for (int i = 0; i <= 12; i++) {           /* minor + major ticks */
        float a = (a0 + (a1 - a0) * i / 12.0f) * 0.0174533f;
        float r0 = (i % 2) ? 103.5f : 102.0f;
        gfx_line(CX + r0 * cosf(a), 120 + r0 * sinf(a), CX + 106.5f * cosf(a), 120 + 106.5f * sinf(a), 1,
                 (i % 2) ? C_LINE : C_DIM);
    }
    {
        float r = 97, a = a0 * 0.0174533f, b = a1 * 0.0174533f;
        gfx_text(&font_m7, CX + r * cosf(a) + 6, 120 + r * sinf(a) - 1, "0", C_DIM, ALIGN_C, MID, 0);
        gfx_text(&font_m7, CX + r * cosf(b) - 6, 120 + r * sinf(b) - 1, "3A", C_DIM, ALIGN_C, MID, 0);
    }
    header();
    gfx_text(&font_sb12, CX, 40, p ? p->name : "NO PROFILE", C_TXT, ALIGN_C, MID, 0);
    switch (m->link) {
    case LINK_ACTIVE:   pill(CX, 50, 63, "I2C LINK", &font_sb8, C_OK, C_OK_BG, true, 0.6f); break;
    case LINK_IDLE:     pill(CX, 50, 63, "I2C IDLE", &font_sb8, C_WARN, C_WARN_BG, true, 0.6f); break;
    case LINK_ERROR:    pill(CX, 50, 63, "I2C ERROR", &font_sb8, C_ERR, C_ERR_BG, true, 0.6f); break;
    default:            pill(CX, 50, 63, "I2C OFF", &font_sb8, C_SUB, C_PANEL, true, 0.6f); break;
    }
    gfx_text(&font_m7, CX, 74, "VOLTAGE", C_DIM, ALIGN_C, MID, 1.2f);
    char b[16];
    fmt_num(b, sizeof b, m->v, 3, valid);
    float vw = gfx_text_width(&font_d40, b, 0), uw = gfx_text_width(&font_m16, "V", 0);
    float x0 = CX - (vw + 3 + uw) / 2;
    gfx_text(&font_d40, x0, 98, b, valid ? C_TXT : C_DIM, ALIGN_L, MID, 0);
    gfx_text(&font_m16, x0 + vw + 3, 104, "V", C_MEAS, ALIGN_L, BASE, 0);
    gfx_line(CX - 52, 120.5f, CX + 52, 120.5f, 1, C_LINE);
    fmt_num(b, sizeof b, m->a, 3, valid);
    float aw = gfx_text_width(&font_d30, b, 0), auw = gfx_text_width(&font_m14, "A", 0);
    x0 = CX - (aw + 3 + auw) / 2;
    gfx_text(&font_d30, x0, 139, b, valid ? C_AMP : C_DIM, ALIGN_L, MID, 0);
    gfx_text(&font_m14, x0 + aw + 3, 144, "A", C_AMP, ALIGN_L, BASE, 0);
    if (m->tq == TQ_MEASURED)       gfx_text(&font_m7, CX, 157, m->sensor_uncalibrated ? "MEASURED \x80 UNCAL" : "CURRENT \x80 MEASURED", C_DIM, ALIGN_C, MID, 0.9f);
    else if (m->tq == TQ_SIMULATED) gfx_text(&font_m7, CX, 157, "SIMULATED \x80 NOT MEASURED", C_WARN, ALIGN_C, MID, 0.6f);
    else                            gfx_text(&font_m7, CX, 157, "NO POWER MONITOR", C_ERR, ALIGN_C, MID, 0.9f);
    /* cells */
    gfx_round_rect(CX - 66, 166, CX - 3, 190, 6, C_PANEL, true, C_LINE, 1);
    gfx_round_rect(CX + 3, 166, CX + 66, 190, 6, C_SIM_BG, true, C_SIM_LN, 1);
    fmt_num(b, sizeof b, m->w, 2, valid);
    gfx_text(&font_d13, CX - 34.5f, 175.5f, b, valid ? C_TXT : C_DIM, ALIGN_C, MID, 0);
    gfx_text(&font_m6, CX - 34.5f, 185.5f, "POWER  W", C_SUB, ALIGN_C, MID, 0.6f);
    {   /* emulated SOC cell: requested preset; label says whether it actually reached a backend */
        bool sent = m->soc_applied >= 0 && m->soc_applied == m->soc_requested;
        rgb_t vc = sent ? C_SIM : RGB(0x6E6390);
        battery_icon(CX + 9, 171.5f, 12, 7, m->soc_requested / 100.0f, vc);
        snprintf(b, sizeof b, "%d%%", m->soc_requested);
        gfx_text(&font_d13, CX + 25, 175.5f, b, vc, ALIGN_L, MID, 0);
        const char *lab = "SIM  SOC"; rgb_t lc = C_SIM;
        switch (m->soc_result) {
        case SOC_RES_APPLIED_SYNTHETIC: lab = "SIM \x80 BENCH"; break;
        case SOC_RES_APPLIED_VERIFIED:  lab = "SIM  SOC"; break;
        case SOC_RES_BUSY_RETRY:        lab = "SIM \x80 PENDING"; lc = C_WARN; break;
        default:                        lab = "SIM \x80 NOT SENT"; lc = C_WARN; break;
        }
        gfx_text(&font_m6, CX + 34.5f, 185.5f, lab, lc, ALIGN_C, MID, 0.3f);
    }
    switch (m->out) {
    case OUT_ON:    pill(CX, 197, 209, "OUTPUT ON", &font_sb6, C_OK, C_OK_BG, false, 0.6f); break;
    case OUT_ARMED: pill(CX, 197, 209, "ARMED", &font_sb6, C_WARN, C_WARN_BG, false, 0.6f); break;
    default:        pill(CX, 197, 209, "OUTPUT OFF", &font_sb6, C_SUB, C_PANEL, false, 0.6f); break;
    }
    snprintf(b, sizeof b, "%s", p ? p->id : "-");
    char line[48];
    snprintf(line, sizeof line, "PROFILE  %s  \x80  %s", b,
             p && p->status == PROF_VERIFIED ? "VERIFIED" : "UNVERIFIED");
    gfx_text(&font_m6, CX, 218, line, C_DIM, ALIGN_C, MID, 0.3f);
    toast(m);
}

static rgb_t status_col(prof_status_t s) {
    switch (s) {
    case PROF_VERIFIED: return C_OK;
    case PROF_IN_DEVELOPMENT: return C_WARN;
    case PROF_BENCH_TEST: return C_MEAS;
    default: return C_SUB;
    }
}

static const char *model_item(int i) {           /* 0 = BACK, 1..n = profiles */
    if (i == 0) return "\x81 BACK";
    return profile_get(i - 1)->name;
}

static void hints(const char *l1, const char *l2) {
    gfx_round_rect(52, 184, 188, 214, 8, C_PANEL, true, 0, 0);
    gfx_text(&font_m7, CX, 193, l1, C_SUB, ALIGN_C, MID, 0.4f);
    gfx_text(&font_sb6, CX, 205, l2, C_MEAS, ALIGN_C, MID, 0.4f);
}

static void scr_models(const ui_model_t *m) {
    int n = profile_count() + 1, c = m->model_cur;
    const float a0 = -60, a1 = 60;
    gfx_arc(113, 3, a0, a1, C_TRACK, true);
    float pos = a0 + (a1 - a0) * c / (float)(n - 1);
    gfx_arc(113, 3, pos - 4, pos + 4, C_MEAS, true);
    header();
    gfx_text(&font_sb9, CX, 40, "SELECT MODEL", C_SUB, ALIGN_C, MID, 1.4f);
    char b[48];
    if (c > 0) snprintf(b, sizeof b, "FAMILY  \x80  %s", profile_get(c - 1)->family);
    else snprintf(b, sizeof b, "RETURN TO MENU");
    gfx_text(&font_m7, CX, 52, b, C_DIM, ALIGN_C, MID, 0.5f);
    gfx_text(&font_m10, CX, 72, model_item((c + n - 2) % n), C_DIM, ALIGN_C, MID, 0);
    gfx_text(&font_m12, CX, 91, model_item((c + n - 1) % n), C_SUB, ALIGN_C, MID, 0);
    gfx_round_rect(26, 103, 214, 137, 10, C_PANEL, true, C_MEAS, 1.2f);
    if (c == 0) {
        gfx_text(&font_sb15, CX, 119, "\x81 BACK", C_TXT, ALIGN_C, MID, 0);
    } else {
        const batt_profile_t *p = profile_get(c - 1);
        gfx_text(&font_sb15, CX, 114.5f, p->name, C_TXT, ALIGN_C, MID, 0);
        const char *st = profile_status_str(p->status);
        float bw = gfx_text_width(&font_sb6, st, 0.4f) + 12;
        gfx_round_rect(CX - bw / 2, 123.5f, CX + bw / 2, 133, 4.5f,
                       p->status == PROF_IN_DEVELOPMENT ? C_WARN_BG : C_PANEL, true, C_LINE, p->status == PROF_IN_DEVELOPMENT ? 0 : 1);
        gfx_text(&font_sb6, CX, 128.5f, st, status_col(p->status), ALIGN_C, MID, 0.4f);
    }
    gfx_text(&font_m12, CX, 150, model_item((c + 1) % n), C_SUB, ALIGN_C, MID, 0);
    gfx_text(&font_m10, CX, 168, model_item((c + 2) % n), C_DIM, ALIGN_C, MID, 0);
    hints("UP / DOWN  \x80  BROWSE", "OK  \x80  SELECT (CONFIRM NEXT)");
    if (c > 0) snprintf(b, sizeof b, "%d / %d", c, n - 1); else snprintf(b, sizeof b, "BACK");
    gfx_text(&font_m7, CX, 224, b, C_DIM, ALIGN_C, MID, 0);
    toast(m);
}

static const char *k_menu[] = {"EMULATED SOC", "PHONE MODEL", "CHARGE TEST", "ENABLE OUTPUT", "OUTPUT OFF",
                               "PROTOCOL STATUS", "CLEAR I2C COUNTERS", "\x81 BACK"};
#define MENU_N 8
#define MENU_OFF_IDX 4

static void scr_menu(const ui_model_t *m) {
    int c = m->menu_cur;
    gfx_arc(114, 2, 135, 405, C_TRACK, true);
    header();
    gfx_text(&font_sb9, CX, 40, "MENU", C_SUB, ALIGN_C, MID, 1.4f);
    char b[32];
    for (int d = -2; d <= 2; d++) {
        int i = (c + d + MENU_N) % MENU_N;
        const char *t = k_menu[i];
        if (i == 0) { snprintf(b, sizeof b, "EMULATED SOC  %d%%", m->soc_requested); t = b; }
        float y = 118 + d * 23;
        if (d == 0) {
            gfx_round_rect(26, y - 15, 214, y + 15, 10, C_PANEL, true, C_MEAS, 1.2f);
            gfx_text(&font_sb12, CX, y, t, i == MENU_OFF_IDX ? C_ERR : C_TXT, ALIGN_C, MID, 0);
        } else {
            gfx_text(d == -1 || d == 1 ? &font_m10 : &font_m8, CX, y, t, d == -1 || d == 1 ? C_SUB : C_DIM, ALIGN_C, MID, 0);
        }
    }
    hints("UP / DOWN  \x80  OK SELECT", "HOLD OK  \x80  OUTPUT OFF");
    toast(m);
}

static void scr_soc(const ui_model_t *m) {
    gfx_arc(114, 2, 135, 405, C_TRACK, true);
    header();
    gfx_text(&font_sb9, CX, 40, "EMULATED SOC", C_SIM, ALIGN_C, MID, 1.4f);
    gfx_text(&font_m6, CX, 52, "SIMULATED VALUE \x80 NOT A REAL BATTERY", C_DIM, ALIGN_C, MID, 0.3f);
    static const char *lab[4] = {"25%", "50%", "100%", "\x81 BACK"};
    for (int i = 0; i < 3; i++) {
        float x = CX - 66 + i * 66;
        bool sel = m->soc_cur == i, cur = k_soc_presets[i] == m->soc_requested;
        gfx_round_rect(x - 29, 84, x + 29, 120, 9, sel ? C_SIM_BG : C_PANEL, true, sel ? C_SIM : C_LINE, sel ? 1.6f : 1);
        gfx_text(&font_d13, x, 102, lab[i], sel ? C_TXT : C_SUB, ALIGN_C, MID, 0);
        if (cur) gfx_text(&font_m6, x, 128, "CURRENT", C_SIM, ALIGN_C, MID, 0.3f);
    }
    bool bsel = m->soc_cur == 3;
    gfx_round_rect(CX - 34, 138, CX + 34, 156, 8, bsel ? C_PANEL : C_BG, true, bsel ? C_MEAS : C_LINE, 1);
    gfx_text(&font_m8, CX, 147, lab[3], bsel ? C_TXT : C_SUB, ALIGN_C, MID, 0);
    char b[40];
    if (m->soc_result == SOC_RES_APPLIED_SYNTHETIC) snprintf(b, sizeof b, "BENCH: %d%% APPLIED (SYNTHETIC)", m->soc_applied);
    else if (m->soc_result == SOC_RES_APPLIED_VERIFIED) snprintf(b, sizeof b, "PHONE PROFILE: %d%% APPLIED", m->soc_applied);
    else snprintf(b, sizeof b, "PHONE: NOT SENT \x80 PROFILE UNSUPPORTED");
    gfx_text(&font_m6, CX, 170, b, m->soc_result == SOC_RES_APPLIED_SYNTHETIC || m->soc_result == SOC_RES_APPLIED_VERIFIED ? C_SUB : C_WARN, ALIGN_C, MID, 0.2f);
    hints("UP / DOWN  \x80  OK SELECT", "CONFIRM IS A SEPARATE STEP");
    toast(m);
}
static void scr_protocol(const ui_model_t *m) {
    const batt_profile_t *p = profile_get(m->prof_idx);
    gfx_arc(114, 2, 135, 405, C_TRACK, true);
    header();
    gfx_text(&font_sb9, CX, 40, "PROTOCOL STATUS", C_SUB, ALIGN_C, MID, 1.4f);
    char v[7][24]; rgb_t col[7];
    const char *k[7] = {"BUS", "ADDRESS", "PROFILE", "RX / TX", "UNKNOWN RD", "IGNORED WR", "LAST REG"};
    static const char *bus[] = {"DISABLED", "IDLE", "ACTIVE", "ERROR"};
    static const rgb_t busc[] = {C_SUB, C_WARN, C_OK, C_ERR};
    snprintf(v[0], 24, "%s", bus[m->link]); col[0] = busc[m->link];
    if (!m->addr) { snprintf(v[1], 24, "REQ. CAPTURE"); col[1] = C_WARN; }
    else if (!m->addr_verified) { snprintf(v[1], 24, "0x%02X CANDIDATE", m->addr); col[1] = C_WARN; }
    else { snprintf(v[1], 24, "0x%02X", m->addr); col[1] = C_TXT; }
    snprintf(v[2], 24, "%s", p ? p->id : "-"); col[2] = C_TXT;
    snprintf(v[3], 24, "%lu / %lu", (unsigned long)m->rx, (unsigned long)m->tx); col[3] = C_TXT;
    snprintf(v[4], 24, "%lu", (unsigned long)m->unknown); col[4] = m->unknown ? C_WARN : C_TXT;
    snprintf(v[5], 24, "%lu", (unsigned long)m->ignored); col[5] = m->ignored ? C_WARN : C_TXT;
    if (m->last_reg >= 0) snprintf(v[6], 24, "0x%02X", m->last_reg); else snprintf(v[6], 24, "0x--");
    col[6] = C_SUB;
    float y = 58;
    for (int i = 0; i < 7; i++) {
        gfx_text(&font_m8, 44, y, k[i], C_DIM, ALIGN_L, MID, 0.5f);
        gfx_text(&font_sb9, 196, y, v[i], col[i], ALIGN_R, MID, 0);
        gfx_line(44, y + 9.5f, 196, y + 9.5f, 1, RGB(0x121820));
        y += 18.5f;
    }
    char f[48];
    snprintf(f,sizeof f,"REPLAY %lu/%lu  ERR %lu",(unsigned long)m->replay_frames,
             (unsigned long)m->replay_total,(unsigned long)m->replay_errors);
    gfx_text(&font_m7,CX,190,f,m->replay_errors?C_ERR:C_MEAS,ALIGN_C,MID,0.2f);
    gfx_text(&font_m6,CX,207,"CAPTURE DATA \x80 OFFLINE",C_DIM,ALIGN_C,MID,0.4f);
    toast(m);
}

static void scr_fault(const ui_model_t *m) {
    gfx_arc(114, 4, 0, 360, RGB(0x3A1214), false);
    gfx_arc(114, 4, 200, 340, C_ERR, true);
    header();
    warn_tri(CX, 62, 30, C_ERR, C_BG);
    gfx_text(&font_db17, CX, 94, fault_name(m->fault), C_ERR, ALIGN_C, MID, 0.6f);
    char b[48] = "";
    switch (m->fault) {
    case FLT_OVERVOLTAGE: snprintf(b, sizeof b, "V  %.2f V  >  LIMIT %.2f V", (double)m->fault_value, (double)m->lim_ov); break;
    case FLT_OVERCURRENT: snprintf(b, sizeof b, "I  %.2f A  OVER LIMIT", (double)m->fault_value); break;
    case FLT_UNDERVOLTAGE: snprintf(b, sizeof b, "V  %.2f V  BELOW LIMIT", (double)m->fault_value); break;
    case FLT_NO_MONITOR: snprintf(b, sizeof b, "POWER MONITOR UNAVAILABLE"); break;
    case FLT_ALERT_PIN: snprintf(b, sizeof b, "HARDWARE ALERT LINE ACTIVE"); break;
    default: snprintf(b, sizeof b, "CHECK SETUP"); break;
    }
    gfx_text(&font_m8, CX, 112, b, C_TXT, ALIGN_C, MID, 0);
    gfx_round_rect(48, 124, 192, 144, 7, C_ERR_BG, true, 0, 0);
    gfx_text(&font_sb9, CX, 134.3f, "OUTPUT DISABLED", C_ERR, ALIGN_C, MID, 0.8f);
    const char *l1 = "Reduce PSU voltage, then", *l2 = "check adapter wiring.";
    if (m->fault == FLT_OVERCURRENT) { l1 = "Check for a short on the"; l2 = "adapter or phone board."; }
    else if (m->fault == FLT_NO_MONITOR) { l1 = "Check monitor wiring"; l2 = "(I2C0 GP8/GP9)."; }
    else if (m->fault == FLT_UNSUPPORTED_MODEL) { l1 = "Profile is not verified."; l2 = "Phone output stays locked."; }
    else if (m->fault == FLT_ALERT_PIN) { l1 = "Monitor raised ALERT."; l2 = "Check limits and wiring."; }
    gfx_text(&font_m8, CX, 158, l1, C_SUB, ALIGN_C, MID, 0);
    gfx_text(&font_m8, CX, 169, l2, C_SUB, ALIGN_C, MID, 0);
    snprintf(b, sizeof b, "CODE  %s", fault_code(m->fault));
    gfx_text(&font_sb8, CX, 190, b, C_DIM, ALIGN_C, MID, 0.8f);
    gfx_text(&font_sb6, CX, 205, "OK  \x80  ACKNOWLEDGE", C_SUB, ALIGN_C, MID, 0.4f);
}

static void two_choices(int cur) {
    const char *t[2] = {"CANCEL", "APPLY"};
    for (int i = 0; i < 2; i++) {
        float x = CX - 40 + i * 80;
        bool sel = cur == i;
        rgb_t edge = sel ? (i ? C_WARN : C_MEAS) : C_LINE;
        gfx_round_rect(x - 34, 160, x + 34, 184, 9, sel ? C_PANEL : C_BG, true, edge, sel ? 1.8f : 1);
        gfx_text(&font_sb9, x, 172, t[i], sel ? C_TXT : C_DIM, ALIGN_C, MID, 0.6f);
    }
    gfx_text(&font_m6, CX, 198, "UP / DOWN CHOOSE  \x80  OK", C_SUB, ALIGN_C, MID, 0.3f);
    gfx_text(&font_m6, CX, 208, "10 s WITHOUT INPUT = CANCEL", C_DIM, ALIGN_C, MID, 0.3f);
}

static void scr_confirm(const ui_model_t *m) {
    const batt_profile_t *p = profile_get(m->prof_idx);
    rgb_t acc = m->confirm == CONF_SOC ? C_SIM : C_WARN;
    gfx_arc(114, 4, 0, 360, m->confirm == CONF_SOC ? C_SIM_BG : RGB(0x2A2110), false);
    gfx_arc(114, 4, 200, 340, acc, true);
    header();
    char b[48];
    if (m->confirm == CONF_SOC) {
        gfx_text(&font_sb9, CX, 48, "APPLY EMULATED SOC?", C_SIM, ALIGN_C, MID, 1.0f);
        snprintf(b, sizeof b, "%d%%  >  %d%%", m->confirm_from, m->confirm_arg);
        gfx_text(&font_db22, CX, 84, b, C_TXT, ALIGN_C, MID, 0.5f);
        gfx_text(&font_m6, CX, 110, "SIMULATED \x80 PSU VOLTAGE UNCHANGED", C_SUB, ALIGN_C, MID, 0.3f);
        gfx_text(&font_m6, CX, 120, "OUTPUT STATE UNCHANGED", C_SUB, ALIGN_C, MID, 0.3f);
        if (!p || !p->soc_build)
            gfx_text(&font_sb6, CX, 138, "PROFILE UNSUPPORTED \x80 PHONE NOT UPDATED", C_WARN, ALIGN_C, MID, 0.3f);
        else if (p->status != PROF_VERIFIED)
            gfx_text(&font_sb6, CX, 138, "BENCH SYNTHETIC BACKEND \x80 NOT A PHONE", C_MEAS, ALIGN_C, MID, 0.3f);
    } else if (m->confirm == CONF_MODEL) {
        const batt_profile_t *np = profile_get(m->confirm_arg);
        gfx_text(&font_sb9, CX, 48, "LOAD PROFILE?", C_WARN, ALIGN_C, MID, 1.0f);
        gfx_text(&font_sb12, CX, 84, np ? np->name : "-", C_TXT, ALIGN_C, MID, 0);
        gfx_text(&font_sb6, CX, 104, np ? profile_status_str(np->status) : "", C_WARN, ALIGN_C, MID, 0.4f);
        gfx_text(&font_m6, CX, 122, "OUTPUT STAYS OFF \x80 SOC RESETS TO 50%", C_SUB, ALIGN_C, MID, 0.3f);
    } else {
        warn_tri(CX, 58, 24, C_WARN, C_BG);
        gfx_text(&font_db17, CX, 86, "ENABLE OUTPUT?", C_WARN, ALIGN_C, MID, 0.6f);
        gfx_text(&font_sb12, CX, 106, p ? p->name : "-", C_TXT, ALIGN_C, MID, 0);
        snprintf(b, sizeof b, "LIMITS  %.2f V  \x80  %.1f A", (double)m->lim_ov, (double)m->lim_oc);
        gfx_text(&font_m7, CX, 124, b, C_SUB, ALIGN_C, MID, 0.4f);
        gfx_text(&font_sb6, CX, 138, "VERIFIED PROFILES ONLY", C_SUB, ALIGN_C, MID, 0.4f);
    }
    two_choices(m->confirm_cur);
}

static void scr_startup(const ui_model_t *m) {
    float prog = m->boot_progress / 100.0f;
    gfx_arc(114, 3, -90 + 360 * prog + 4, 266, C_TRACK, false);
    if (prog > 0.01f) gfx_arc(114, 3, -90, -90 + 360 * prog, C_MEAS, true);
    brand_mark(CX, 88, 26, C_TXT, C_BG);
    gfx_text(&font_db22, CX, 128, "CHIPLER", C_TXT, ALIGN_C, MID, 3.2f);
    gfx_text(&font_m7, CX, 146, "SMART BATTERY EMULATOR", C_SUB, ALIGN_C, MID, 1.3f);
    gfx_text(&font_m7, CX, 176, "Self-test  \x80  power monitor", C_DIM, ALIGN_C, MID, 0);
    gfx_text(&font_m7, CX, 188, m->fw ? m->fw : "", C_DIM, ALIGN_C, MID, 0);
    if (m->disp) { char b[48]; snprintf(b, sizeof b, "DISPLAY SET %s", m->disp);
                   gfx_text(&font_sb6, CX, 200, b, C_MEAS, ALIGN_C, MID, 0.4f); }
}

/* ================= CHARGE TEST mode (CP-09 concept merged, v0.3.0-dev) =================
 * Data sources: battery terminal V/I/W = the existing power monitor (signed current);
 * charger input = separate sensor, NOT FITTED -> UNAVAILABLE; protocol = target-engine counters;
 * SOC = CR-003 emulated value (SIM). No sink exists, so no test runs and the result is always
 * NOT EVALUATED. There is intentionally no pass / healthy verdict anywhere in firmware. */
#define C_CHG    RGB(0x5B9DFF)
#define C_CHG_BG RGB(0x0F1B33)
#define C_STALE  RGB(0x6B7686)

static void hints_at(float y0, const char *l1, const char *l2) {
    gfx_round_rect(52, y0, 188, y0 + 28, 8, C_PANEL, true, 0, 0);
    gfx_text(&font_m7, CX, y0 + 9, l1, C_SUB, ALIGN_C, MID, 0.4f);
    gfx_text(&font_sb6, CX, y0 + 20.5f, l2, C_MEAS, ALIGN_C, MID, 0.4f);
}
static void page_dots(float y, int cur, int n) {
    for (int i = 0; i < n; i++) {
        float x = CX + (i - (n - 1) / 2.0f) * 7;
        if (i == cur) gfx_round_rect(x - 2.6f, y - 1.6f, x + 2.6f, y + 1.6f, 1.6f, C_MEAS, true, 0, 0);
        else gfx_circle(x, y, 1.3f, C_DIM);
    }
}
static void arrow_r(float x, float y, float s, rgb_t c) { gfx_triangle(x, y - s, x + s * 1.3f, y, x, y + s, c); }
static void arrow_l(float x, float y, float s, rgb_t c) { gfx_triangle(x + s * 1.3f, y - s, x, y, x + s * 1.3f, y + s, c); }
static void flow_strip(float y, int dir, const char *lead, rgb_t col) {
    float lw = gfx_text_width(&font_sb6, lead, 0.5f);
    float ew = gfx_text_width(&font_m7, "EMU", 0.6f), pw = gfx_text_width(&font_m7, "PHONE", 0.6f);
    float gap = 22, total = lw + 8 + ew + gap + pw, x = CX - total / 2;
    gfx_text(&font_sb6, x, y, lead, col, ALIGN_L, MID, 0.5f); x += lw + 8;
    gfx_text(&font_m7, x, y, "EMU", dir ? C_SUB : C_DIM, ALIGN_L, MID, 0.6f); x += ew + 4;
    if (dir > 0) { arrow_r(x + 1, y, 3, col); arrow_r(x + 7, y, 3, col); }
    else if (dir < 0) { arrow_l(x + 1, y, 3, col); arrow_l(x + 7, y, 3, col); }
    else gfx_line(x + 2, y, x + 12, y, 1, C_DIM);
    x += gap - 4;
    gfx_text(&font_m7, x, y, "PHONE", dir ? C_SUB : C_DIM, ALIGN_L, MID, 0.6f);
}
static void check_icon(float x, float y, int st) {
    if (st == CHK_PASS) { gfx_circle(x, y, 5, C_OK_BG); gfx_line(x - 2.6f, y, x - 0.6f, y + 2.2f, 1.4f, C_OK); gfx_line(x - 0.6f, y + 2.2f, x + 2.8f, y - 2.4f, 1.4f, C_OK); }
    else if (st == CHK_FAIL) { gfx_circle(x, y, 5, C_ERR_BG); gfx_line(x - 2.2f, y - 2.2f, x + 2.2f, y + 2.2f, 1.4f, C_ERR); gfx_line(x - 2.2f, y + 2.2f, x + 2.2f, y - 2.2f, 1.4f, C_ERR); }
    else { gfx_circle(x, y, 5, C_WARN_BG); gfx_text(&font_sb6, x, y + 0.3f, "?", C_WARN, ALIGN_C, MID, 0); }
}
static void chg_title(const char *t, rgb_t c, const char *sub) {
    gfx_arc(114, 2, 135, 405, C_TRACK, true);
    header();
    gfx_text(&font_sb9, CX, 39, t, c, ALIGN_C, MID, 1.4f);
    if (sub) gfx_text(&font_m6, CX, 50, sub, C_DIM, ALIGN_C, MID, 0.4f);
}
static void chg_footer(const ui_model_t *m, float y_hint, const char *l1) {
    hints_at(y_hint, l1, "OK  \x80  CHARGE MENU");
    if (m->tq == TQ_SIMULATED) gfx_text(&font_m6, CX, y_hint + 38, "SIMULATED VALUES \x80 NOT MEASURED", C_WARN, ALIGN_C, MID, 0.4f);
    page_dots(y_hint + 48, m->chg_page, CHG_PAGES);
}
typedef struct { const char *k; char v[28]; rgb_t c; } chg_row_t;
static void chg_rows(const chg_row_t *r, int n, float y, float step) {
    for (int i = 0; i < n; i++) {
        gfx_text(&font_m8, 42, y, r[i].k, C_DIM, ALIGN_L, MID, 0.5f);
        gfx_text(&font_sb9, 198, y, r[i].v, r[i].c, ALIGN_R, MID, 0);
        gfx_line(42, y + step / 2, 198, y + step / 2, 1, RGB(0x121820));
        y += step;
    }
}

/* values used by the charge pages: live when fresh, last-good when stale */
static void chg_vals(const ui_model_t *m, float *v, float *a, float *w) {
    if (m->chg_flow == CHF_STALE) { *v = m->lg_v; *a = m->lg_a; *w = m->lg_w; }
    else { *v = m->v; *a = m->a; *w = m->w; }
}

static void scr_chg_overview(const ui_model_t *m) {
    chg_flow_t f = m->chg_flow;
    bool valid = f != CHF_UNAVAILABLE, stale = f == CHF_STALE, sim = m->tq == TQ_SIMULATED;
    float v, a, w; chg_vals(m, &v, &a, &w);
    int dir = f == CHF_SUPPLYING ? 1 : f == CHF_RECEIVING ? -1 : 0;
    if (stale) dir = a >= CHG_I_THRESH_A ? 1 : a <= -CHG_I_THRESH_A ? -1 : 0;
    gfx_arc(114, 6, 135, 405, C_TRACK, true);
    if (valid) {
        float fr = fabsf(a) / 3.0f; if (fr > 1) fr = 1;
        rgb_t c = stale ? C_STALE : (dir > 0 ? C_AMP : C_CHG);
        if (fr > 0.004f && dir > 0) gfx_arc(114, 6, 270 - 135 * fr, 270, c, true);
        if (fr > 0.004f && dir < 0) gfx_arc(114, 6, 270, 270 + 135 * fr, c, true);
    }
    for (int k = 0; k <= 12; k++) {
        float an = (135 + 270 * k / 12.0f) * 0.0174533f;
        float r0 = k == 6 ? 101.0f : ((k % 2) ? 103.5f : 102.0f);
        gfx_line(CX + r0 * cosf(an), 120 + r0 * sinf(an), CX + 106.5f * cosf(an), 120 + 106.5f * sinf(an),
                 k == 6 ? 1.6f : 1, k == 6 ? C_SUB : ((k % 2) ? C_LINE : C_DIM));
    }
    header();
    gfx_text(&font_sb9, CX, 39, "CHARGE TEST", C_TXT, ALIGN_C, MID, 1.4f);
    char b[32];
    {   const char *st; rgb_t fg, bg;
        switch (f) {
        case CHF_SUPPLYING: st = "SUPPLYING PHONE"; fg = C_AMP; bg = C_WARN_BG; break;
        case CHF_RECEIVING: st = "RECEIVING CURRENT"; fg = C_CHG; bg = C_CHG_BG; break;
        case CHF_NO_NET:    st = "NO NET CURRENT"; fg = C_SUB; bg = C_PANEL; break;
        case CHF_STALE:     snprintf(b, sizeof b, "DATA STALE  %.1f s", (double)(m->t_age_ms / 1000.0f)); st = b; fg = C_WARN; bg = C_WARN_BG; break;
        default:            st = m->chg_input_fitted ? "NO CHARGER" : "CHARGER UNKNOWN"; fg = C_SUB; bg = C_PANEL; break;
        }
        pill(CX, 47, 60, st, &font_sb8, fg, bg, true, 0.6f);
    }
    gfx_text(&font_m7, CX, 71, "BATTERY TERMINAL", C_DIM, ALIGN_C, MID, 1.2f);
    if (valid) snprintf(b, sizeof b, "%.3f", (double)v); else snprintf(b, sizeof b, "-.---");
    float vw = gfx_text_width(&font_d40, b, 0), uw = gfx_text_width(&font_m16, "V", 0), x0 = CX - (vw + 3 + uw) / 2;
    gfx_text(&font_d40, x0, 95, b, valid && !stale ? C_TXT : C_DIM, ALIGN_L, MID, 0);
    gfx_text(&font_m16, x0 + vw + 3, 101, "V", stale || !valid ? C_DIM : C_MEAS, ALIGN_L, BASE, 0);
    gfx_line(CX - 52, 117.5f, CX + 52, 117.5f, 1, C_LINE);
    if (valid) snprintf(b, sizeof b, "%.3f", (double)fabsf(a)); else snprintf(b, sizeof b, "-.---");
    rgb_t ic = !valid ? C_DIM : stale ? C_STALE : dir > 0 ? C_AMP : dir < 0 ? C_CHG : C_SUB;
    float aw = gfx_text_width(&font_d30, b, 0), auw = gfx_text_width(&font_m14, "A", 0);
    x0 = CX - (aw + 3 + auw) / 2;
    gfx_text(&font_d30, x0, 135, b, ic, ALIGN_L, MID, 0);
    gfx_text(&font_m14, x0 + aw + 3, 140, "A", ic, ALIGN_L, BASE, 0);
    if (!valid) gfx_text(&font_sb6, CX, 153, "MEASUREMENT UNAVAILABLE", C_ERR, ALIGN_C, MID, 0.6f);
    else flow_strip(153, stale ? 0 : dir, stale ? "STALE" : dir > 0 ? "SUPPLYING" : dir < 0 ? "CHARGE IN" : "NO NET",
                    stale ? C_WARN : ic);
    gfx_round_rect(CX - 66, 163, CX - 3, 187, 6, C_PANEL, true, C_LINE, 1);
    gfx_round_rect(CX + 3, 163, CX + 66, 187, 6, C_SIM_BG, true, C_SIM_LN, 1);
    if (valid) snprintf(b, sizeof b, "%.2f", (double)fabsf(w)); else snprintf(b, sizeof b, "-.--");
    gfx_text(&font_d13, CX - 34.5f, 172.5f, b, valid && !stale ? C_TXT : C_DIM, ALIGN_C, MID, 0);
    gfx_text(&font_m6, CX - 34.5f, 182.5f, stale ? "LAST  W" : dir > 0 ? "SUPPLIED  W" : dir < 0 ? "RECEIVED  W" : "POWER  W",
             !valid ? C_DIM : dir < 0 ? C_CHG : dir > 0 ? C_AMP : C_SUB, ALIGN_C, MID, 0.4f);
    {   bool sent = m->soc_applied >= 0 && m->soc_applied == m->soc_requested;
        rgb_t vc = sent ? C_SIM : RGB(0x8E7FD0);
        battery_icon(CX + 9, 168.5f, 12, 7, m->soc_requested / 100.0f, sent ? C_SIM : RGB(0x6E6390));
        snprintf(b, sizeof b, "%d%%", m->soc_requested);
        gfx_text(&font_d13, CX + 25, 172.5f, b, vc, ALIGN_L, MID, 0);
        gfx_text(&font_m6, CX + 34.5f, 182.5f, sent ? "SIM  SOC" : "SIM \x80 NOT SENT", sent ? C_SIM : C_WARN, ALIGN_C, MID, 0.3f);
    }
    {   const char *o = m->out == OUT_ON ? "OUTPUT ON" : m->out == OUT_ARMED ? "ARMED" : "OUTPUT OFF";
        snprintf(b, sizeof b, "%s \x80 %s", o, m->chg_sink_fitted ? "SINK OFF" : "NO SINK");
        pill(CX, 193, 205, b, &font_sb6, m->out == OUT_ON ? C_OK : C_SUB, m->out == OUT_ON ? C_OK_BG : C_PANEL, false, 0.6f);
    }
    if (m->soc_requested == 100 && f == CHF_NO_NET)
        gfx_text(&font_m6, CX, 213.5f, "100% MAY STOP CHARGING \x80 SEE SOC", C_WARN, ALIGN_C, MID, 0.4f);
    else
        gfx_text(&font_m6, CX, 213.5f, stale ? "RESULT \x80 NOT EVALUATED \x80 STALE" : "RESULT \x80 NOT EVALUATED", stale ? C_WARN : C_SUB, ALIGN_C, MID, 0.4f);
    if (sim) gfx_text(&font_m6, CX, 222, "SIMULATED \x80 NOT MEASURED", C_WARN, ALIGN_C, MID, 0.5f);
    page_dots(230, 0, CHG_PAGES);
    toast(m);
}

static void scr_chg_input(const ui_model_t *m) {
    chg_title("CHARGER INPUT", C_SUB, "CHARGER SIDE \x80 SEPARATE SENSOR");
    chg_row_t r[5] = {{"VOLTAGE", "UNAVAILABLE", C_DIM}, {"CURRENT", "UNAVAILABLE", C_DIM}, {"POWER", "UNAVAILABLE", C_DIM},
                      {"SOURCE", "NOT FITTED", C_WARN}, {"UT658A / PW11", "NOT LINKED", C_WARN}};
    chg_rows(r, 5, 66, 18);
    gfx_text(&font_sb6, CX, 162, "INPUT AMPS ARE NOT BATTERY CHARGE AMPS", C_SUB, ALIGN_C, MID, 0.3f);
    chg_footer(m, 172, "UP / DOWN  \x80  PAGE");
    toast(m);
}

static void scr_chg_battery(const ui_model_t *m) {
    chg_title("BATTERY TERMINAL", C_SUB, "BATTERY CONNECTOR \x80 SIGNED");
    chg_flow_t f = m->chg_flow;
    float v, a, w; chg_vals(m, &v, &a, &w);
    bool valid = f != CHF_UNAVAILABLE, stale = f == CHF_STALE;
    rgb_t dc = !valid ? C_DIM : stale ? C_STALE : a > 0 ? C_AMP : C_CHG;
    chg_row_t r[6] = {{"VOLTAGE", "", C_TXT}, {"CURRENT", "", dc}, {"DIRECTION", "", dc},
                      {"POWER", "", dc}, {"SAMPLE AGE", "", C_SUB}, {"SOURCE", "", C_SUB}};
    if (valid) {
        snprintf(r[0].v, 28, "%.3f V", (double)v); if (stale) r[0].c = C_STALE;
        snprintf(r[1].v, 28, "%.3f A", (double)fabsf(a));
        snprintf(r[2].v, 28, "%s", fabsf(a) < CHG_I_THRESH_A ? "NO NET" : a > 0 ? "EMU > PHONE" : "PHONE > EMU");
        snprintf(r[3].v, 28, "%.2f W %s", (double)fabsf(w), a > 0 ? "SUPPLIED" : "RECEIVED");
    } else {
        for (int i = 0; i < 4; i++) { snprintf(r[i].v, 28, "UNAVAILABLE"); r[i].c = C_DIM; }
    }
    if (!m->t_have) snprintf(r[4].v, 28, "NO SAMPLE");
    else if (m->t_age_ms > 60000u) snprintf(r[4].v, 28, "> 60 s");
    else snprintf(r[4].v, 28, "%lu ms", (unsigned long)m->t_age_ms);
    if (stale) r[4].c = C_WARN;
    snprintf(r[5].v, 28, "%s", m->tq == TQ_SIMULATED ? "SIMULATED" : "POWER MONITOR");
    if (m->tq == TQ_SIMULATED) r[5].c = C_WARN;
    chg_rows(r, 6, 64, 17);
    chg_footer(m, 172, "UP / DOWN  \x80  PAGE");
    toast(m);
}

static void scr_chg_proto(const ui_model_t *m) {
    chg_title("BATTERY PROTOCOL", C_SUB, "TARGET-SIDE COUNTERS");
    static const char *bus[] = {"DISABLED", "IDLE", "ACTIVE", "ERROR"};
    static const rgb_t busc[] = {C_SUB, C_WARN, C_OK, C_ERR};
    chg_row_t r[7] = {{"BUS ACTIVITY", "", busc[m->link]}, {"TRANSACTIONS", "", C_TXT}, {"RX / TX BYTES", "", C_TXT},
                      {"UNKNOWN RD", "", C_TXT}, {"IGNORED WR", "", C_TXT}, {"OVERFLOWS", "", C_TXT},
                      {"VALIDATED", "0 \x80 NO REFERENCE", C_WARN}};
    snprintf(r[0].v, 28, "%s", bus[m->link]);
    snprintf(r[1].v, 28, "%lu", (unsigned long)m->txns);
    snprintf(r[2].v, 28, "%lu / %lu", (unsigned long)m->rx, (unsigned long)m->tx);
    snprintf(r[3].v, 28, "%lu", (unsigned long)m->unknown); if (m->unknown) r[3].c = C_WARN;
    snprintf(r[4].v, 28, "%lu", (unsigned long)m->ignored); if (m->ignored) r[4].c = C_WARN;
    snprintf(r[5].v, 28, "%lu", (unsigned long)m->overflows); if (m->overflows) r[5].c = C_ERR;
    chg_rows(r, 7, 62, 14.5f);
    gfx_text(&font_sb6, CX, 164, "TRAFFIC / ACK IS NOT A CHARGING PASS", C_WARN, ALIGN_C, MID, 0.3f);
    chg_footer(m, 172, "UP / DOWN  \x80  PAGE");
    toast(m);
}

static void scr_chg_soc(const ui_model_t *m) {
    chg_title("EMULATED SOC", C_SIM, "SIMULATED VALUE \x80 NOT A REAL BATTERY");
    static const char *lab[3] = {"25%", "50%", "100%"};
    for (int i = 0; i < 3; i++) {
        float x = CX - 64 + i * 64;
        bool cur = k_soc_presets[i] == m->soc_requested;
        gfx_round_rect(x - 28, 62, x + 28, 92, 9, cur ? C_SIM_BG : C_PANEL, true, cur ? C_SIM : C_LINE, cur ? 1.6f : 1);
        gfx_text(&font_d13, x, 77, lab[i], cur ? C_TXT : C_SUB, ALIGN_C, MID, 0);
        if (cur) gfx_text(&font_m6, x, 99, "CURRENT", C_SIM, ALIGN_C, MID, 0.3f);
    }
    gfx_text(&font_sb6, CX, 113, "100% MAY REDUCE OR STOP CHARGING", C_WARN, ALIGN_C, MID, 0.3f);
    gfx_text(&font_m6, CX, 124, "LOW CURRENT AT 100% IS NOT A FAULT", C_SUB, ALIGN_C, MID, 0.3f);
    gfx_text(&font_m6, CX, 136, "SOC \x80 VOLTAGE \x80 CAPACITY \x80 TEMP", C_SUB, ALIGN_C, MID, 0.3f);
    gfx_text(&font_m6, CX, 145, "MUST MATCH A VERIFIED TEST PROFILE", C_SUB, ALIGN_C, MID, 0.3f);
    bool sent = m->soc_applied >= 0 && m->soc_applied == m->soc_requested;
    gfx_text(&font_sb6, CX, 160, sent ? (m->soc_result == SOC_RES_APPLIED_SYNTHETIC ? "BENCH SYNTHETIC BACKEND \x80 NOT A PHONE" : "APPLIED BY VERIFIED PROFILE")
                                      : "PHONE: NOT SENT \x80 PROFILE UNSUPPORTED", sent ? C_SUB : C_WARN, ALIGN_C, MID, 0.3f);
    chg_footer(m, 172, "CHANGE: OK \x80 EMULATED SOC");
    toast(m);
}

static void scr_chg_result(const ui_model_t *m) {
    chg_title("TEST RESULT", C_SUB, NULL);
    gfx_round_rect(30, 52, 210, 86, 10, C_PANEL, true, C_SUB, 1.2f);
    gfx_text(&font_db17, CX, 69.5f, "NOT EVALUATED", C_SUB, ALIGN_C, MID, 0.5f);
    gfx_text(&font_sb6, CX, 96, "TEST NOT RUN", C_SUB, ALIGN_C, MID, 0.4f);
    static const char *l[] = {"NEEDS: SINK + TEST RECIPE + LIMITS", "BASELINE FOR THIS EXACT MODEL",
                              "RECORDED CHARGER / CABLE / TEMP", "AMPS OR BUS TRAFFIC ALONE = NO VERDICT"};
    for (int i = 0; i < 4; i++) gfx_text(&font_m7, CX, 112 + i * 12, l[i], i == 3 ? C_WARN : C_SUB, ALIGN_C, MID, 0.2f);
    chg_footer(m, 172, "UP / DOWN  \x80  PAGE");
    toast(m);
}

static const char *k_chg_menu[] = {"START CHARGE TEST", "STOP TEST", "EMULATED SOC", "VIEW RESULT", "OUTPUT OFF",
                                   "EXIT TO MENU", "\x81 BACK"};
#define CHG_MENU_N 7
static void scr_chg_menu(const ui_model_t *m) {
    int c = m->chg_menu_cur;
    gfx_arc(114, 2, 135, 405, C_TRACK, true);
    header();
    gfx_text(&font_sb9, CX, 40, "CHARGE MENU", C_SUB, ALIGN_C, MID, 1.4f);
    char b[32];
    for (int d = -2; d <= 2; d++) {
        int i = (c + d + CHG_MENU_N) % CHG_MENU_N;
        const char *t = k_chg_menu[i];
        if (i == 2) { snprintf(b, sizeof b, "EMULATED SOC  %d%%", m->soc_requested); t = b; }
        float y = 118 + d * 23;
        if (d == 0) {
            gfx_round_rect(26, y - 15, 214, y + 15, 10, C_PANEL, true, C_MEAS, 1.2f);
            gfx_text(&font_sb12, CX, y, t, i == 4 ? C_ERR : C_TXT, ALIGN_C, MID, 0);
        } else {
            gfx_text(d == -1 || d == 1 ? &font_m10 : &font_m8, CX, y, t, i == 4 ? RGB(0x8A3B3D) : (d == -1 || d == 1 ? C_SUB : C_DIM), ALIGN_C, MID, 0);
        }
    }
    hints("UP / DOWN  \x80  OK SELECT", "HOLD OK  \x80  OUTPUT OFF");
    toast(m);
}

static void scr_chg_confirm(const ui_model_t *m) {
    bool blocked = !m->chg_start_ok;
    gfx_arc(114, 4, 0, 360, RGB(0x2A2110), false);
    gfx_arc(114, 4, 200, 340, C_WARN, true);
    header();
    gfx_text(&font_sb9, CX, 40, "START CHARGE TEST?", C_WARN, ALIGN_C, MID, 1.0f);
    float y = 58;
    for (int i = 0; i < CHK_N; i++) {
        int st = m->chg_checks.c[i];
        check_icon(52, y, st);
        gfx_text(&font_m8, 62, y, chk_name(i), st == CHK_PASS ? C_TXT : C_SUB, ALIGN_L, MID, 0.4f);
        y += 16;
    }
    if (blocked) gfx_text(&font_sb6, CX, 143, "START BLOCKED \x80 FIX FAILED CHECKS", C_ERR, ALIGN_C, MID, 0.3f);
    else gfx_text(&font_m6, CX, 143, "SINK ENABLES ONLY AFTER START", C_SUB, ALIGN_C, MID, 0.3f);
    const char *t[2] = {"CANCEL", blocked ? "BLOCKED" : "START"};
    for (int i = 0; i < 2; i++) {
        float x = CX - 40 + i * 80;
        bool sel = m->confirm_cur == i, dis = blocked && i == 1;
        rgb_t edge = dis ? C_LINE : sel ? (i ? C_WARN : C_MEAS) : C_LINE;
        gfx_round_rect(x - 34, 154, x + 34, 178, 9, sel ? C_PANEL : C_BG, true, edge, sel ? 1.8f : 1);
        gfx_text(&font_sb9, x, 166, t[i], dis ? RGB(0x3A4350) : sel ? C_TXT : C_DIM, ALIGN_C, MID, 0.6f);
    }
    gfx_text(&font_m6, CX, 190, "UP / DOWN CHOOSE  \x80  OK", C_SUB, ALIGN_C, MID, 0.3f);
    gfx_text(&font_m6, CX, 199, "10 s WITHOUT INPUT = CANCEL", C_DIM, ALIGN_C, MID, 0.3f);
    toast(m);
}

static void scr_chg(const ui_model_t *m) {
    switch (m->chg_page) {
    case 1: scr_chg_input(m); break;
    case 2: scr_chg_battery(m); break;
    case 3: scr_chg_proto(m); break;
    case 4: scr_chg_soc(m); break;
    case 5: scr_chg_result(m); break;
    default: scr_chg_overview(m); break;
    }
}

void ui_render(const ui_model_t *m) {
    gfx_clear(C_BG);
    switch (m->screen) {
    case SCR_STARTUP: scr_startup(m); break;
    case SCR_DASH:    scr_dashboard(m); break;
    case SCR_PROTO:   scr_protocol(m); break;
    case SCR_MODELS:  scr_models(m); break;
    case SCR_MENU:    scr_menu(m); break;
    case SCR_SOC:     scr_soc(m); break;
    case SCR_CONFIRM: if (m->confirm == CONF_CHG_START) scr_chg_confirm(m); else scr_confirm(m); break;
    case SCR_FAULT:   scr_fault(m); break;
    case SCR_CHG:     scr_chg(m); break;
    case SCR_CHG_MENU: scr_chg_menu(m); break;
    }
    gfx_mask_round();
}
