/* Measurement freshness (Codex R-04). A reading counts as MEASURED only if:
 *   - the I2C read of the power monitor succeeded this cycle, AND
 *   - a NEW conversion completed (INA228 DIAG_ALRT.CNVRF) within TF_MAX_AGE_MS.
 * If conversions stop while I2C keeps answering, quality drops to ABSENT after TF_MAX_AGE_MS,
 * which refuses arming and trips FLT_NO_MONITOR if the output is ON.
 * Pure logic, host-tested. */
#ifndef CHIPLER_TELEM_FRESH_H
#define CHIPLER_TELEM_FRESH_H
#include <stdbool.h>
#include <stdint.h>

#define TF_MAX_AGE_MS 100u   /* nominal conversion period ~26 ms (16x avg, 540 us x3) */

typedef struct {
    bool have_sample;
    uint32_t last_new_ms;
    uint32_t read_failures, stale_events;
    bool was_fresh;
} telem_fresh_t;

static inline void tf_init(telem_fresh_t *t) {
    t->have_sample = false; t->last_new_ms = 0; t->read_failures = 0; t->stale_events = 0; t->was_fresh = false;
}
/* returns true if the current reading may be labelled MEASURED */
static inline bool tf_update(telem_fresh_t *t, uint32_t now, bool read_ok, bool new_conversion) {
    if (!read_ok) { t->read_failures++; t->was_fresh = false; return false; }
    if (new_conversion) { t->have_sample = true; t->last_new_ms = now; }
    bool fresh = t->have_sample && (uint32_t)(now - t->last_new_ms) <= TF_MAX_AGE_MS;
    if (!fresh && t->was_fresh) t->stale_events++;
    t->was_fresh = fresh;
    return fresh;
}
#endif
