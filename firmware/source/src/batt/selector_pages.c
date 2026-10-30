/* DEEPSEEK AUTH-GATE 082 — selector pages, wire-path implementation (see header). */
#include "selector_pages.h"

bool sp_is_selector(uint8_t cmd) { return cmd == 0x65 || cmd == 0x73; }
bool sp_is_page_target(uint8_t cmd) { return cmd == 0x66 || cmd == 0x74; }

void sp_init(sp_map_t *m, const ds_page_row_t *rows, unsigned n_rows, bool serve_dynamic) {
    if (!m) return;
    m->rows = rows ? rows : 0;
    m->n_rows = rows ? n_rows : 0;
    m->serve_dynamic = serve_dynamic;
    m->has_65 = false;
    m->has_73 = false;
    m->page_65 = 0;
    m->page_73 = 0;
    m->last_target = 0;
    m->page65_len = 0;
    m->page73_len = 0;
    m->unknown_pages = 0;
    m->applied = 0;
}

TE_RAMFUNC static const ds_page_row_t *find_page(const sp_map_t *m, uint8_t sel_cmd, uint8_t page) {
    for (unsigned k = 0; k < m->n_rows; k++) {
        if (m->rows[k].sel_cmd == sel_cmd && m->rows[k].page == page) return &m->rows[k];
    }
    return 0;
}

TE_RAMFUNC sp_write_result_t sp_on_write(sp_map_t *m, uint8_t cmd, const uint8_t *body, uint8_t body_len) {
    if (!m || !sp_is_selector(cmd)) return SP_WRITE_NOT_SELECTOR;
    /* R1: dispatch strictly by the selector that was written. */
    uint8_t page = (body && body_len >= 1) ? body[0] : 0xFF;
    const ds_page_row_t *row = find_page(m, cmd, page);
    uint8_t target = (cmd == 0x65) ? 0x66 : 0x74;
    if (!row) {
        /* R2: an unknown page invalidates the state for ITS OWN target only; the other
         * selector keeps its own page. Nothing stale survives for this target. */
        m->unknown_pages++;
        if (cmd == 0x65) { m->has_65 = false; m->page_65 = 0; m->page65_len = 0; }
        else             { m->has_73 = false; m->page_73 = 0; m->page73_len = 0; }
        if (m->last_target == target) m->last_target = 0;
        return SP_WRITE_UNKNOWN_PAGE;
    }
    uint8_t n = row->count;
    if (n > 32) n = 32;
    if (cmd == 0x65) {
        m->page_65 = page; m->has_65 = true;
        for (uint8_t k = 0; k < n; k++) m->page65_data[k] = row->data[k];
        m->page65_len = n;
    } else {
        m->page_73 = page; m->has_73 = true;
        for (uint8_t k = 0; k < n; k++) m->page73_data[k] = row->data[k];
        m->page73_len = n;
    }
    m->last_target = target;
    m->applied++;
    return SP_WRITE_APPLIED;
}

TE_RAMFUNC bool sp_lookup(sp_map_t *m, uint8_t target_cmd, const uint8_t **data, uint8_t *count,
                          const char **row_id) {
    if (!m || !data || !count) return false;
    uint8_t page = 0;
    bool have = false;
    if (target_cmd == 0x66 && m->has_65) { page = m->page_65; have = true; }
    else if (target_cmd == 0x74 && m->has_73) { page = m->page_73; have = true; }
    if (!have) return false;
    const ds_page_row_t *r = find_page(m, target_cmd == 0x66 ? 0x65 : 0x73, page);
    if (!r || r->target_cmd != target_cmd) return false;
    if (r->cls != DS_EV_STATIC && !m->serve_dynamic) return false;
    *data = r->data;
    *count = r->count;
    if (row_id) *row_id = r->row_id;
    return true;
}

TE_RAMFUNC bool sp_select_hook(void *ctx, uint8_t cmd, const uint8_t *body, uint8_t len) {
    sp_map_t *m = (sp_map_t *)ctx;
    if (!m || !sp_is_selector(cmd)) return false;
    (void)sp_on_write(m, cmd, body, len);   /* state updated here, in the write path */
    return true;
}

TE_RAMFUNC bool sp_is_target_hook(void *ctx, uint8_t cmd) {
    (void)ctx;
    return sp_is_page_target(cmd);
}

TE_RAMFUNC bool sp_read_hook(void *ctx, uint8_t cmd, const uint8_t **data, uint8_t *count) {
    sp_map_t *m = (sp_map_t *)ctx;
    if (!m || !sp_is_page_target(cmd)) return false;
    /* SRAM cache filled at selector time: no flash payload read on the read path. */
    if (cmd == 0x66) {
        if (!m->has_65 || !m->page65_len) return false;   /* R2: refuse, no stale page */
        *data = m->page65_data;
        *count = m->page65_len;
        return true;
    }
    if (!m->has_73 || !m->page73_len) return false;
    *data = m->page73_data;
    *count = m->page73_len;
    return true;
}

TE_RAMFUNC uint8_t sp_last_target(const sp_map_t *m) { return m ? m->last_target : 0; }
