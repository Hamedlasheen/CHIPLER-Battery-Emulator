/* Three-button decoder: UP / DOWN / OK (pure logic, host-testable). CR-003-r1.
 *
 * Rules (documented in docs/ar/02 and 03):
 *  - Per-button debounce: 20 ms stable level.
 *  - UP / DOWN: event on RELEASE if held 30 ms..600 ms. Held >= 600 ms -> auto-repeat every
 *    200 ms while held (for long lists); no extra event on release.
 *  - OK: OK_SHORT on RELEASE if held 30 ms..1.5 s. OK_LONG once at 1.5 s while held (global
 *    "output OFF"); nothing on release. An OK event therefore always needs a fresh press and
 *    release -> a held OK can never confirm the next screen (no hold-through).
 *  - Two or more buttons down at the same time: every involved press is cancelled (no event)
 *    until all buttons are released. Simultaneous presses never confirm anything.
 */
#ifndef CHIPLER_BUTTONS_H
#define CHIPLER_BUTTONS_H
#include <stdint.h>
#include <stdbool.h>

#define BTN_DEBOUNCE_MS     20u
#define BTN_SHORT_MIN_MS    30u
#define BTN_REPEAT_START_MS 600u
#define BTN_REPEAT_MS       200u
#define BTN_OK_LONG_MS      1500u

typedef enum { BTN_UP = 0, BTN_DOWN = 1, BTN_OK = 2, BTN_COUNT = 3 } btn_id_t;

typedef enum {
    BTN_EV_NONE = 0, BTN_EV_UP, BTN_EV_DOWN, BTN_EV_OK, BTN_EV_OK_LONG
} btn_event_t;

typedef struct {
    bool raw, stable;
    uint32_t raw_since, down_at, next_repeat;
    bool consumed;            /* this press must not produce any further event */
    bool repeating;
} btn_ch_t;

typedef struct {
    btn_ch_t ch[BTN_COUNT];
    bool chord_lock;          /* >1 button was down: swallow until all released */
} buttons_t;

void buttons_init(buttons_t *b, uint32_t now_ms);
/* Call every 1..10 ms with pressed states. Returns at most one event per call. */
btn_event_t buttons_update(buttons_t *b, uint32_t now_ms, bool up, bool down, bool ok);
const char *btn_event_name(btn_event_t e);
#endif
