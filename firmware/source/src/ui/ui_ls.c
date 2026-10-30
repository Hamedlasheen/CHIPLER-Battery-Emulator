#include "ui_ls.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../gfx/gfx.h"
#include "../gfx/fonts_data.h"
#include "../batt/profile.h"
#include "../batt/soc_ctrl.h"
#include "../safety/safety.h"
#include "ui_nav.h"
#if CHIPLER_AUTO_LEARN
#include "../auto/auto_core.h"
#endif
#include "../charge/charge_diag.h"
#define C_CHG RGB(0x5B9DFF)
#define C_CHG_BG RGB(0x0F1B33)
#define C_STALE RGB(0x6B7686)

#define C_BG     RGB(0x06080B)
#define C_PANEL  RGB(0x0E131A)
#define C_PANEL2 RGB(0x151C26)
#define C_LINE   RGB(0x1B232E)
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

/* ------------------------------------------------------------------ layout */
static void add(ls_layout_t *l, int x0, int y0, int x1, int y1, ls_cmd_t cmd, int idx, const char *lab,
                ls_style_t st, bool hl) {
    if (l->n >= LS_MAX_REGIONS) return;
    ls_region_t *r = &l->r[l->n++];
    r->x0 = (int16_t)x0; r->y0 = (int16_t)y0; r->x1 = (int16_t)x1; r->y1 = (int16_t)y1;
    r->cmd = cmd; r->idx = (int16_t)idx; r->label = lab; r->style = (uint8_t)st; r->highlight = hl;
}

static void footer(ls_layout_t *l, bool back) {
    if (back) add(l, 6, 204, 106, 236, LS_CMD_BACK, 0, "\x81 BACK", LS_ST_NORMAL, false);
    add(l, 214, 204, 314, 236, LS_CMD_STOP, 0, "STOP REPLIES", LS_ST_DANGER, false);
}

static const struct { int mi; const char *label; } k_tiles[] = {
#if CHIPLER_AUTO_LEARN
    /* AUTO_LEARN: no phone-model tile. AUTO ON / RELEARN / CLEAR SAVED drive the AUTO controller; counters are cleared on PROTOCOL. */
    {MI_SOC, "EMULATED SOC"}, {MI_MODEL, "AUTO ON"}, {MI_ENABLE, "RELEARN"},
    {MI_PROTO, "PROTOCOL"}, {MI_BATT, "BATTERY DATA"}, {MI_CLEAR, "CLEAR SAVED"},
#else
    {MI_SOC, "EMULATED SOC"}, {MI_MODEL, "PHONE MODEL"}, {MI_ENABLE, "ENABLE REPLIES"},
    {MI_PROTO, "PROTOCOL"}, {MI_BATT, "BATTERY DATA"}, {MI_CLEAR, "CLEAR COUNTERS"},
#endif
    {MI_SETTINGS, "SETTINGS"},      /* legacy MI_OFF ("OUTPUT OFF") is the persistent STOP REPLIES button in every footer */
};
#define N_TILES ((int)(sizeof k_tiles / sizeof k_tiles[0]))
#define LIST_ROWS 4

static int models_window_start(const ui_model_t *m) {
    int cur = m->model_cur > 0 ? m->model_cur - 1 : 0;
    return (cur / LIST_ROWS) * LIST_ROWS;
}

void ui_ls_layout(const ui_model_t *m, ls_layout_t *l) {
    l->n = 0;
    switch (m->screen) {
    case SCR_STARTUP:
        break;
    case SCR_TOUCHDIAG:
        footer(l, true);
        break;
    case SCR_CAL:
        /* escape hatch that needs a working calibration to hit-test; without one the 45 s timeout leaves */
        if (m->touch_cal_valid) add(l, 110, 176, 210, 208, LS_CMD_BACK, 0, "CANCEL", LS_ST_NORMAL, false);   /* clear of all five targets */
        break;
    case SCR_DASH:
        add(l, 214, 32, 314, 80, LS_CMD_SELECT, 0, "MENU", LS_ST_PRIMARY, false);
        add(l, 214, 88, 314, 136, LS_CMD_SELECT, 1, "PROTOCOL", LS_ST_NORMAL, false);
        footer(l, false);
        break;
    case SCR_MENU:
        {   int n = 0;
            for (int i = 0; i < N_TILES + 1; i++) {
                const char *lab; int mi;
                if (i == 2 && m->chg_ui) { lab = "CHARGE TEST"; mi = MI_CHARGE; }
                else { int k = (i > 2 && m->chg_ui) ? i - 1 : i; if (k >= N_TILES) break; lab = k_tiles[k].label; mi = k_tiles[k].mi; }
                int col = n % 3, row = n / 3;
                int x0 = 6 + col * 104, y0 = 30 + row * 56;
                add(l, x0, y0, x0 + 100, y0 + 50, LS_CMD_SELECT, mi, lab, LS_ST_NORMAL, mi == m->menu_cur);
                n++;
            }
        }
        footer(l, true);
        break;
    case SCR_CHG:
        add(l, 6, 168, 96, 200, LS_CMD_UP, 0, "PREV", LS_ST_NORMAL, false);
        add(l, 110, 168, 210, 200, LS_CMD_SELECT, 0, "CHARGE MENU", LS_ST_PRIMARY, false);
        add(l, 224, 168, 314, 200, LS_CMD_DOWN, 0, "NEXT", LS_ST_NORMAL, false);
        footer(l, true);
        break;
    case SCR_CHG_MENU: {
        static const char *cl[CMI_COUNT] = {"START TEST", "STOP TEST", "EMULATED SOC", "VIEW RESULT", "STOP REPLIES", "EXIT TO MENU", "BACK TO PAGES"};
        for (int i = 0; i < CMI_COUNT; i++) {
            int col = i % 2, row = i / 2, x0 = 6 + col * 158, y0 = 30 + row * 44;
            add(l, x0, y0, x0 + 150, y0 + 38, LS_CMD_SELECT, i, cl[i], i == CMI_OFF ? LS_ST_DANGER : (i == CMI_START ? LS_ST_PRIMARY : LS_ST_NORMAL), i == m->chg_menu_cur);
        }
        footer(l, true);
        break; }
    case SCR_SOC:
#if CHIPLER_OWNER_PHONE_TEST
        if (m->soc_capable) {   /* OWNER r7 picker; indices match nav_select(): 0..3 steps, 4 APPLY, 5..7 presets */
            static const char *st[4] = {"-10", "-1", "+1", "+10"};
            static const char *pr[3] = {"25%", "50%", "100%"};
            for (int i = 0; i < 4; i++) add(l, 8 + i * 78, 96, 8 + i * 78 + 72, 136, LS_CMD_SELECT, i, st[i], LS_ST_NORMAL, false);
            for (int i = 0; i < 3; i++) add(l, 8 + i * 78, 144, 8 + i * 78 + 72, 184, LS_CMD_SELECT, OWNER_SOC_PRESET0 + i, pr[i], LS_ST_NORMAL, false);
            add(l, 242, 144, 314, 184, LS_CMD_SELECT, OWNER_SOC_APPLY, "APPLY", LS_ST_PRIMARY, false);
        }
        footer(l, true);
        break;
#endif
        if (m->soc_capable) {
            static const char *lab[SOC_N_PRESETS] = {"25%", "50%", "100%"};
            for (int i = 0; i < SOC_N_PRESETS; i++)
                add(l, 8 + i * 104, 84, 8 + i * 104 + 96, 144, LS_CMD_SELECT, i, lab[i], LS_ST_NORMAL,
                    k_soc_presets[i] == m->soc_requested);
        }
        footer(l, true);
        break;
    case SCR_MODELS: {
        int ws = models_window_start(m), n = profile_count();
        for (int k = 0; k < LIST_ROWS && ws + k < n; k++) {
            const batt_profile_t *p = profile_get(ws + k);
            add(l, 6, 30 + k * 42, 250, 30 + k * 42 + 38, LS_CMD_SELECT, ws + k + 1, p->name, LS_ST_NORMAL,
                ws + k == m->prof_idx);
            l->r[l->n - 1].style = LS_ST_LISTROW;
        }
        add(l, 258, 30, 314, 100, LS_CMD_UP, 0, "PREV", LS_ST_NORMAL, false);
        add(l, 258, 106, 314, 176, LS_CMD_DOWN, 0, "NEXT", LS_ST_NORMAL, false);
        footer(l, true);
        break;
    }
    case SCR_CONFIRM:
        if (m->confirm == CONF_CHG_START) {
            add(l, 10, 156, 154, 198, LS_CMD_SELECT, 0, "CANCEL", LS_ST_PRIMARY, m->confirm_cur == 0);
            add(l, 166, 156, 310, 198, LS_CMD_SELECT, 1, m->chg_start_ok ? "START" : "BLOCKED", m->chg_start_ok ? LS_ST_NORMAL : LS_ST_DANGER, m->confirm_cur == 1);
        } else {
            add(l, 10, 142, 154, 196, LS_CMD_SELECT, 0, "CANCEL", LS_ST_PRIMARY, m->confirm_cur == 0);
            add(l, 166, 142, 310, 196, LS_CMD_SELECT, 1, "APPLY", LS_ST_NORMAL, m->confirm_cur == 1);
        }
        add(l, 214, 204, 314, 236, LS_CMD_STOP, 0, "STOP REPLIES", LS_ST_DANGER, false);
        break;
    case SCR_FAULT:
        add(l, 10, 150, 154, 196, LS_CMD_SELECT, 0, "ACKNOWLEDGE", LS_ST_PRIMARY, true);
        add(l, 214, 204, 314, 236, LS_CMD_STOP, 0, "STOP REPLIES", LS_ST_DANGER, false);
        break;
    case SCR_PROTO:
        add(l, 214, 150, 314, 196, LS_CMD_SELECT, 0, "CLEAR COUNTERS", LS_ST_NORMAL, false);
        footer(l, true);
        break;
    case SCR_SETTINGS:
        add(l, 10, 32, 154, 80, LS_CMD_SELECT, 0, "ROTATE 180", LS_ST_NORMAL, false);
        add(l, 166, 32, 310, 80, LS_CMD_SELECT, 1, "CALIBRATE TOUCH", LS_ST_PRIMARY, !m->touch_cal_valid);
        add(l, 10, 88, 154, 136, LS_CMD_SELECT, 2, "TOUCH DIAGNOSTIC", LS_ST_NORMAL, false);
        footer(l, true);
        break;
    case SCR_BATT:
        footer(l, true);
        break;
    default:
        footer(l, true);
        break;
    }
}

bool ui_ls_hit(const ls_layout_t *l, int x, int y, ls_region_t *out) {
    /* last region wins on (disallowed) overlap; the host test proves there is none */
    for (int i = l->n - 1; i >= 0; i--) {
        const ls_region_t *r = &l->r[i];
        if (x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1) { if (out) *out = *r; return true; }
    }
    return false;
}

/* ------------------------------------------------------------------ drawing helpers */
static const font_t *pick_font(const char *t, float maxw) {
    /* one label size per button class: wide buttons (>= 130 px) use the large font when it fits */
    if (maxw >= 118 && gfx_text_width(&font_sb12, t, 0) <= maxw) return &font_sb12;
    if (gfx_text_width(&font_sb9, t, 0) <= maxw) return &font_sb9;
    return &font_m8;
}

bool ui_ls_label_fits(const ls_region_t *r) {
    if (!r->label || !r->label[0]) return true;
    float w = (float)(r->x1 - r->x0) - 12;
    return gfx_text_width(pick_font(r->label, w), r->label, 0) <= w;
}

static void draw_region(const ls_region_t *r) {
    rgb_t fill = C_PANEL, line = C_LINE, txt = C_TXT;
    float ow = 1.0f;
    switch (r->style) {
    case LS_ST_PRIMARY: line = C_MEAS; ow = 1.4f; break;
    case LS_ST_DANGER:  fill = C_ERR_BG; line = C_ERR; txt = C_ERR; ow = 1.2f; break;
    case LS_ST_SAFE:    fill = C_OK_BG; line = C_OK; txt = C_OK; break;
    default: break;
    }
    if (r->highlight) { line = C_MEAS; ow = 2.0f; fill = C_PANEL2; }
    gfx_round_rect(r->x0, r->y0, r->x1, r->y1, 8, fill, true, line, ow);
    if (r->label && r->label[0]) {
        float w = (float)(r->x1 - r->x0) - 12;
        const font_t *f = pick_font(r->label, w);
        if (r->style == LS_ST_LISTROW)
            gfx_text(&font_sb9, r->x0 + 12, (r->y0 + r->y1) / 2.0f + 0.5f, r->label, txt, ALIGN_L, MID, 0);
        else
            gfx_text(f, (r->x0 + r->x1) / 2.0f, (r->y0 + r->y1) / 2.0f + 0.5f, r->label, txt, ALIGN_C, MID, 0);
    }
}

static void header(const ui_model_t *m, const char *title) {
    gfx_fill_rect(0, 0, LS_W, 26, C_PANEL);
    gfx_fill_rect(0, 26, LS_W, 27, C_LINE);
    gfx_text(&font_sb9, 8, 13.5f, "CHIPLER", C_SUB, ALIGN_L, MID, 1.4f);
    if (title) gfx_text(&font_sb9, 100, 13.5f, title, C_TXT, ALIGN_L, MID, 1.0f);
    /* Owner request 100: no sensor label in the shared header. The sensor state stays visible where it is data:
     * DASHBOARD quality line (MEASURED / UNCAL / NO LOCAL MEASUREMENT) and BATTERY DATA (calibration line). */
    (void)m;
}

static void toast(const ui_model_t *m) {
    if (!m->toast[0] || (int32_t)(m->toast_until - m->now_ms) <= 0) return;
    float tw = gfx_text_width(&font_sb9, m->toast, 0.5f);
    gfx_round_rect(LS_W / 2.0f - tw / 2 - 12, 176, LS_W / 2.0f + tw / 2 + 12, 198, 10, C_WARN_BG, true, C_WARN, 1);
    gfx_text(&font_sb9, LS_W / 2.0f, 187.3f, m->toast, C_WARN, ALIGN_C, MID, 0.5f);
}

static void fmt_num(char *b, size_t n, float v, int dec, bool valid) {
    if (!valid) { snprintf(b, n, dec == 3 ? "-.---" : "-.--"); return; }
    snprintf(b, n, "%.*f", dec, (double)v);
}

static void pill(float x0, float y0, float x1, float y1, const char *t, rgb_t fg, rgb_t bg) {
    gfx_round_rect(x0, y0, x1, y1, (y1 - y0) / 2, bg, true, 0, 0);
    gfx_text(&font_sb8, (x0 + x1) / 2, (y0 + y1) / 2 + 0.3f, t, fg, ALIGN_C, MID, 0.4f);
}

static void line_kv(float y, const char *k, const char *v, rgb_t vc, float x0, float x1) {
    gfx_text(&font_m8, x0, y, k, C_DIM, ALIGN_L, MID, 0.4f);
    gfx_text(&font_sb9, x1, y, v, vc, ALIGN_R, MID, 0);
    gfx_line(x0, y + 9.5f, x1, y + 9.5f, 1, RGB(0x121820));
}

/* ------------------------------------------------------------------ screens */
/* Startup screen = the CHIPLER brand opening (owner request, matches the intro video frame): dark field with a dot grid
 * fading out on the left and a dim hex column on the right, the gold dot-matrix mark + CHIPLER, the slogan, the gold rule
 * and chipler.net, then the device name and the self-test progress bar. Drawn with primitives (no bitmap in flash). */
#define C_GOLD    RGB(0xD9A94E)
#define C_GOLD_DK RGB(0x3A2E17)
static rgb_t mix(rgb_t a, rgb_t b, float t) {   /* a -> b, t = 0..1 */
    int r = (int)(((a >> 16) & 0xFF) + (((int)((b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * t));
    int g = (int)(((a >> 8) & 0xFF) + (((int)((b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * t));
    int c = (int)((a & 0xFF) + (((int)(b & 0xFF) - (int)(a & 0xFF)) * t));
    return RGB((uint32_t)(r << 16 | g << 8 | c));
}
static void brand_mark(float x, float y, float s) {   /* 7x7 dot matrix: top/bottom rows, left column, 4x3 inner block */
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 7; c++) {
            bool on = r == 0 || r == 6 || c == 0 || (r >= 2 && r <= 4 && c >= 3);
            if (!on) continue;
            float cx = x + c * s + s / 2, cy = y + r * s + s / 2;
            if (r == 0 && c == 0) { gfx_circle(cx, cy, s * 0.42f, C_GOLD); gfx_circle(cx, cy, s * 0.20f, C_BG); }
            else gfx_circle(cx, cy, s * 0.36f, C_GOLD);
        }
}
static void scr_startup(const ui_model_t *m) {
    /* left: dot grid fading towards the centre */
    for (float x = 4.5f; x < 110; x += 9)
        for (float y = 4.5f; y < LS_H; y += 9) {
            float t = x / 110.0f;
            gfx_circle(x, y, 3.0f, mix(RGB(0x1E2125), C_BG, t * t));
        }
    /* right: the hex of "CHIPLER ENGINEER THE UNSEEN." as a dim memory dump */
    static const char msg[] = "CHIPLER ENGINEER THE UNSEEN.";
    for (unsigned row = 0; row < 9; row++) {
        char b[40]; int n = snprintf(b, sizeof b, "%08X", row * 3);
        for (unsigned k = 0; k < 3 && row * 3 + k < sizeof msg - 1; k++) n += snprintf(b + n, sizeof b - n, " %02X", (unsigned char)msg[row * 3 + k]);
        gfx_text(&font_m6, 316, 60 + row * 8.0f, b, RGB(0x1E232A), ALIGN_R, MID, 0.2f);
    }
    /* mark + wordmark, centred as one group */
    const float ms = 3.4f, mw = 7 * ms, gap = 8;
    float ww = gfx_text_width(&font_db22, "CHIPLER", 1.6f), x0 = (LS_W - (mw + gap + ww)) / 2.0f;
    brand_mark(x0, 76 - mw / 2, ms);
    gfx_text(&font_db22, x0 + mw + gap, 77, "CHIPLER", C_TXT, ALIGN_L, MID, 1.6f);
    gfx_text(&font_m8, LS_W / 2.0f, 106, "ENGINEER THE UNSEEN.", RGB(0xC9CED6), ALIGN_C, MID, 2.4f);
    gfx_line(LS_W / 2.0f - 26, 118, LS_W / 2.0f + 26, 118, 1, C_GOLD_DK);
    gfx_text(&font_m7, LS_W / 2.0f, 128, "chipler.net", C_DIM, ALIGN_C, MID, 0.8f);
    /* the device */
    gfx_text(&font_sb12, LS_W / 2.0f, 160, "CHIPLER Battery Emulator", C_TXT, ALIGN_C, MID, 0.6f);
    /* self-test progress */
    float f = m->boot_progress / 100.0f;
    gfx_round_rect(90, 184, 230, 187, 1.5f, RGB(0x1A1F26), true, 0, 0);
    if (f > 0.01f) gfx_round_rect(90, 184, 90 + 140 * (f > 1 ? 1 : f), 187, 1.5f, C_GOLD, true, 0, 0);
}

static void scr_dash(const ui_model_t *m) {
    const batt_profile_t *p = profile_get(m->prof_idx);
#if CHIPLER_AUTO_LEARN
    (void)p;
    {   char t[40]; snprintf(t, sizeof t, "AUTO \x80 %s", auto_state_str((auto_state_t)m->auto_state)); header(m, t); }
#else
    header(m, "DASHBOARD");
#endif
    bool valid = m->tq != TQ_ABSENT;
    char b[24];
    gfx_text(&font_m8, 10, 38, "VOLTAGE", C_DIM, ALIGN_L, MID, 1.2f);
    fmt_num(b, sizeof b, m->v, 3, valid);
    float vw = gfx_text_width(&font_d40, b, 0);
    gfx_text(&font_d40, 10, 64, b, valid ? C_TXT : C_DIM, ALIGN_L, MID, 0);
    gfx_text(&font_sb15, 10 + vw + 5, 70, "V", C_MEAS, ALIGN_L, false, 0);
    gfx_text(&font_m8, 10, 98, "CURRENT", C_DIM, ALIGN_L, MID, 1.2f);
    fmt_num(b, sizeof b, m->a, 3, valid);
    float aw = gfx_text_width(&font_d30, b, 0);
    gfx_text(&font_d30, 10, 120, b, valid ? C_AMP : C_DIM, ALIGN_L, MID, 0);
    gfx_text(&font_sb15, 10 + aw + 5, 126, "A", C_AMP, ALIGN_L, false, 0);
    fmt_num(b, sizeof b, m->w, 2, valid);
    gfx_text(&font_d13, 134, 112, b, valid ? C_TXT : C_DIM, ALIGN_L, MID, 0);
    gfx_text(&font_m8, 134, 128, "WATT", C_DIM, ALIGN_L, MID, 1.0f);
    const char *q = m->tq == TQ_MEASURED ? (m->sensor_uncalibrated ? "LOCAL INA226 \x80 MEASURED \x80 UNCAL" : "LOCAL INA226 \x80 MEASURED")
                  : m->tq == TQ_SIMULATED ? "SIMULATED \x80 NOT MEASURED" : "NO LOCAL MEASUREMENT";
    gfx_text(&font_m7, 10, 148, q, m->tq == TQ_ABSENT ? C_ERR : (m->tq == TQ_SIMULATED ? C_WARN : C_DIM), ALIGN_L, MID, 0.3f);
    /* cells: link + emulated SOC. The SOC cell is SIMULATED data, never mixed with the measurement. */
    static const char *lk[] = {"I2C OFF", "I2C IDLE", "I2C ACTIVE", "I2C ERROR"};
    static const rgb_t lkc[] = {C_SUB, C_WARN, C_OK, C_ERR};
    static const rgb_t lkb[] = {C_PANEL, C_WARN_BG, C_OK_BG, C_ERR_BG};
    pill(10, 160, 104, 180, lk[m->link], lkc[m->link], lkb[m->link]);
    bool sent = m->soc_applied >= 0 && m->soc_applied == m->soc_requested;
    gfx_round_rect(110, 160, 206, 192, 6, C_SIM_BG, true, C_SIM_LN, 1);
    snprintf(b, sizeof b, "%d%%", m->soc_requested);
    gfx_text(&font_d13, 118, 170, b, sent ? C_SIM : RGB(0x6E6390), ALIGN_L, MID, 0);
#if CHIPLER_AUTO_LEARN
    /* our value, sent in our replies; the phone's own SOC is not read on this bus */
    const char *lab = m->auto_bus == AB_RUN && sent ? "OUR SOC \x80 SENT" : "OUR SOC \x80 NOT SENT";
#else
    const char *lab = m->soc_result == SOC_RES_APPLIED_SYNTHETIC ? "SIM \x80 BENCH" :
                      m->soc_result == SOC_RES_APPLIED_VERIFIED ? "SIM SOC" :
                      m->soc_result == SOC_RES_APPLIED_PHONE_DATA ? "SIM  DATA SET" :
                      m->soc_result == SOC_RES_RECORDED_RETAINED ? "SIM  RECORDED" :
                      m->soc_result == SOC_RES_BUSY_RETRY ? "SIM \x80 PENDING" : "SIM \x80 NOT SENT";
#endif
    gfx_text(&font_m6, 158, 184, lab, m->soc_result == SOC_RES_APPLIED_SYNTHETIC || m->soc_result == SOC_RES_APPLIED_VERIFIED || m->soc_result == SOC_RES_APPLIED_PHONE_DATA ? C_SIM : C_WARN, ALIGN_C, MID, 0.3f);
    /* output/reply state (legacy dashboard pill, honest wording: it never means the phone supply is cut) */
    switch (m->out) {
    case OUT_ON:    pill(214, 176, 314, 194, "REPLIES ON", C_OK, C_OK_BG); break;
    case OUT_ARMED: pill(214, 176, 314, 194, "ARMED", C_WARN, C_WARN_BG); break;
    default:
        if (m->bus_enabled) pill(214, 176, 314, 194, "DATA REPLIES ON", C_MEAS, C_PANEL);   /* data-only experiment, not power */
        else pill(214, 176, 314, 194, "REPLIES OFF", C_SUB, C_PANEL);
        break;
    }
#if CHIPLER_AUTO_LEARN
    /* right column: AUTO evidence instead of a model name (no model is ever selected or named) */
    {   rgb_t sc = m->auto_bus == AB_RUN ? C_OK : m->auto_bus == AB_LISTEN ? C_WARN : C_SUB;
        gfx_text(&font_m7, 264, 148, m->auto_bus == AB_RUN ? "REPLYING" : m->auto_bus == AB_LISTEN ? "LISTENING (0xFF)" : "REPLIES OFF", sc, ALIGN_C, MID, 0);
        char b2[40];
        if (m->auto_have) snprintf(b2, sizeof b2, "FP %08lX G%lu", (unsigned long)m->auto_fp, (unsigned long)m->auto_gen);
        else snprintf(b2, sizeof b2, "REQ %u  COV %u  UNK %u", m->auto_dist, m->auto_cov, m->auto_unc);
        gfx_text(&font_m6, 264, 160, b2, C_DIM, ALIGN_C, MID, 0.3f);
        snprintf(b2, sizeof b2, "%s  SAVED %u%s", m->auto_have ? (m->auto_reused ? "REUSED" : "NEW") : "-", m->auto_saved, m->auto_partial ? " LOW CONF" : "");
        gfx_text(&font_m6, 264, 171, b2, m->auto_partial ? C_WARN : C_MEAS, ALIGN_C, MID, 0.3f);
        gfx_text(&font_m6, 10, 196, m->auto_note, C_SUB, ALIGN_L, MID, 0.2f);
        gfx_text(&font_m6, 214, 196, "PHONE SOC: NOT READ", C_DIM, ALIGN_L, MID, 0.2f);
    }
#else
    /* right column info under the buttons */
    gfx_text(&font_m7, 264, 148, p ? p->name : "NO PROFILE", C_TXT, ALIGN_C, MID, 0);
    snprintf(b, sizeof b, "%s", p ? p->id : "-");
    gfx_text(&font_m6, 264, 160, b, C_DIM, ALIGN_C, MID, 0.3f);
    {   const char *st = "UNVERIFIED"; rgb_t sc = C_WARN;
        if (p) switch (p->status) {
        case PROF_VERIFIED: st = "VERIFIED"; sc = C_OK; break;
        case PROF_BENCH_TEST: st = "BENCH ONLY"; sc = C_SIM; break;
        case PROF_PHONE_DATA_TEST: st = p->backend == BK_SHARED_12PRO ? "SHARED 12 PRO DATA" : "PHONE DATA TEST"; sc = C_MEAS; break;
        case PROF_IN_DEVELOPMENT: st = "IN DEVELOPMENT"; break;
        default: break; }
        gfx_text(&font_m6, 264, 171, st, sc, ALIGN_C, MID, 0.5f); }
#endif
    /* DS-UI-11 D-01 / C-13: the emergency gesture and the manual PSU are always visible on the dashboard */
    gfx_text(&font_m7, 10, 210, "HOLD 1.2 s ANYWHERE = STOP REPLIES", C_DIM, ALIGN_L, MID, 0.3f);
    gfx_text(&font_m7, 10, 224, "EXTERNAL PSU IS SWITCHED BY HAND", C_WARN, ALIGN_L, MID, 0.3f);
    toast(m);
}

static void scr_menu(const ui_model_t *m) {
    header(m, "MENU");
    char b[32]; (void)b;
    toast(m);
}

static void scr_soc(const ui_model_t *m) {
    header(m, "EMULATED SOC");
    gfx_text(&font_sb8, 160, 40, "SIMULATED VALUE \x80 NOT A REAL BATTERY", C_SIM, ALIGN_C, MID, 0.5f);
    char b[48];
    snprintf(b, sizeof b, "REQUESTED %d%%   %s", m->soc_requested,
             m->soc_applied >= 0 && m->soc_applied == m->soc_requested ? (m->soc_result == SOC_RES_APPLIED_PHONE_DATA ? "REPLY DATA SET" : "SENT: YES") : (m->soc_result == SOC_RES_RECORDED_RETAINED ? "RECORDED VALUE SERVED" : "SENT: NO"));
    gfx_text(&font_m8, 160, 58, b, C_SUB, ALIGN_C, MID, 0.3f);
    if (!m->soc_capable) {
        gfx_round_rect(20, 84, 300, 160, 8, C_WARN_BG, true, C_WARN, 1);
        gfx_text(&font_sb12, 160, 104, "SOC CONTROL LOCKED", C_WARN, ALIGN_C, MID, 0.6f);
        gfx_text(&font_m8, 160, 126, "This profile has no verified SOC backend.", C_SUB, ALIGN_C, MID, 0);
        gfx_text(&font_m8, 160, 140, "Nothing is sent to the phone.", C_SUB, ALIGN_C, MID, 0);
    } else {
#if CHIPLER_OWNER_PHONE_TEST
        snprintf(b, sizeof b, "SET %d%%", m->soc_pick);
        gfx_text(&font_db22, 160, 80, b, m->soc_pick == m->soc_requested ? C_SUB : C_SIM, ALIGN_C, MID, 0);
        gfx_text(&font_m7, 160, 194, "1-100 %  \x80  APPLY then confirm", C_DIM, ALIGN_C, MID, 0);
#else
        gfx_text(&font_m8, 160, 160, "Select a preset, then confirm.", C_DIM, ALIGN_C, MID, 0);
#endif
    }
    toast(m);
}

const char *ui_ls_model_status(const batt_profile_t *p) {
    return p->backend == BK_SHARED_12PRO ? "SHARED 12 PRO TEST" : profile_status_str(p->status);   /* review 106 */
}
bool ui_ls_model_row_fits(const batt_profile_t *p) {   /* list row x0 = 6, name at x0 + 12 (draw_regions, LS_ST_LISTROW) */
    float name_end = 6 + 12 + gfx_text_width(&font_sb9, p->name, 0);
    float status_start = UI_LS_MODEL_STATUS_X - gfx_text_width(&font_sb8, ui_ls_model_status(p), 0.2f);
    return name_end + 6 <= status_start;
}
static void scr_models(const ui_model_t *m) {
    header(m, "PHONE MODEL");
    int ws = models_window_start(m), n = profile_count();
    for (int k = 0; k < LIST_ROWS && ws + k < n; k++) {
        const batt_profile_t *p = profile_get(ws + k);
        float y = 30 + k * 42 + 19;
        const char *st = ui_ls_model_status(p);
        rgb_t c = p->status == PROF_VERIFIED ? C_OK : (p->status == PROF_BENCH_TEST ? C_SIM : (p->status == PROF_PHONE_DATA_TEST ? C_MEAS : C_WARN));
        gfx_text(&font_sb8, UI_LS_MODEL_STATUS_X, y, st, c, ALIGN_R, MID, 0.2f);
    }
    char b[24];
    snprintf(b, sizeof b, "%d-%d / %d", ws + 1, ws + LIST_ROWS > n ? n : ws + LIST_ROWS, n);
    gfx_text(&font_m7, 286, 188, b, C_DIM, ALIGN_C, MID, 0.2f);
    toast(m);
}

static void scr_proto(const ui_model_t *m) {
    const batt_profile_t *p = profile_get(m->prof_idx);
    header(m, "PROTOCOL");
    static const char *bus[] = {"DISABLED", "IDLE", "ACTIVE", "ERROR"};
    static const rgb_t busc[] = {C_SUB, C_WARN, C_OK, C_ERR};
    char v[8][28]; rgb_t col[8];
    const char *k[8] = {"BUS", "ADDRESS", "PROFILE", "RX / TX", "UNKNOWN READS", "IGNORED WRITES", "LAST CMD", "0x71 / AUTH"};
    snprintf(v[0], 28, "%s", bus[m->link]); col[0] = busc[m->link];
    if (!m->addr) { snprintf(v[1], 28, "REQ. CAPTURE"); col[1] = C_WARN; }
    else if (!m->addr_verified) { snprintf(v[1], 28, "0x%02X CANDIDATE", m->addr); col[1] = C_WARN; }
    else { snprintf(v[1], 28, "0x%02X", m->addr); col[1] = C_TXT; }
    snprintf(v[2], 28, "%s", p ? p->id : "-"); col[2] = C_TXT;
    snprintf(v[3], 28, "%lu / %lu", (unsigned long)m->rx, (unsigned long)m->tx); col[3] = C_TXT;
    snprintf(v[4], 28, "%lu", (unsigned long)m->unknown); col[4] = m->unknown ? C_WARN : C_TXT;
    snprintf(v[5], 28, "%lu", (unsigned long)m->ignored); col[5] = m->ignored ? C_WARN : C_TXT;
    if (m->last_reg >= 0) snprintf(v[6], 28, "0x%02X", m->last_reg); else snprintf(v[6], 28, "0x--");
    col[6] = C_SUB;
    snprintf(v[7], 28, "UNKNOWN"); col[7] = C_WARN;
    float y = 40;
    for (int i = 0; i < 8; i++, y += 19) line_kv(y, k[i], v[i], col[i], 10, 204);
    snprintf(v[0], 28, "%lu/%lu", (unsigned long)m->replay_frames, (unsigned long)m->replay_total);
    gfx_text(&font_m6, 264, 40, "OFFLINE REPLAY", C_DIM, ALIGN_C, MID, 0.4f);
    gfx_text(&font_sb9, 264, 54, v[0], m->replay_errors ? C_ERR : C_MEAS, ALIGN_C, MID, 0);
    snprintf(v[1], 28, "ERR %lu", (unsigned long)m->replay_errors);
    gfx_text(&font_m6, 264, 67, v[1], m->replay_errors ? C_ERR : C_DIM, ALIGN_C, MID, 0.4f);
    snprintf(v[0], 28, "I2C ERRORS %lu", (unsigned long)m->overflows);
    gfx_text(&font_m6, 264, 92, v[0], m->overflows ? C_ERR : C_DIM, ALIGN_C, MID, 0.2f);
    gfx_text(&font_m6, 264, 105, "CAPTURE DATA - OFFLINE", C_DIM, ALIGN_C, MID, 0.1f);
    toast(m);
}

static void scr_batt(const ui_model_t *m) {
    header(m, "BATTERY DATA");
    char v[28];
    bool sent = m->soc_applied >= 0 && m->soc_applied == m->soc_requested;
    snprintf(v, sizeof v, "%d%%  (user setting)", m->soc_requested);
    line_kv(44, "REQUESTED SOC", v, C_SIM, 10, 310);
    line_kv(64, "SENT TO PHONE", sent ? (m->soc_result == SOC_RES_APPLIED_SYNTHETIC ? "BENCH SYNTHETIC ONLY" : (m->soc_result == SOC_RES_APPLIED_PHONE_DATA ? "REPLY DATA ONLY" : "YES")) : "NO", sent ? C_SIM : C_WARN, 10, 310);
    line_kv(84, "PHONE-REPORTED SOC", "UNKNOWN", C_WARN, 10, 310);
    line_kv(104, "SOC CONTROL", m->soc_capable ? (m->soc_result == SOC_RES_APPLIED_PHONE_DATA || m->soc_result == SOC_RES_RECORDED_RETAINED ? "DATA TEST (0x0D/0x0F)" : "AVAILABLE") : "LOCKED (no verified backend)", m->soc_capable ? C_OK : C_WARN, 10, 310);
    line_kv(124, "0x71 / AUTHENTICATION", "UNKNOWN \x80 NOT CLAIMED", C_WARN, 10, 310);
    char mv[28];
    if (m->tq == TQ_ABSENT) snprintf(mv, sizeof mv, "NO MEASUREMENT");
    else snprintf(mv, sizeof mv, "%.3f V  %.3f A%s", (double)m->v, (double)m->a, m->tq == TQ_SIMULATED ? " SIM" : "");
    line_kv(144, "LOCAL INA226 (not phone data)", mv, m->tq == TQ_MEASURED ? C_MEAS : C_DIM, 10, 310);
    line_kv(164, "INA226 CALIBRATION", !m->ina_present ? "SENSOR ABSENT" : (m->sensor_uncalibrated ? "NOT DONE (R010 NOMINAL)" : "DONE"),
            !m->ina_present ? C_ERR : (m->sensor_uncalibrated ? C_WARN : C_OK), 10, 310);
    toast(m);
}

static void scr_settings(const ui_model_t *m) {
    header(m, "SETTINGS");
    line_kv(152, "TOUCH CALIBRATION", m->touch_cal_valid ? "DONE" : "NOT DONE", m->touch_cal_valid ? C_OK : C_WARN, 10, 310);
    line_kv(168, "SAVED TO FLASH", m->settings_saved ? "YES" : "NO (RAM ONLY)", m->settings_saved ? C_OK : C_DIM, 10, 310);
    line_kv(184, "ROTATION", m->rot180 ? "180 DEG" : "NORMAL", C_TXT, 10, 310);
    gfx_text(&font_m6, 166, 112, "Module chip U1 is marked HR2046.", C_DIM, ALIGN_L, MID, 0.2f);
    gfx_text(&font_m6, 166, 123, "Check TOUCH DIAGNOSTIC first.", C_DIM, ALIGN_L, MID, 0.2f);
    toast(m);
}

static void scr_touchdiag(const ui_model_t *m, const ls_touch_info_t *t) {
    header(m, "TOUCH DIAGNOSTIC");
    char v[32];
    line_kv(40, "BACKEND", t->hw_enabled ? "HR2046 (XPT2046 cmds)" : "NONE", t->hw_enabled ? C_WARN : C_ERR, 10, 310);
    snprintf(v, sizeof v, "%u / %u / %u", t->rx, t->ry, t->rz);
    line_kv(58, "RAW X / Y / Z", v, C_TXT, 10, 310);
    snprintf(v, sizeof v, "%d, %d", t->sx, t->sy);
    line_kv(76, "SCREEN X, Y", v, t->cal_valid ? C_TXT : C_DIM, 10, 310);
    line_kv(94, "PEN / T_IRQ PIN", t->down ? (t->irq ? "DOWN / ACTIVE" : "DOWN / idle") : (t->irq ? "UP / ACTIVE" : "UP / idle"), C_TXT, 10, 310);
    snprintf(v, sizeof v, "%lu / %lu / %lu", (unsigned long)t->polls, (unsigned long)t->presses, (unsigned long)t->taps);
    line_kv(112, "POLLS / PRESSES / TAPS", v, C_TXT, 10, 310);
    snprintf(v, sizeof v, "%lu rail / %lu SPI-refused", (unsigned long)t->rejected, (unsigned long)t->spi_violations);
    line_kv(130, "REJECTED", v, (t->rejected || t->spi_violations) ? C_WARN : C_TXT, 10, 310);
    line_kv(148, "CALIBRATION", t->cal_valid ? "VALID" : "NONE", t->cal_valid ? C_OK : C_WARN, 10, 310);
    gfx_text(&font_m6, 160, 176, t->ever_pen ? "Touch the panel: raw values should move." : "No contact seen yet. If raw stays 0 check T_DO, T_CS and T_IRQ wiring.", C_DIM, ALIGN_C, MID, 0.2f);
    /* live crosshair dot (calibrated position) */
    if (t->down && t->cal_valid) {
        gfx_circle((float)t->sx, (float)t->sy, 4, C_AMP);
    }
}

static void scr_cal(const ui_model_t *m, const ls_touch_info_t *t) {
    char b[48];
    gfx_text(&font_sb12, 160, 100, "TOUCH CALIBRATION", C_TXT, ALIGN_C, MID, 1.0f);
    snprintf(b, sizeof b, "Tap the cross  (%d of %d)", t->cal_step + 1, t->cal_total);
    gfx_text(&font_m10, 160, 122, b, C_SUB, ALIGN_C, MID, 0);
    snprintf(b, sizeof b, "Cancels in %lu s  \x80  or press the KEY", (unsigned long)(t->cal_left_ms / 1000));
    gfx_text(&font_m7, 160, 142, b, C_DIM, ALIGN_C, MID, 0.2f);
    if (t->cal_fail) gfx_text(&font_sb9, 160, 166, "Fit rejected \x80 try again", C_ERR, ALIGN_C, MID, 0.5f);
    float x = t->cal_tx, y = t->cal_ty;
    gfx_line(x - 12, y, x + 12, y, 2, C_AMP);
    gfx_line(x, y - 12, x, y + 12, 2, C_AMP);
    gfx_circle(x, y, 3, C_AMP);
    (void)m;
}

static const char *conf_text(const ui_model_t *m, char *l1, size_t n1, char *l2, size_t n2) {
    const char *title = "CONFIRM";
    l1[0] = l2[0] = 0;
    switch (m->confirm) {
    case CONF_SOC:
        title = "APPLY EMULATED SOC?";
        snprintf(l1, n1, "%d%% > %d%%", m->confirm_from, m->confirm_arg);
        snprintf(l2, n2, m->soc_capable ? "Simulated value for the active profile." : "Profile cannot apply it: NOT SENT.");
        break;
    case CONF_MODEL: {
        const batt_profile_t *p = profile_get(m->confirm_arg);
        title = "LOAD PROFILE?";
        snprintf(l1, n1, "%s", p ? p->name : "-");
        if (p && p->status == PROF_PHONE_DATA_TEST && p->backend == BK_SHARED_12PRO) snprintf(l2, n2, "12 PRO DATA \x80 supply switched manually");
        else if (p && p->status == PROF_PHONE_DATA_TEST) snprintf(l2, n2, "DATA ONLY \x80 supply is switched manually");
        else snprintf(l2, n2, "%s \x80 %s", p ? p->id : "-", p ? profile_status_str(p->status) : "-");
        break;
    }
#if CHIPLER_AUTO_LEARN
    case CONF_CLEAR_PROFILES:
        title = "CLEAR SAVED PROFILES?";
        snprintf(l1, n1, "%u saved profile(s) erased", m->auto_saved);
        snprintf(l2, n2, "Only after STOP. AUTO learns again.");
        break;
#endif
    case CONF_ARM:
        title = "ENABLE BUS REPLIES?";
        snprintf(l1, n1, "I2C target replies on GP6/GP7.");
        snprintf(l2, n2, "Power path is switched manually.");
        break;
    default:
        snprintf(l1, n1, "-");
        break;
    }
    return title;
}

static void scr_confirm(const ui_model_t *m) {
    header(m, "CONFIRM");
    char l1[40], l2[48];
    const char *t = conf_text(m, l1, sizeof l1, l2, sizeof l2);
    gfx_text(&font_sb12, 160, 52, t, C_TXT, ALIGN_C, MID, 0.6f);
    gfx_text(&font_sb15, 160, 88, l1, C_AMP, ALIGN_C, MID, 0.2f);
    gfx_text(&font_m8, 160, 112, l2, C_SUB, ALIGN_C, MID, 0);
    gfx_text(&font_m7, 160, 128, "Default is CANCEL. A tap needs a fresh press.", C_DIM, ALIGN_C, MID, 0.2f);
}

/* ---------------- CHARGE TEST (ported from the round UI; unreachable unless CHG_UI_ENABLED) ---------------- */
static void chg_vals(const ui_model_t *m, float *v, float *a, float *w) {
    if (m->chg_flow == CHF_STALE) { *v = m->lg_v; *a = m->lg_a; *w = m->lg_w; }
    else { *v = m->v; *a = m->a; *w = m->w; }
}
static void scr_chg_page(const ui_model_t *m) {
    static const char *title[CHG_PAGES] = {"CHARGE TEST", "CHARGER INPUT", "BATTERY TERMINAL", "BATTERY PROTOCOL", "EMULATED SOC", "TEST RESULT"};
    int pg = m->chg_page >= 0 && m->chg_page < CHG_PAGES ? m->chg_page : 0;
    header(m, title[pg]);
    chg_flow_t f = m->chg_flow;
    bool valid = f != CHF_UNAVAILABLE, stale = f == CHF_STALE;
    float v, a, w; chg_vals(m, &v, &a, &w);
    int dir = f == CHF_SUPPLYING ? 1 : f == CHF_RECEIVING ? -1 : 0;
    if (stale) dir = a >= CHG_I_THRESH_A ? 1 : a <= -CHG_I_THRESH_A ? -1 : 0;
    char b[40];
    if (pg == 0) {
        const char *st; rgb_t fg, bg;
        switch (f) {
        case CHF_SUPPLYING: st = "SUPPLYING PHONE"; fg = C_AMP; bg = C_WARN_BG; break;
        case CHF_RECEIVING: st = "RECEIVING CURRENT"; fg = C_CHG; bg = C_CHG_BG; break;
        case CHF_NO_NET: st = "NO NET CURRENT"; fg = C_SUB; bg = C_PANEL; break;
        case CHF_STALE: snprintf(b, sizeof b, "DATA STALE %.1f s", (double)(m->t_age_ms / 1000.0f)); st = b; fg = C_WARN; bg = C_WARN_BG; break;
        default: st = m->chg_input_fitted ? "NO CHARGER" : "CHARGER UNKNOWN"; fg = C_SUB; bg = C_PANEL; break;
        }
        pill(10, 34, 150, 52, st, fg, bg);
        gfx_text(&font_m7, 10, 66, "BATTERY TERMINAL", C_DIM, ALIGN_L, MID, 1.2f);
        if (valid) snprintf(b, sizeof b, "%.3f", (double)v); else snprintf(b, sizeof b, "-.---");
        float vw = gfx_text_width(&font_d40, b, 0);
        gfx_text(&font_d40, 10, 90, b, valid && !stale ? C_TXT : C_DIM, ALIGN_L, MID, 0);
        gfx_text(&font_sb15, 10 + vw + 5, 96, "V", C_MEAS, ALIGN_L, false, 0);
        if (valid) snprintf(b, sizeof b, "%.3f", (double)fabsf(a)); else snprintf(b, sizeof b, "-.---");
        rgb_t ic = !valid ? C_DIM : stale ? C_STALE : dir > 0 ? C_AMP : dir < 0 ? C_CHG : C_SUB;
        float aw = gfx_text_width(&font_d30, b, 0);
        gfx_text(&font_d30, 10, 130, b, ic, ALIGN_L, MID, 0);
        gfx_text(&font_sb15, 10 + aw + 5, 136, "A", ic, ALIGN_L, false, 0);
        gfx_text(&font_sb8, 176, 100, !valid ? "MEASUREMENT UNAVAILABLE" : stale ? "STALE" : dir > 0 ? "EMU > PHONE  SUPPLYING" : dir < 0 ? "PHONE > EMU  CHARGE IN" : "NO NET CURRENT", !valid ? C_ERR : ic, ALIGN_L, MID, 0.3f);
        if (valid) snprintf(b, sizeof b, "%.2f W", (double)fabsf(w)); else snprintf(b, sizeof b, "-.-- W");
        gfx_text(&font_d13, 176, 122, b, valid && !stale ? C_TXT : C_DIM, ALIGN_L, MID, 0);
        bool sent = m->soc_applied >= 0 && m->soc_applied == m->soc_requested;
        snprintf(b, sizeof b, "SIM SOC %d%%  %s", m->soc_requested, sent ? "" : "NOT SENT");
        gfx_text(&font_sb8, 176, 142, b, sent ? C_SIM : C_WARN, ALIGN_L, MID, 0.3f);
        gfx_text(&font_m7, 10, 156, m->soc_requested == 100 && f == CHF_NO_NET ? "100% MAY STOP CHARGING - SEE SOC" : stale ? "RESULT - NOT EVALUATED - STALE" : "RESULT - NOT EVALUATED", C_SUB, ALIGN_L, MID, 0.3f);
        if (m->tq == TQ_SIMULATED) gfx_text(&font_m7, 310, 156, "SIMULATED - NOT MEASURED", C_WARN, ALIGN_R, MID, 0.3f);
    } else if (pg == 1) {
        line_kv(44, "VOLTAGE", "UNAVAILABLE", C_DIM, 10, 310); line_kv(62, "CURRENT", "UNAVAILABLE", C_DIM, 10, 310);
        line_kv(80, "POWER", "UNAVAILABLE", C_DIM, 10, 310); line_kv(98, "SOURCE", "NOT FITTED", C_WARN, 10, 310);
        line_kv(116, "UT658A / PW11", "NOT LINKED", C_WARN, 10, 310);
        gfx_text(&font_sb8, 160, 146, "INPUT AMPS ARE NOT BATTERY CHARGE AMPS", C_SUB, ALIGN_C, MID, 0.3f);
    } else if (pg == 2) {
        rgb_t dc = !valid ? C_DIM : stale ? C_STALE : a > 0 ? C_AMP : C_CHG;
        char r[6][28];
        if (valid) {
            snprintf(r[0], 28, "%.3f V", (double)v); snprintf(r[1], 28, "%.3f A", (double)fabsf(a));
            snprintf(r[2], 28, "%s", fabsf(a) < CHG_I_THRESH_A ? "NO NET" : a > 0 ? "EMU > PHONE" : "PHONE > EMU");
            snprintf(r[3], 28, "%.2f W %s", (double)fabsf(w), a > 0 ? "SUPPLIED" : "RECEIVED");
        } else for (int i = 0; i < 4; i++) snprintf(r[i], 28, "UNAVAILABLE");
        if (!m->t_have) snprintf(r[4], 28, "NO SAMPLE"); else if (m->t_age_ms > 60000u) snprintf(r[4], 28, "> 60 s"); else snprintf(r[4], 28, "%lu ms", (unsigned long)m->t_age_ms);
        snprintf(r[5], 28, "%s", m->tq == TQ_SIMULATED ? "SIMULATED" : "POWER MONITOR");
        static const char *k[6] = {"VOLTAGE", "CURRENT", "DIRECTION", "POWER", "SAMPLE AGE", "SOURCE"};
        for (int i = 0; i < 6; i++) line_kv(42 + i * 19, k[i], r[i], i < 4 ? (valid ? dc : C_DIM) : (i == 5 && m->tq == TQ_SIMULATED ? C_WARN : C_SUB), 10, 310);
    } else if (pg == 3) {
        static const char *bus[] = {"DISABLED", "IDLE", "ACTIVE", "ERROR"};
        char r[6][28];
        snprintf(r[0], 28, "%s", bus[m->link]); snprintf(r[1], 28, "%lu", (unsigned long)m->txns);
        snprintf(r[2], 28, "%lu / %lu", (unsigned long)m->rx, (unsigned long)m->tx); snprintf(r[3], 28, "%lu", (unsigned long)m->unknown);
        snprintf(r[4], 28, "%lu", (unsigned long)m->ignored); snprintf(r[5], 28, "%lu", (unsigned long)m->overflows);
        static const char *k[6] = {"BUS ACTIVITY", "TRANSACTIONS", "RX / TX BYTES", "UNKNOWN READS", "IGNORED WRITES", "OVERFLOWS"};
        for (int i = 0; i < 6; i++) line_kv(42 + i * 18, k[i], r[i], i == 5 && m->overflows ? C_ERR : C_TXT, 10, 310);
        line_kv(150, "VALIDATED", "0 - NO REFERENCE", C_WARN, 10, 310);
    } else if (pg == 4) {
        static const char *lab[3] = {"25%", "50%", "100%"};
        for (int i = 0; i < 3; i++) {
            int x = 40 + i * 90; bool cur = k_soc_presets[i] == m->soc_requested;
            gfx_round_rect(x, 40, x + 80, 72, 8, cur ? C_SIM_BG : C_PANEL, true, cur ? C_SIM : C_LINE, cur ? 1.6f : 1);
            gfx_text(&font_d13, x + 40, 56, lab[i], cur ? C_TXT : C_SUB, ALIGN_C, MID, 0);
        }
        gfx_text(&font_sb8, 160, 90, "100% MAY REDUCE OR STOP CHARGING", C_WARN, ALIGN_C, MID, 0.3f);
        gfx_text(&font_m7, 160, 106, "LOW CURRENT AT 100% IS NOT A FAULT", C_SUB, ALIGN_C, MID, 0.2f);
        gfx_text(&font_m7, 160, 120, "SOC - VOLTAGE - CAPACITY - TEMP MUST MATCH A VERIFIED PROFILE", C_SUB, ALIGN_C, MID, 0.1f);
        bool sent = m->soc_applied >= 0 && m->soc_applied == m->soc_requested;
        gfx_text(&font_sb8, 160, 140, sent ? (m->soc_result == SOC_RES_APPLIED_SYNTHETIC ? "BENCH SYNTHETIC BACKEND - NOT A PHONE" : "APPLIED BY VERIFIED PROFILE") : "PHONE: NOT SENT - PROFILE UNSUPPORTED", sent ? C_SUB : C_WARN, ALIGN_C, MID, 0.3f);
        gfx_text(&font_m7, 160, 156, "CHANGE IT IN CHARGE MENU > EMULATED SOC", C_DIM, ALIGN_C, MID, 0.2f);
    } else {
        gfx_round_rect(50, 40, 270, 80, 10, C_PANEL, true, C_SUB, 1.2f);
        gfx_text(&font_db17, 160, 60, "NOT EVALUATED", C_SUB, ALIGN_C, MID, 0.5f);
        gfx_text(&font_sb8, 160, 94, "TEST NOT RUN", C_SUB, ALIGN_C, MID, 0.4f);
        static const char *l[] = {"NEEDS: SINK + TEST RECIPE + LIMITS", "BASELINE FOR THIS EXACT MODEL", "RECORDED CHARGER / CABLE / TEMP", "AMPS OR BUS TRAFFIC ALONE = NO VERDICT"};
        for (int i = 0; i < 4; i++) gfx_text(&font_m8, 160, 110 + i * 13, l[i], i == 3 ? C_WARN : C_SUB, ALIGN_C, MID, 0.2f);
    }
    snprintf(b, sizeof b, "PAGE %d / %d", pg + 1, CHG_PAGES);
    gfx_text(&font_m7, 160, 160, b, C_DIM, ALIGN_C, MID, 0.4f);
    toast(m);
}
static void scr_chg_menu(const ui_model_t *m) {
    header(m, "CHARGE MENU");
    char b[40]; snprintf(b, sizeof b, "SIM SOC %d%%   OUTPUT: %s", m->soc_requested, m->chg_sink_fitted ? "SINK OFF" : "NO SINK");
    gfx_text(&font_m7, 160, 212, "", C_DIM, ALIGN_C, MID, 0);
    toast(m);
    (void)b;
}
static void scr_chg_confirm(const ui_model_t *m) {
    header(m, "CONFIRM");
    gfx_text(&font_sb12, 160, 40, "START CHARGE TEST?", C_WARN, ALIGN_C, MID, 0.8f);
    for (int i = 0; i < CHK_N; i++) {
        int st = m->chg_checks.c[i]; float y = 62 + i * 16;
        rgb_t c = st == CHK_PASS ? C_OK : st == CHK_FAIL ? C_ERR : C_WARN;
        gfx_circle(60, y, 5, st == CHK_PASS ? C_OK_BG : st == CHK_FAIL ? C_ERR_BG : C_WARN_BG);
        gfx_text(&font_sb8, 60, y + 0.3f, st == CHK_PASS ? "+" : st == CHK_FAIL ? "x" : "?", c, ALIGN_C, MID, 0);
        gfx_text(&font_m8, 74, y, chk_name(i), st == CHK_PASS ? C_TXT : C_SUB, ALIGN_L, MID, 0.4f);
    }
    gfx_text(&font_sb8, 160, 146, m->chg_start_ok ? "SINK ENABLES ONLY AFTER START" : "START BLOCKED - FIX FAILED CHECKS", m->chg_start_ok ? C_SUB : C_ERR, ALIGN_C, MID, 0.3f);
}

static void scr_fault(const ui_model_t *m) {
    header(m, "FAULT");
    gfx_text(&font_db17, 160, 50, fault_name(m->fault), C_ERR, ALIGN_C, MID, 0.6f);
    char b[48] = "";
    switch (m->fault) {
    case FLT_OVERVOLTAGE: snprintf(b, sizeof b, "V %.2f V > LIMIT %.2f V", (double)m->fault_value, (double)m->lim_ov); break;
    case FLT_OVERCURRENT: snprintf(b, sizeof b, "I %.2f A OVER LIMIT", (double)m->fault_value); break;
    case FLT_UNDERVOLTAGE: snprintf(b, sizeof b, "V %.2f V BELOW LIMIT", (double)m->fault_value); break;
    case FLT_NO_MONITOR: snprintf(b, sizeof b, "POWER MONITOR UNAVAILABLE"); break;
    case FLT_ALERT_PIN: snprintf(b, sizeof b, "HARDWARE ALERT LINE ACTIVE"); break;
    default: snprintf(b, sizeof b, "CHECK SETUP"); break;
    }
    gfx_text(&font_m10, 160, 76, b, C_TXT, ALIGN_C, MID, 0);
    gfx_round_rect(40, 92, 280, 130, 8, C_ERR_BG, true, C_ERR, 1);
    gfx_text(&font_sb9, 160, 104, "BUS REPLIES STOPPED", C_ERR, ALIGN_C, MID, 0.8f);
    gfx_text(&font_m8, 160, 119, "EXTERNAL PHONE SUPPLY: SWITCH OFF MANUALLY", C_WARN, ALIGN_C, MID, 0.2f);
    snprintf(b, sizeof b, "CODE  %s", fault_code(m->fault));
    gfx_text(&font_sb8, 230, 172, b, C_DIM, ALIGN_C, MID, 0.8f);
}

void ui_ls_render(const ui_model_t *m, const ls_touch_info_t *ti) {
    static const ls_touch_info_t none;
    if (!ti) ti = &none;
    gfx_clear(C_BG);
    ls_layout_t lay;
    ui_ls_layout(m, &lay);
    for (int i = 0; i < lay.n; i++) draw_region(&lay.r[i]);
    switch (m->screen) {
    case SCR_STARTUP: scr_startup(m); break;
    case SCR_DASH: scr_dash(m); break;
    case SCR_MENU: scr_menu(m); break;
    case SCR_SOC: scr_soc(m); break;
    case SCR_MODELS: scr_models(m); break;
    case SCR_PROTO: scr_proto(m); break;
    case SCR_BATT: scr_batt(m); break;
    case SCR_SETTINGS: scr_settings(m); break;
    case SCR_TOUCHDIAG: scr_touchdiag(m, ti); break;
    case SCR_CAL: scr_cal(m, ti); break;
    case SCR_CONFIRM: if (m->confirm == CONF_CHG_START) scr_chg_confirm(m); else scr_confirm(m); break;
    case SCR_CHG: scr_chg_page(m); break;
    case SCR_CHG_MENU: scr_chg_menu(m); break;
    case SCR_FAULT: scr_fault(m); break;
    default: header(m, "NOT AVAILABLE"); break;
    }
}
