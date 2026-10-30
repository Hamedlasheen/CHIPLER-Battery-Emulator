#include "live_setup.h"
#include "live_prepare.h"
#include "ds12p_config.h"
#include <stddef.h>
#if CHIPLER_12P_REFERENCE
#include "ds12p_evidence.h"
#include "ds12p_gate_power.inc"
#include "ds12p_pages_power.inc"
#include "ds12p_gate_charger.inc"
#include "ds12p_pages_charger.inc"
#endif

#ifndef TE_RAMFUNC
#define TE_RAMFUNC
#endif
#if CHIPLER_12P_REFERENCE
_Static_assert(offsetof(live_setup_t, pages) == 0, "selector hooks take the hook ctx as sp_map_t *");
/* R3 + ISR-RAM rule: these run in the wire path (called from st_begin_read/st_on_stop),
 * so their code lives in SRAM (TE_RAMFUNC) and the gate rows were copied to RAM by
 * ag_init; the read path must not fetch code or row data from XIP flash. */
TE_RAMFUNC static bool ls_allow_read(void *ctx, uint8_t cmd) { return ag_may_serve(&((live_setup_t *)ctx)->auth, cmd); }
static const st_hooks_t s_ls_hooks = {ls_allow_read, sp_is_target_hook, sp_read_hook, sp_select_hook};
#endif

bool live_setup_prepare(live_setup_t *s, st_target_t *t, const batt_profile_t *p, int soc, bool *evidence) {
    bool evid = false;
#if CHIPLER_12P_REFERENCE
    int cap = profile_12p_capture_state(p);
    evid = cap != 0;
    if (cap == 1) {
        ag_init(&s->auth, DS12P_GATE_POWER_ROWS, sizeof DS12P_GATE_POWER_ROWS / sizeof DS12P_GATE_POWER_ROWS[0], true);
        sp_init(&s->pages, DS12P_PAGES_POWER, sizeof DS12P_PAGES_POWER / sizeof DS12P_PAGES_POWER[0], true);
    } else if (evid) {
        ag_init(&s->auth, DS12P_GATE_CHARGER_ROWS, sizeof DS12P_GATE_CHARGER_ROWS / sizeof DS12P_GATE_CHARGER_ROWS[0], true);
        sp_init(&s->pages, DS12P_PAGES_CHARGER, sizeof DS12P_PAGES_CHARGER / sizeof DS12P_PAGES_CHARGER[0], true);
    } else
#endif
    {
        ag_init(&s->auth, 0, 0, false);
        sp_init(&s->pages, 0, 0, false);
    }
    bool ok = live_prepare(t, p, soc) && st_live_ready(t);
#if CHIPLER_12P_REFERENCE
    /* R3: hooks are installed before activate(); selector state then lives in the wire path. */
    if (ok && evid) st_set_hooks(t, &s_ls_hooks, s);
#endif
    if (evidence) *evidence = evid;
    return ok;
}
