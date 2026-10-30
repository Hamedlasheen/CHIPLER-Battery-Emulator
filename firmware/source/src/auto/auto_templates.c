/* CHIPLER AUTO: the runtime templates that exist on the board, built from the evidence tables already compiled in.
 *
 * T1 "12P-POWER-0B": the recorded iPhone 12 Pro POWER (no charger) capture - DS12P_POWER_ROWS (PEC-valid read rows,
 * sha256 66c7e61d...) + selector page targets 0x66/0x74 (served by the selector hook) + DS12P_WRITES shapes. This is
 * exactly the map the r7..r13 shared engine serves (profile P12P-C1); AUTO adds no byte to it.
 * There is deliberately NO template built from the internet research seed: its values are null and its runtime flag
 * is false (04_Protocol/CODEX_AUTO_LEARN_WEB_RESEARCH_2026-10-29); it is used by the PC analyzer only. */
#include "auto_templates.h"
#include <string.h>
#if CHIPLER_12P_REFERENCE
#include "../batt/ds12p_evidence.h"
#include "../batt/ds12p_evidence_power.inc"
#include "../batt/ds12p_writes.inc"
#endif

int auto_templates_build(auto_template_t *out, int max) {
    int n = 0;
#if CHIPLER_12P_REFERENCE
    if (max < 1) return 0;
    auto_template_t *t = &out[n++];
    memset(t, 0, sizeof *t);
    t->id = AUTO_T_12P_POWER; t->name = "12P-POWER-0B"; t->addr7 = 0x0B; t->runtime = true;
    for (unsigned k = 0; k < sizeof DS12P_POWER_ROWS / sizeof DS12P_POWER_ROWS[0]; k++) auto_tmpl_set(t->read, DS12P_POWER_ROWS[k].cmd);
    auto_tmpl_set(t->read, 0x66); auto_tmpl_set(t->read, 0x74);   /* selector page targets (hook-served after a selector write) */
    for (unsigned k = 0; k < sizeof DS12P_WRITES / sizeof DS12P_WRITES[0]; k++) auto_tmpl_set(t->write, DS12P_WRITES[k].cmd);
#else
    (void)out; (void)max;
#endif
    return n;
}
