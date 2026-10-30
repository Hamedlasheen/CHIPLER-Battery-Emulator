/* Landscape 320x240 touch UI (P1_ILI9341_TOUCH, revision P1_r1).
 *
 * One layout description drives BOTH drawing and hit-testing, so what is drawn is exactly what can be
 * tapped (tests/host/test_ui_ls.c checks bounds, minimum target size, overlap and label fit for every
 * screen and state).
 *
 * Touch results are navigation commands only. They are executed by core 0 through the same
 * ui_nav state machine as the physical key; core 1 never changes protocol state.
 */
#ifndef CHIPLER_UI_LS_H
#define CHIPLER_UI_LS_H
#include <stdbool.h>
#include <stdint.h>
#include "ui_model.h"

#define LS_W 320
#define LS_H 240
#define LS_MIN_TARGET_W 56
#define LS_MIN_TARGET_H 30
#define LS_MAX_REGIONS 24

typedef enum { LS_CMD_NONE = 0, LS_CMD_SELECT, LS_CMD_BACK, LS_CMD_STOP, LS_CMD_UP, LS_CMD_DOWN } ls_cmd_t;
typedef enum { LS_ST_NORMAL = 0, LS_ST_PRIMARY, LS_ST_DANGER, LS_ST_SAFE, LS_ST_LISTROW } ls_style_t;

typedef struct {
    int16_t x0, y0, x1, y1;          /* inclusive-exclusive */
    ls_cmd_t cmd;
    int16_t idx;                     /* meaning per screen, see nav_select() */
    const char *label;
    uint8_t style;
    bool highlight;                  /* current cursor (e.g. default CANCEL) */
} ls_region_t;

typedef struct { ls_region_t r[LS_MAX_REGIONS]; int n; } ls_layout_t;

/* Core-1 local touch information (never sent to core 0 except as commands). */
typedef struct {
    bool hw_enabled;                 /* a touch backend is built in */
    bool cal_valid;
    bool ever_pen;
    bool down, irq;
    uint16_t rx, ry, rz;
    int16_t sx, sy;
    uint32_t polls, presses, taps, rejected, spi_violations, rows_pushed;
    /* calibration procedure */
    int cal_step, cal_total;
    float cal_tx, cal_ty;
    uint32_t cal_left_ms;
    bool cal_fail;
} ls_touch_info_t;

void ui_ls_layout(const ui_model_t *m, ls_layout_t *out);
bool ui_ls_label_fits(const ls_region_t *r);   /* label drawn inside the region without clipping */
/* PHONE MODEL list: status text drawn right-aligned at UI_LS_MODEL_STATUS_X on each row (review 111: the name and the
 * status must not overlap; test_ui_ls checks every registry profile in each build). */
#define UI_LS_MODEL_STATUS_X 244
const char *ui_ls_model_status(const batt_profile_t *p);
bool ui_ls_model_row_fits(const batt_profile_t *p);
bool ui_ls_hit(const ls_layout_t *l, int x, int y, ls_region_t *out);
/* gfx must have been initialised with an LS_W x LS_H framebuffer (GFX_W=320, GFX_H=240). */
void ui_ls_render(const ui_model_t *m, const ls_touch_info_t *ti);
#endif
