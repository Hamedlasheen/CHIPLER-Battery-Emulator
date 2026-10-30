/* DEEPSEEK AUTH-GATE 081 — evidence gate for the battery-side SMBus target.
 *
 * Purpose (owner package 1+2): the emulator may only serve response bytes that are
 * backed by recorded evidence, and it must never report an authentication result it
 * does not have.
 *
 *   - `ag_may_serve()` is the single place that decides whether a read command may be
 *     answered. A command without an evidence row is refused (the wire layer already
 *     returns ST_UNSUPPORTED and no data bytes for unregistered commands).
 *   - DYNAMIC rows are payloads that CHANGED inside the capture (e.g. 0x86, 0x66 page 0);
 *     they are a recorded reference snapshot, never a live measurement. They are served
 *     only when the owner enables `serve_dynamic`.
 *   - `ag_auth_state()` is always "UNVERIFIED" unless a row whose provenance id starts
 *     with AUTH-EVIDENCE/ exists AND `allow_auth_rows` was explicitly enabled. No such
 *     row exists today: the captures contain no authentication exchange, and this module
 *     must not fabricate one.
 *
 * No dynamic allocation, no libc dependency beyond the caller's stack; safe for host
 * tests and for the main-loop (non-IRQ) context of the RP2350 firmware.
 */
#ifndef CHIPLER_AUTH_GATE_H
#define CHIPLER_AUTH_GATE_H

#include <stdint.h>
#include <stdbool.h>

#define AG_ROW_PREFIX_AUTH "AUTH-EVIDENCE/"

typedef enum {
    AG_ROW_STATIC = 0,   /* identical payload in every PEC-valid capture instance */
    AG_ROW_DYNAMIC = 1   /* payload changes inside the capture (recorded reference only) */
} ag_row_class_t;

#ifndef TE_RAMFUNC
#define TE_RAMFUNC   /* IRQ-path functions: SRAM section on the target, empty on host */
#endif
#define AG_MAX_ROWS 56u

typedef struct {
    uint8_t cmd;
    ag_row_class_t cls;
    const char *row_id;  /* provenance: capture / transaction id / timestamp */
} ag_row_t;

typedef enum {
    AG_AUTH_UNVERIFIED = 0,     /* no authentication exchange identified in the captures -> never claimed */
    AG_AUTH_EVIDENCE_PRESENT    /* only possible with a real AUTH-EVIDENCE/ row */
} ag_auth_state_t;

typedef struct {
    /* Rows are copied into this RAM cache by ag_init (main-loop context) so the read path
     * executed from the I2C IRQ never has to fetch row metadata from XIP flash. */
    ag_row_t rows_ram[AG_MAX_ROWS];
    const ag_row_t *rows;
    unsigned n_rows;
    bool serve_dynamic;         /* serve recorded DYNAMIC rows (reference snapshot) */
    bool allow_auth_rows;        /* must be enabled explicitly; false in every shipped profile */
    bool has_auth_evidence;
    const char *auth_evidence_id;
    uint32_t last_unknown_reads;
    uint32_t refused_reads;
    uint32_t refused_writes;
    uint32_t dynamic_refusals;
    uint8_t last_refused_cmd;
    bool have_last_refused;
} ag_gate_t;

/* rows may be NULL/0 (gate then refuses every command). */
void ag_init(ag_gate_t *g, const ag_row_t *rows, unsigned n_rows, bool serve_dynamic);

/* Does any evidence row exist for this command? */
bool ag_has_evidence(const ag_gate_t *g, uint8_t cmd);

/* May this command be answered under the current policy? */
TE_RAMFUNC bool ag_may_serve(const ag_gate_t *g, uint8_t cmd);

/* Record a refusal (called by the main loop when the wire layer reports an unsupported
 * command; the IRQ path itself stays untouched). */
void ag_note_refused_read(ag_gate_t *g, uint8_t cmd);
void ag_note_refused_write(ag_gate_t *g, uint8_t cmd);

/* Poll the wire layer's monotonic unknown-read counter. Adds the delta to refused_reads
 * and remembers the last command byte. Safe to call every control cycle. */
void ag_poll_counters(ag_gate_t *g, uint32_t unknown_reads_total, uint8_t last_cmd);

ag_auth_state_t ag_auth_state(const ag_gate_t *g);
/* Explicit opt-in for AUTH-EVIDENCE/ rows. The shipped profiles never enable it; the
 * host test enables it only to prove the state machine, with a row marked "test-only". */
void ag_set_allow_auth_rows(ag_gate_t *g, bool allow);
/* "AUTH=UNVERIFIED(no_authentication_exchange_identified)" - suitable for the STAT log line. */
const char *ag_auth_state_str(const ag_gate_t *g);
/* "STATIC" / "DYNAMIC" / "NONE" for a command; diagnostic only. */
const char *ag_row_class_str(const ag_gate_t *g, uint8_t cmd);
/* Number of commands the gate may serve right now (0..n_rows). */
unsigned ag_servable_count(const ag_gate_t *g);

#endif
