/* DEEPSEEK AUTH-GATE 081 — see auth_gate.h for the policy. */
#include "auth_gate.h"

static bool prefix_match(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

TE_RAMFUNC static const ag_row_t *find_row(const ag_gate_t *g, uint8_t cmd) {
    if (!g || !g->rows) return 0;
    for (unsigned k = 0; k < g->n_rows; k++) {
        if (g->rows[k].cmd == cmd) return &g->rows[k];
    }
    return 0;
}

void ag_init(ag_gate_t *g, const ag_row_t *rows, unsigned n_rows, bool serve_dynamic) {
    if (!g) return;
    unsigned n = rows ? n_rows : 0;
    if (n > AG_MAX_ROWS) n = AG_MAX_ROWS;
    for (unsigned k = 0; k < n; k++) g->rows_ram[k] = rows[k];
    g->rows = n ? g->rows_ram : 0;   /* RAM cache: IRQ read path never touches XIP rows */
    g->n_rows = n;
    g->serve_dynamic = serve_dynamic;
    g->allow_auth_rows = false;
    g->has_auth_evidence = false;
    g->auth_evidence_id = 0;
    g->last_unknown_reads = 0;
    g->refused_reads = 0;
    g->refused_writes = 0;
    g->dynamic_refusals = 0;
    g->last_refused_cmd = 0;
    g->have_last_refused = false;
    /* Scan once: an authentication row only counts when its provenance id says so. */
    for (unsigned k = 0; k < g->n_rows; k++) {
        const char *id = g->rows[k].row_id;
        if (prefix_match(id, AG_ROW_PREFIX_AUTH)) {
            g->has_auth_evidence = true;
            g->auth_evidence_id = id;
        }
    }
}

bool ag_has_evidence(const ag_gate_t *g, uint8_t cmd) { return find_row(g, cmd) != 0; }

TE_RAMFUNC bool ag_may_serve(const ag_gate_t *g, uint8_t cmd) {
    const ag_row_t *r = find_row(g, cmd);
    if (!r) return false;
    if (r->cls == AG_ROW_STATIC) return true;
    return g->serve_dynamic;
}

static void note_refusal(ag_gate_t *g, uint8_t cmd) {
    if (!g) return;
    g->last_refused_cmd = cmd;
    g->have_last_refused = true;
}

void ag_note_refused_read(ag_gate_t *g, uint8_t cmd) {
    if (!g) return;
    g->refused_reads++;
    note_refusal(g, cmd);
}

void ag_note_refused_write(ag_gate_t *g, uint8_t cmd) {
    if (!g) return;
    g->refused_writes++;
    note_refusal(g, cmd);
}

void ag_poll_counters(ag_gate_t *g, uint32_t unknown_reads_total, uint8_t last_cmd) {
    if (!g) return;
    if (unknown_reads_total > g->last_unknown_reads) {
        uint32_t delta = unknown_reads_total - g->last_unknown_reads;
        g->refused_reads += delta;
        note_refusal(g, last_cmd);
    }
    g->last_unknown_reads = unknown_reads_total;
}

ag_auth_state_t ag_auth_state(const ag_gate_t *g) {
    if (g && g->has_auth_evidence && g->allow_auth_rows) return AG_AUTH_EVIDENCE_PRESENT;
    return AG_AUTH_UNVERIFIED;
}

void ag_set_allow_auth_rows(ag_gate_t *g, bool allow) {
    if (g) g->allow_auth_rows = allow;
}

const char *ag_auth_state_str(const ag_gate_t *g) {
    if (ag_auth_state(g) == AG_AUTH_EVIDENCE_PRESENT) return "AUTH=EVIDENCE_PRESENT";
    return "AUTH=UNVERIFIED(no_authentication_exchange_identified)";
}

const char *ag_row_class_str(const ag_gate_t *g, uint8_t cmd) {
    const ag_row_t *r = find_row(g, cmd);
    if (!r) return "NONE";
    return r->cls == AG_ROW_STATIC ? "STATIC" : "DYNAMIC";
}

unsigned ag_servable_count(const ag_gate_t *g) {
    if (!g) return 0;
    unsigned n = 0;
    for (unsigned k = 0; k < g->n_rows; k++) {
        if (g->rows[k].cls == AG_ROW_STATIC || g->serve_dynamic) n++;
    }
    return n;
}
