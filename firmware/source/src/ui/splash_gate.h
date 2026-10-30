/* Cross-core splash coordination (063 R1). Core 1 acknowledges that the SCR_STARTUP frame was pushed to the panel; core 0 starts the
 * hold timer from that acknowledgement (or from a fallback timeout) and then leaves the splash. atomic_bool with release/acquire is the
 * inter-core synchronization; the gate is initialised before core 1 is launched. No hardware is enabled by the splash. */
#ifndef CHIPLER_SPLASH_GATE_H
#define CHIPLER_SPLASH_GATE_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#define SPLASH_HOLD_MS 2500u
#define SPLASH_FALLBACK_MS 8000u
typedef struct { atomic_bool on_panel; uint32_t t0; } splash_gate_t;
static inline void splash_gate_init(splash_gate_t *g) { atomic_init(&g->on_panel, false); g->t0 = 0; }
/* core 1, after each lcd_push_frame_dirty(): only a completed push that moved rows, while the startup screen is shown, acknowledges */
static inline void splash_gate_on_push(splash_gate_t *g, bool screen_is_startup, uint32_t rows_pushed, bool push_ok) {
    if (screen_is_startup && rows_pushed > 0 && push_ok) atomic_store_explicit(&g->on_panel, true, memory_order_release);
}
/* core 0, every loop pass while the startup screen is up: true when the splash may be left */
static inline bool splash_gate_leave(splash_gate_t *g, uint32_t now_ms) {
    if (!g->t0) {
        if (atomic_load_explicit(&g->on_panel, memory_order_acquire) || now_ms > SPLASH_FALLBACK_MS) g->t0 = now_ms ? now_ms : 1u;
        return false;
    }
    return (uint32_t)(now_ms - g->t0) >= SPLASH_HOLD_MS;
}
#endif
