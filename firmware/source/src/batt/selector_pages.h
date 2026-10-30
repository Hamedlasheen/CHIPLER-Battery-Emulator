/* DEEPSEEK AUTH-GATE 082 — selector-page responses from recorded evidence (v2).
 *
 * v2 fixes Codex DS081 R1/R2/R3:
 *   R1  sp_on_write() reports WHICH selector was written and sp_last_target() names the
 *       block command to republish, so a 0x73 write can never be mis-applied to 0x66.
 *   R2  sp_read_hook() refuses a page-target read when no valid selector page is latched
 *       (and after an unknown page) instead of falling back to a stale/static payload.
 *   R3  all functions used by the wire path are marked TE_RAMFUNC (SRAM on target) and are
 *       called from the SMBus target's write-commit / begin-read path, i.e. inside the same
 *       critical section as the transaction, not from the diagnostic drain.
 *
 * The module only ever returns payloads taken from the capture evidence rows; it never
 * invents a page and never infers one.
 */
#ifndef CHIPLER_SELECTOR_PAGES_H
#define CHIPLER_SELECTOR_PAGES_H

#include <stdint.h>
#include <stdbool.h>
#include "ds12p_evidence.h"

#ifndef TE_RAMFUNC
#define TE_RAMFUNC
#endif

typedef enum {
    SP_WRITE_NOT_SELECTOR = 0,   /* command is not a selector we model */
    SP_WRITE_APPLIED = 1,        /* recorded page latched (sp_last_target() valid) */
    SP_WRITE_UNKNOWN_PAGE = 2    /* selector written but that page has no evidence row */
} sp_write_result_t;

typedef struct {
    const ds_page_row_t *rows;
    unsigned n_rows;
    bool serve_dynamic;
    bool has_65, has_73;
    uint8_t page_65, page_73;
    uint8_t last_target;         /* 0x66 / 0x74 after the last APPLIED selector, else 0 */
    /* Selected page payload is copied here inside sp_on_write (single flash read at the
     * selector write, IRQ context) so the page READ path touches SRAM only. */
    uint8_t page65_data[33];
    uint8_t page73_data[33];
    uint8_t page65_len, page73_len;
    uint32_t unknown_pages;
    uint32_t applied;
} sp_map_t;

void sp_init(sp_map_t *m, const ds_page_row_t *rows, unsigned n_rows, bool serve_dynamic);

/* Is this command a selector command handled by this map (0x65/0x73)? */
bool sp_is_selector(uint8_t cmd);
/* Is this command a page target (0x66/0x74)? */
bool sp_is_page_target(uint8_t cmd);

/* Latch the page carried by a queued write body (body = bytes after command, before PEC). */
TE_RAMFUNC sp_write_result_t sp_on_write(sp_map_t *m, uint8_t cmd, const uint8_t *body, uint8_t body_len);

/* Payload currently selected for `target_cmd`, or false when nothing valid is latched. */
TE_RAMFUNC bool sp_lookup(sp_map_t *m, uint8_t target_cmd, const uint8_t **data, uint8_t *count,
                          const char **row_id);

/* ---- hooks passed to the SMBus target (wire path) ---- */
/* page_select: called from commit_write after PEC validation; returns true when the write
 * was a selector. State changes here, before the next START can be decoded. */
TE_RAMFUNC bool sp_select_hook(void *ctx, uint8_t cmd, const uint8_t *body, uint8_t len);
/* is_target: true for 0x66/0x74. */
TE_RAMFUNC bool sp_is_target_hook(void *ctx, uint8_t cmd);
/* page_read: return true with data/count when the current page is valid; false => refuse
 * the read (no bytes served, no stale page). */
TE_RAMFUNC bool sp_read_hook(void *ctx, uint8_t cmd, const uint8_t **data, uint8_t *count);
/* last republished target after an applied selector (0x66/0x74, else 0) - diagnostics/tests. */
TE_RAMFUNC uint8_t sp_last_target(const sp_map_t *m);

#endif
