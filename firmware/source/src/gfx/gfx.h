/* Minimal deterministic renderer for a 240x240 RGB565 framebuffer.
 * No dynamic allocation. Platform-independent (also builds on host for tests). */
#ifndef CHIPLER_GFX_H
#define CHIPLER_GFX_H
#include <stdint.h>
#include <stdbool.h>
#include "font.h"

#ifndef GFX_W
#define GFX_W 240
#endif
#ifndef GFX_H
#define GFX_H 240
#endif

typedef uint32_t rgb_t;               /* 0xRRGGBB */
#define RGB(h) ((rgb_t)(h))

typedef enum { ALIGN_L, ALIGN_C, ALIGN_R } gfx_align_t;

void gfx_init(uint16_t *fb);           /* fb: GFX_W*GFX_H RGB565 (native order) */
uint16_t *gfx_fb(void);
void gfx_clear(rgb_t c);
void gfx_pixel_blend(int x, int y, rgb_t c, uint8_t a /*0..255*/);
void gfx_fill_rect(int x0, int y0, int x1, int y1, rgb_t c);  /* inclusive-exclusive */
void gfx_round_rect(float x0, float y0, float x1, float y1, float r, rgb_t fill, bool has_fill,
                    rgb_t outline, float ow);
void gfx_circle(float cx, float cy, float r, rgb_t c);
/* Ring arc centred on the screen centre. Angles in degrees, 0 = 3 o'clock, clockwise.
 * r_out = outer radius, width = thickness. round_caps adds semicircle caps. */
void gfx_arc(float r_out, float width, float a0, float a1, rgb_t c, bool round_caps);
void gfx_arc_at(float cx, float cy, float r_out, float width, float a0, float a1, rgb_t c, bool round_caps);
void gfx_line(float x0, float y0, float x1, float y1, float w, rgb_t c);
void gfx_triangle(float x0, float y0, float x1, float y1, float x2, float y2, rgb_t c);

/* Text. y is the vertical CENTRE of the cap height when valign_mid, else baseline.
 * tracking in px (letter spacing). Returns rendered width in px. */
float gfx_text_width(const font_t *f, const char *s, float tracking);
float gfx_text(const font_t *f, float x, float y, const char *s, rgb_t c, gfx_align_t al,
               bool valign_mid, float tracking);

/* Round-panel mask: paint outside of r=120 black (matches invisible corners). */
void gfx_mask_round(void);

static inline uint16_t gfx_565(rgb_t c) {
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}
static inline rgb_t gfx_888(uint16_t p) {
    uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}
#endif
