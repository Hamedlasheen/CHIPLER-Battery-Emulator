#ifndef CHIPLER_FONT_H
#define CHIPLER_FONT_H
#include <stdint.h>

/* One glyph of a 4-bpp anti-aliased bitmap font. x0/y0 are offsets of the bitmap's
 * top-left corner from the pen position on the baseline (y0 negative = above). */
typedef struct {
    uint32_t offset;   /* byte offset into bitmap array */
    uint8_t  w, h;
    int8_t   x0, y0;
    uint16_t adv16;    /* advance in 1/16 px (tabular figures) */
} glyph_t;

typedef struct {
    const uint8_t *bits;
    const glyph_t *glyphs;
    const char    *chars;   /* characters present, same order as glyphs */
    uint16_t       count;
    uint8_t        ascent, descent;
} font_t;

#endif
