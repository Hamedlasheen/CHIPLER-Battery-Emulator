/* Touch input layer (P1_r1): raw sample -> filter/debounce -> calibration -> press/tap/long events.
 * Pure logic, no hardware access, host-tested (tests/host/test_touch_core.c).
 *
 * Nothing here knows which controller sits on the module (marked HR2046 at U1). A backend (touch_hw.c, XPT2046-family command set,
 * candidate, NOT RUN) only has to deliver touch_raw_t samples. Events never reach the I2C IRQ path and never
 * change protocol state by themselves: they are mapped to navigation commands by ui_ls.c.
 */
#ifndef CHIPLER_TOUCH_CORE_H
#define CHIPLER_TOUCH_CORE_H
#include <stdbool.h>
#include <stdint.h>

#define TCH_PRESS_POLLS      3u     /* consecutive "pen down" polls before a press is accepted */
#define TCH_RELEASE_POLLS    3u     /* consecutive "pen up" polls before a release is accepted */
#define TCH_TAP_MAX_MS       800u   /* press longer than this is not a tap */
#define TCH_TAP_MOVE_PX      18     /* movement beyond this cancels the tap */
#define TCH_LONG_MS          1200u  /* stationary hold -> TCH_EV_LONG once */
#define TCH_RAW_MIN          40u    /* readings at the ADC rails are rejected */
#define TCH_RAW_MAX          4055u
#define TCH_OUTLIER_RAW      700    /* single-poll jump (raw counts) treated as noise */
#define TCH_CAL_MAX_ERR_PX   10.0f  /* calibration rejected when worst residual exceeds this */
#define TCH_CAL_MAX_POINTS   5
/* DS-TOUCH-12 additions (proposed patch; not present in frozen commit 385c87d) */
#define TCH_STUCK_MS         8000u  /* press held this long without release is force-released */
#define TCH_OUTLIER_RESYNC   3u     /* this many consecutive outlier samples are a real move, not noise */

typedef struct { uint16_t x, y, z; bool pen; } touch_raw_t;

typedef struct {
    float a, b, c, d, e, f;   /* sx = a*x + b*y + c ; sy = d*x + e*y + f   (raw -> screen of cal rotation) */
    uint16_t w, h;            /* screen size the calibration was taken on */
    uint8_t rot180;           /* rotation active when the points were taken */
    bool valid;
} touch_cal_t;

typedef struct { float x, y; } tch_pt_t;

typedef enum { TCH_EV_NONE = 0, TCH_EV_DOWN, TCH_EV_UP, TCH_EV_TAP, TCH_EV_LONG } tch_ev_kind_t;
typedef struct { tch_ev_kind_t kind; int16_t x, y; } tch_event_t;

typedef struct {
    touch_cal_t cal;
    uint8_t rot180;                       /* current display rotation */
    uint16_t w, h;
    /* filter */
    uint16_t hx[3], hy[3]; uint8_t nh;    /* last raw positions (median of 3) */
    /* debounce */
    uint8_t down_cnt, up_cnt; bool down;
    uint32_t down_at; int16_t x0, y0, x, y; bool moved, long_sent;
    /* diagnostics */
    uint16_t last_rx, last_ry, last_rz; int16_t last_sx, last_sy;
    uint16_t press_rx, press_ry;          /* median raw position at the accepted press (for calibration) */
    uint32_t polls, presses, taps, longs, rejected_raw, outliers;
    bool ever_pen;
    /* DS-TOUCH-12 diagnostics/state (proposed patch) */
    uint32_t stuck, bus_err, canceled, resync;
    uint8_t out_run;                      /* consecutive outlier samples */
    bool stuck_lock;                      /* ignore contact until a real pen-up (forced release) */
} touch_core_t;

void touch_core_init(touch_core_t *t, uint16_t w, uint16_t h);
void touch_core_set_cal(touch_core_t *t, const touch_cal_t *cal);
void touch_core_set_rotation(touch_core_t *t, uint8_t rot180);
/* DS-TOUCH-12: drop the current contact without emitting TAP/UP; ignore the rest of that contact
 * until a real pen-up. Call when the screen changes or the mapping/rotation changes under a contact. */
void touch_core_cancel(touch_core_t *t);
/* One poll (call every ~10 ms). Returns at most one event per call.
 * DS-TOUCH-12: raw == NULL means "read failed" and is NOT treated as "no touch". */
tch_event_t touch_core_feed(touch_core_t *t, uint32_t now_ms, const touch_raw_t *raw);
/* Map a raw reading to current-rotation screen pixels. False when uncalibrated. */
bool touch_core_map(const touch_core_t *t, uint16_t rx, uint16_t ry, int16_t *sx, int16_t *sy);

/* Least-squares affine fit through n >= 3 point pairs (raw -> screen). Exact for n == 3.
 * Returns false on degenerate input or when the worst residual exceeds TCH_CAL_MAX_ERR_PX. */
bool touch_cal_solve(const tch_pt_t *raw, const tch_pt_t *scr, unsigned n, uint16_t w, uint16_t h,
                     uint8_t rot180, touch_cal_t *out, float *worst_err);
/* Standard calibration target positions (inset from the edges). n = 3 or 5. */
void touch_cal_targets(unsigned n, uint16_t w, uint16_t h, tch_pt_t *out);

#endif
