#include "gfx.h"
#include <math.h>
#include <string.h>

static uint16_t *s_fb;

void gfx_init(uint16_t *fb) { s_fb = fb; }
uint16_t *gfx_fb(void) { return s_fb; }

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void gfx_clear(rgb_t c) {
    uint16_t p = gfx_565(c);
    for (int i = 0; i < GFX_W * GFX_H; i++) s_fb[i] = p;
}

void gfx_pixel_blend(int x, int y, rgb_t c, uint8_t a) {
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H || a == 0) return;
    uint16_t *d = &s_fb[y * GFX_W + x];
    if (a == 255) { *d = gfx_565(c); return; }
    rgb_t b = gfx_888(*d);
    uint32_t r = (((c >> 16) & 255) * a + ((b >> 16) & 255) * (255 - a)) / 255;
    uint32_t g = (((c >> 8) & 255) * a + ((b >> 8) & 255) * (255 - a)) / 255;
    uint32_t bl = ((c & 255) * a + (b & 255) * (255 - a)) / 255;
    *d = gfx_565((r << 16) | (g << 8) | bl);
}

static inline void cov_px(int x, int y, rgb_t c, float cov) {
    if (cov <= 0.0f) return;
    gfx_pixel_blend(x, y, c, (uint8_t)(clampf(cov, 0, 1) * 255.0f + 0.5f));
}

void gfx_fill_rect(int x0, int y0, int x1, int y1, rgb_t c) {
    uint16_t p = gfx_565(c);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GFX_W) x1 = GFX_W;
    if (y1 > GFX_H) y1 = GFX_H;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) s_fb[y * GFX_W + x] = p;
}

/* signed distance to rounded rectangle (negative inside) */
static float sd_rrect(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float outside = sqrtf(ox * ox + oy * oy);
    float inside = fminf(fmaxf(qx, qy), 0.0f);
    return outside + inside - r;
}

void gfx_round_rect(float x0, float y0, float x1, float y1, float r, rgb_t fill, bool has_fill,
                    rgb_t outline, float ow) {
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hw = (x1 - x0) / 2, hh = (y1 - y0) / 2;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    int ix0 = (int)floorf(x0) - 1, iy0 = (int)floorf(y0) - 1, ix1 = (int)ceilf(x1) + 1, iy1 = (int)ceilf(y1) + 1;
    for (int y = iy0; y <= iy1; y++)
        for (int x = ix0; x <= ix1; x++) {
            float d = sd_rrect(x + 0.5f, y + 0.5f, cx, cy, hw, hh, r);
            if (has_fill) cov_px(x, y, fill, 0.5f - d);
            if (ow > 0) cov_px(x, y, outline, clampf(0.5f - d, 0, 1) * clampf(0.5f + d + ow, 0, 1));
        }
}

void gfx_circle(float cx, float cy, float r, rgb_t c) {
    int ix0 = (int)floorf(cx - r) - 1, ix1 = (int)ceilf(cx + r) + 1;
    int iy0 = (int)floorf(cy - r) - 1, iy1 = (int)ceilf(cy + r) + 1;
    for (int y = iy0; y <= iy1; y++)
        for (int x = ix0; x <= ix1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            cov_px(x, y, c, r + 0.5f - sqrtf(dx * dx + dy * dy));
        }
}

static float norm_deg(float a) { a = fmodf(a, 360.0f); return a < 0 ? a + 360.0f : a; }

void gfx_arc(float r_out, float width, float a0, float a1, rgb_t c, bool round_caps) {
    gfx_arc_at(GFX_W / 2.0f, GFX_H / 2.0f, r_out, width, a0, a1, c, round_caps);
}

void gfx_arc_at(float cx, float cy, float r_out, float width, float a0, float a1, rgb_t c, bool round_caps) {
    float r_in = r_out - width;
    float sweep = a1 - a0;
    bool full = sweep >= 360.0f;
    float s0 = norm_deg(a0);
    int y0 = (int)floorf(cy - r_out) - 1, y1 = (int)ceilf(cy + r_out) + 1;
    for (int y = y0; y <= y1; y++) {
        float dy = y + 0.5f - cy;
        float ro2 = (r_out + 1) * (r_out + 1) - dy * dy;
        if (ro2 < 0) continue;
        float xo = sqrtf(ro2);
        float ri2 = (r_in - 1) * (r_in - 1) - dy * dy;
        float xi = ri2 > 0 ? sqrtf(ri2) : 0;
        /* two spans: [cx-xo, cx-xi] and [cx+xi, cx+xo] */
        for (int span = 0; span < 2; span++) {
            int xa = span == 0 ? (int)floorf(cx - xo) : (int)floorf(cx + xi);
            int xb = span == 0 ? (int)ceilf(cx - xi) : (int)ceilf(cx + xo);
            if (xi == 0) xb = (int)ceilf(cx + xo);   /* row misses inner circle: one full span */
            for (int x = xa; x <= xb; x++) {
                float dx = x + 0.5f - cx;
                float d = sqrtf(dx * dx + dy * dy);
                float radial = fminf(d - r_in, r_out - d) + 0.5f;
                if (radial <= 0) continue;
                float cov = clampf(radial, 0, 1);
                if (!full) {
                    float ang = norm_deg(atan2f(dy, dx) * 57.29578f);
                    float rel = norm_deg(ang - s0);
                    /* angular distance in px to the nearest edge (positive inside) */
                    float inside_deg = (rel <= sweep) ? fminf(rel, sweep - rel)
                                                      : -fminf(rel - sweep, 360.0f - rel);
                    float ang_px = inside_deg * 0.0174533f * d;
                    cov *= clampf(ang_px + 0.5f, 0, 1);
                }
                cov_px(x, y, c, cov);
            }
            if (xi == 0) break; /* spans merged */
        }
    }
    if (round_caps && !full) {
        float rr = r_out - width / 2;
        for (int k = 0; k < 2; k++) {
            float a = (k ? a1 : a0) * 0.0174533f;
            gfx_circle(cx + rr * cosf(a), cy + rr * sinf(a), width / 2, c);
        }
    }
}

void gfx_line(float x0, float y0, float x1, float y1, float w, rgb_t c) {
    float r = w / 2;
    int ix0 = (int)floorf(fminf(x0, x1) - r) - 1, ix1 = (int)ceilf(fmaxf(x0, x1) + r) + 1;
    int iy0 = (int)floorf(fminf(y0, y1) - r) - 1, iy1 = (int)ceilf(fmaxf(y0, y1) + r) + 1;
    float vx = x1 - x0, vy = y1 - y0, L2 = vx * vx + vy * vy;
    for (int y = iy0; y <= iy1; y++)
        for (int x = ix0; x <= ix1; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = L2 > 0 ? clampf((px * vx + py * vy) / L2, 0, 1) : 0;
            float dx = px - t * vx, dy = py - t * vy;
            cov_px(x, y, c, r + 0.5f - sqrtf(dx * dx + dy * dy));
        }
}

void gfx_triangle(float x0, float y0, float x1, float y1, float x2, float y2, rgb_t c) {
    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area < 0) { float t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    float ex[3] = {x0, x1, x2}, ey[3] = {y0, y1, y2};
    int ix0 = (int)floorf(fminf(x0, fminf(x1, x2))) - 1, ix1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2))) + 1;
    int iy0 = (int)floorf(fminf(y0, fminf(y1, y2))) - 1, iy1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2))) + 1;
    for (int y = iy0; y <= iy1; y++)
        for (int x = ix0; x <= ix1; x++) {
            float px = x + 0.5f, py = y + 0.5f, dmin = 1e9f;
            for (int i = 0; i < 3; i++) {
                int j = (i + 1) % 3;
                float vx = ex[j] - ex[i], vy = ey[j] - ey[i];
                float len = sqrtf(vx * vx + vy * vy);
                float d = ((px - ex[i]) * vy - (py - ey[i]) * vx) / len; /* >0 outside for CCW? */
                d = -d;
                if (d < dmin) dmin = d;
            }
            cov_px(x, y, c, dmin + 0.5f);
        }
}

/* ---------------- text ---------------- */
static const glyph_t *find_glyph(const font_t *f, char ch, int *idx) {
    for (int i = 0; i < f->count; i++)
        if (f->chars[i] == ch) { if (idx) *idx = i; return &f->glyphs[i]; }
    return 0;
}

float gfx_text_width(const font_t *f, const char *s, float tracking) {
    float w = 0; int n = 0;
    for (const char *p = s; *p; p++) {
        const glyph_t *g = find_glyph(f, *p, 0);
        if (!g) continue;
        w += g->adv16 / 16.0f; n++;
    }
    if (n > 1) w += tracking * (n - 1);
    return w;
}

static void blit_glyph(const font_t *f, const glyph_t *g, int px, int py, rgb_t c) {
    int rb = (g->w + 1) / 2;
    const uint8_t *b = f->bits + g->offset;
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++) {
            uint8_t byte = b[y * rb + x / 2];
            uint8_t a4 = (x & 1) ? (byte & 15) : (byte >> 4);
            if (a4) gfx_pixel_blend(px + g->x0 + x, py + g->y0 + y, c, (uint8_t)(a4 * 17));
        }
}

float gfx_text(const font_t *f, float x, float y, const char *s, rgb_t c, gfx_align_t al,
               bool valign_mid, float tracking) {
    float w = gfx_text_width(f, s, tracking);
    float pen = al == ALIGN_L ? x : (al == ALIGN_C ? x - w / 2 : x - w);
    float base = y;
    if (valign_mid) {
        /* same as PIL anchor 'm': halfway between font ascender and descender */
        base = y + (f->ascent - f->descent) / 2.0f;
    }
    for (const char *p = s; *p; p++) {
        const glyph_t *g = find_glyph(f, *p, 0);
        if (!g) continue;
        if (g->w) blit_glyph(f, g, (int)floorf(pen + 0.5f), (int)floorf(base + 0.5f), c);
        pen += g->adv16 / 16.0f + tracking;
    }
    return w;
}

void gfx_mask_round(void) {
    const float cx = GFX_W / 2.0f, cy = GFX_H / 2.0f, r = 120.0f;
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > r + 0.5f) s_fb[y * GFX_W + x] = 0;
            else if (d > r - 0.5f) {
                rgb_t o = gfx_888(s_fb[y * GFX_W + x]);
                float k = r + 0.5f - d;
                uint32_t R = (uint32_t)(((o >> 16) & 255) * k), G = (uint32_t)(((o >> 8) & 255) * k), B = (uint32_t)((o & 255) * k);
                s_fb[y * GFX_W + x] = gfx_565((R << 16) | (G << 8) | B);
            }
        }
}
