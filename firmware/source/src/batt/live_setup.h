/* Builds a live SMBus target exactly as the firmware arms it: register map (live_prepare), 12 Pro evidence read gate
 * and selector pages, then the wire-path hooks. Pure (no Pico peripheral): battery_bus.c calls it on the device and the
 * host replay tests call the same code (review 105 R3: the replay must not rebuild the hook setup by hand).
 * The caller owns st_init() and the guard; the hooks are installed only after a successful preparation. */
#ifndef CHIPLER_LIVE_SETUP_H
#define CHIPLER_LIVE_SETUP_H
#include <stdbool.h>
#include "smbus_target.h"
#include "auth_gate.h"
#include "selector_pages.h"
#include "profile.h"

typedef struct {
    sp_map_t pages;   /* first member: the selector hooks receive the hook ctx as an sp_map_t * */
    ag_gate_t auth;
} live_setup_t;

/* false = target not ready (nothing armed). *evidence = the profile serves a 12 Pro capture (hooks installed). */
bool live_setup_prepare(live_setup_t *s, st_target_t *t, const batt_profile_t *p, int soc, bool *evidence);
#endif
