/* DEEPSEEK AUTH-GATE 081 — host unit tests (MSVC / any C11 compiler).
 *
 * The tests are deliberately not just "does the code compile":
 *   - every generated read/page row must carry a PEC that an INDEPENDENT CRC here
 *     recomputes from the recorded bytes (guards against a corrupted generator),
 *   - the gate table and the response table must contain exactly the same command set
 *     (a response can never be served without an evidence row, and vice versa),
 *   - the gate must refuse commands with no evidence (0x00, 0x03, 0x23, 0x2F, 0x61,
 *     0x05, 0x9A) and must report AUTH=UNVERIFIED,
 *   - DYNAMIC rows are refused unless reference serving is enabled,
 *   - selector pages replay only recorded pages and only after the selector write.
 */
#include <stdio.h>
#include <string.h>

#include "auth_gate.h"
#include "selector_pages.h"
#include "ds12p_evidence.h"
#include "ds12p_evidence_power.inc"
#include "ds12p_evidence_charger.inc"
#include "ds12p_gate_power.inc"
#include "ds12p_gate_charger.inc"
#include "ds12p_pages_power.inc"
#include "ds12p_pages_charger.inc"
#include "ds12p_writes.inc"

static int g_checks, g_fail;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static uint8_t crc8(const uint8_t *p, unsigned n) {
    uint8_t c = 0;
    for (unsigned i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (uint8_t)((c << 1) ^ ((c & 0x80u) ? 0x07u : 0u));
    }
    return c;
}

static uint8_t pec_read_row(uint8_t cmd, const ds_ev_row_t *r) {
    uint8_t buf[40];
    unsigned n = 0;
    buf[n++] = (uint8_t)(0x0B << 1);        /* write address */
    buf[n++] = cmd;
    buf[n++] = (uint8_t)((0x0B << 1) | 1);  /* read address */
    if (r->shape == DS_EV_BLOCK) buf[n++] = r->count;   /* count byte is inside the PEC */
    for (unsigned k = 0; k < r->count; k++) buf[n++] = r->data[k];
    return crc8(buf, n);
}

static uint8_t pec_page_row(const ds_page_row_t *r) {
    uint8_t buf[40];
    unsigned n = 0;
    buf[n++] = (uint8_t)(0x0B << 1);
    buf[n++] = r->target_cmd;
    buf[n++] = (uint8_t)((0x0B << 1) | 1);
    buf[n++] = r->count;
    for (unsigned k = 0; k < r->count; k++) buf[n++] = r->data[k];
    return crc8(buf, n);
}

static bool has_cmd_ev(const ds_ev_row_t *rows, unsigned n, uint8_t cmd) {
    for (unsigned k = 0; k < n; k++) if (rows[k].cmd == cmd) return true;
    return false;
}
static bool has_cmd_gate(const ag_row_t *rows, unsigned n, uint8_t cmd) {
    for (unsigned k = 0; k < n; k++) if (rows[k].cmd == cmd) return true;
    return false;
}

int main(void) {
    /* ---- counts + generator integrity ---- */
    CHECK(COUNT(DS12P_POWER_ROWS) == 46);
    CHECK(COUNT(DS12P_CHARGER_ROWS) == 50);
    CHECK(COUNT(DS12P_GATE_POWER_ROWS) == COUNT(DS12P_POWER_ROWS));
    CHECK(COUNT(DS12P_GATE_CHARGER_ROWS) == COUNT(DS12P_CHARGER_ROWS));
    CHECK(COUNT(DS12P_PAGES_POWER) == 22);
    CHECK(COUNT(DS12P_PAGES_CHARGER) == 22);
    CHECK(COUNT(DS12P_WRITES) == 7);

    /* ---- independent PEC recomputation for every emitted frame ---- */
    for (unsigned k = 0; k < COUNT(DS12P_POWER_ROWS); k++) {
        const ds_ev_row_t *r = &DS12P_POWER_ROWS[k];
        CHECK(r->row_id && strncmp(r->row_id, "P12P-", 5) == 0);
        CHECK(r->count >= 1 && r->count <= 32);
        CHECK((r->shape == DS_EV_WORD && r->count == 2) || (r->shape == DS_EV_BLOCK && r->count >= 4));
        CHECK(pec_read_row(r->cmd, r) == r->pec);
    }
    for (unsigned k = 0; k < COUNT(DS12P_CHARGER_ROWS); k++) {
        const ds_ev_row_t *r = &DS12P_CHARGER_ROWS[k];
        CHECK(r->row_id && strncmp(r->row_id, "P12P-", 5) == 0);
        CHECK((r->shape == DS_EV_WORD && r->count == 2) || (r->shape == DS_EV_BLOCK && r->count >= 4));
        CHECK(pec_read_row(r->cmd, r) == r->pec);
    }
    for (unsigned k = 0; k < COUNT(DS12P_PAGES_POWER); k++) CHECK(pec_page_row(&DS12P_PAGES_POWER[k]) == DS12P_PAGES_POWER[k].pec);
    for (unsigned k = 0; k < COUNT(DS12P_PAGES_CHARGER); k++) CHECK(pec_page_row(&DS12P_PAGES_CHARGER[k]) == DS12P_PAGES_CHARGER[k].pec);
    for (unsigned k = 0; k < COUNT(DS12P_WRITES); k++) {
        CHECK(DS12P_WRITES[k].row_id && strncmp(DS12P_WRITES[k].row_id, "P12P-", 5) == 0);
        CHECK(DS12P_WRITES[k].len >= 1 && (DS12P_WRITES[k].shape == DS_EV_WORD || DS12P_WRITES[k].shape == DS_EV_BLOCK));
    }

    /* ---- gate rows must equal the response rows (bidirectional) ---- */
    for (unsigned k = 0; k < COUNT(DS12P_POWER_ROWS); k++)
        CHECK(has_cmd_gate(DS12P_GATE_POWER_ROWS, COUNT(DS12P_GATE_POWER_ROWS), DS12P_POWER_ROWS[k].cmd));
    for (unsigned k = 0; k < COUNT(DS12P_GATE_POWER_ROWS); k++)
        CHECK(has_cmd_ev(DS12P_POWER_ROWS, COUNT(DS12P_POWER_ROWS), DS12P_GATE_POWER_ROWS[k].cmd));
    for (unsigned k = 0; k < COUNT(DS12P_GATE_POWER_ROWS); k++) {
        CHECK(DS12P_GATE_POWER_ROWS[k].row_id && strncmp(DS12P_GATE_POWER_ROWS[k].row_id, "P12P-", 5) == 0);
    }

    /* ---- gate policy ---- */
    ag_gate_t g;
    ag_init(&g, DS12P_GATE_POWER_ROWS, COUNT(DS12P_GATE_POWER_ROWS), false);
    CHECK(ag_auth_state(&g) == AG_AUTH_UNVERIFIED);
    CHECK(strstr(ag_auth_state_str(&g), "UNVERIFIED") != 0);
    CHECK(strstr(ag_auth_state_str(&g), "no_authentication_exchange_identified") != 0);   /* R5 wording */
    CHECK(ag_has_evidence(&g, 0x08));
    CHECK(!ag_may_serve(&g, 0x08));                /* DYNAMIC (temperature moved in capture) -> refused by default */
    CHECK(ag_may_serve(&g, 0x21));                 /* STATIC (DeviceName "Veridian") */
    CHECK(ag_may_serve(&g, 0x86));                 /* POWER capture: 35 identical zero payloads -> STATIC */
    CHECK(!ag_may_serve(&g, 0x00));
    CHECK(!ag_may_serve(&g, 0x03));
    CHECK(!ag_may_serve(&g, 0x23));
    CHECK(!ag_may_serve(&g, 0x2F));
    CHECK(!ag_may_serve(&g, 0x61));                /* write command, never a read row */
    CHECK(!ag_may_serve(&g, 0x9A));                /* excluded: over-read shape undetermined */
    CHECK(!ag_may_serve(&g, 0x05));                /* excluded: glitch artifact */
    CHECK(ag_servable_count(&g) > 0 && ag_servable_count(&g) < COUNT(DS12P_GATE_POWER_ROWS));

    ag_init(&g, DS12P_GATE_POWER_ROWS, COUNT(DS12P_GATE_POWER_ROWS), true);
    CHECK(ag_may_serve(&g, 0x08));
    CHECK(ag_servable_count(&g) == COUNT(DS12P_GATE_POWER_ROWS));

    /* ---- refusal counters via the wire layer's monotonic counter ---- */
    ag_init(&g, DS12P_GATE_POWER_ROWS, COUNT(DS12P_GATE_POWER_ROWS), true);
    ag_poll_counters(&g, 0, 0);
    CHECK(g.refused_reads == 0);
    ag_poll_counters(&g, 3, 0x2F);
    CHECK(g.refused_reads == 3);
    CHECK(g.have_last_refused && g.last_refused_cmd == 0x2F);
    ag_poll_counters(&g, 3, 0x2F);
    CHECK(g.refused_reads == 3);                   /* unchanged total -> no double count */
    ag_note_refused_write(&g, 0x61);
    CHECK(g.refused_writes == 1 && g.last_refused_cmd == 0x61);

    /* ---- a fake auth row may only be claimed with explicit opt-in ---- */
    static const ag_row_t fake_auth[] = { {0x2F, AG_ROW_STATIC, "AUTH-EVIDENCE/test-only"} };
    ag_gate_t ga;
    ag_init(&ga, fake_auth, COUNT(fake_auth), false);
    CHECK(ga.has_auth_evidence);
    CHECK(ag_auth_state(&ga) == AG_AUTH_UNVERIFIED);          /* opt-in still disabled */
    ag_set_allow_auth_rows(&ga, true);
    CHECK(ag_auth_state(&ga) == AG_AUTH_EVIDENCE_PRESENT);

    /* ---- selector pages ---- */
    sp_map_t sp;
    const uint8_t *data = 0;
    uint8_t count = 0;
    const char *rid = 0;
    sp_init(&sp, DS12P_PAGES_POWER, COUNT(DS12P_PAGES_POWER), false);
    CHECK(!sp_lookup(&sp, 0x66, &data, &count, &rid));        /* no selector written yet */
    CHECK(!sp_on_write(&sp, 0x08, 0, 0));                      /* not a selector command */
    {
        const uint8_t body65[] = {0x03, 0x00};                 /* 0x65 page 3: STATIC page */
        CHECK(sp_on_write(&sp, 0x65, body65, 2) == SP_WRITE_APPLIED);
        CHECK(sp_last_target(&sp) == 0x66);                    /* R1: dispatch by written selector */
        CHECK(sp_lookup(&sp, 0x66, &data, &count, &rid));
        CHECK(count == 32);
        CHECK(rid && strstr(rid, "sel65p3") != 0);
        for (unsigned k = 0; k < COUNT(DS12P_PAGES_POWER); k++) {
            if (DS12P_PAGES_POWER[k].sel_cmd == 0x65 && DS12P_PAGES_POWER[k].page == 3) {
                for (unsigned b = 0; b < count; b++) CHECK(data[b] == DS12P_PAGES_POWER[k].data[b]);
            }
        }
        const uint8_t body73[] = {0x05, 0x00};                 /* 0x73 page 5 -> 0x74 */
        CHECK(sp_on_write(&sp, 0x73, body73, 2) == SP_WRITE_APPLIED);
        CHECK(sp_last_target(&sp) == 0x74);                    /* R1: 0x73 must target 0x74 */
        CHECK(sp_lookup(&sp, 0x74, &data, &count, &rid));
        CHECK(count == 32);
        CHECK(rid && strstr(rid, "sel73p5") != 0);
        CHECK(sp_lookup(&sp, 0x66, &data, &count, &rid));      /* 0x66 keeps its 0x65 page */
        const uint8_t body_bad[] = {0x7F, 0x00};               /* page 0x7F: not captured */
        CHECK(sp_on_write(&sp, 0x65, body_bad, 2) == SP_WRITE_UNKNOWN_PAGE);
        CHECK(sp.unknown_pages == 1);
        CHECK(sp_last_target(&sp) == 0x74);                    /* R2: last applied was 0x73; unknown 0x65 only clears 0x66 */
        CHECK(sp_lookup(&sp, 0x74, &data, &count, &rid));      /* 0x73 page 5 untouched */
        CHECK(!sp_lookup(&sp, 0x66, &data, &count, &rid));     /* page not captured -> refuse */
    }
    /* R1/R2 module-level dispatch + page-target helpers */
    CHECK(sp_is_selector(0x65) && sp_is_selector(0x73) && !sp_is_selector(0x66));
    CHECK(sp_is_page_target(0x66) && sp_is_page_target(0x74) && !sp_is_page_target(0x65));
    {
        sp_map_t m2;
        const uint8_t *d2 = 0; uint8_t n2 = 0;
        sp_init(&m2, DS12P_PAGES_POWER, COUNT(DS12P_PAGES_POWER), true);
        CHECK(!sp_read_hook(&m2, 0x66, &d2, &n2));              /* R2: no selector -> no data */
        const uint8_t p3[] = {0x03, 0x00};
        CHECK(sp_select_hook(&m2, 0x65, p3, 2));
        CHECK(sp_is_target_hook(0, 0x66) && !sp_is_target_hook(0, 0x65));
        CHECK(sp_read_hook(&m2, 0x66, &d2, &n2) && d2 && n2 == 32);
        const uint8_t pbad[] = {0x7F, 0x00};
        CHECK(sp_select_hook(&m2, 0x65, pbad, 2));
        CHECK(!sp_read_hook(&m2, 0x66, &d2, &n2));               /* R2: unknown page refuses */
    }

    /* DYNAMIC page 0 must be refused unless reference serving is enabled */
    sp_init(&sp, DS12P_PAGES_POWER, COUNT(DS12P_PAGES_POWER), false);
    {
        const uint8_t body0[] = {0x00, 0x00};
        CHECK(sp_on_write(&sp, 0x65, body0, 2));
        CHECK(!sp_lookup(&sp, 0x66, &data, &count, &rid));
    }
    sp_init(&sp, DS12P_PAGES_POWER, COUNT(DS12P_PAGES_POWER), true);
    {
        const uint8_t body0[] = {0x00, 0x00};
        CHECK(sp_on_write(&sp, 0x65, body0, 2));
        CHECK(sp_lookup(&sp, 0x66, &data, &count, &rid));
    }

    /* ---- CHARGER-only rows exist only in the charger table ---- */
    CHECK(has_cmd_ev(DS12P_CHARGER_ROWS, COUNT(DS12P_CHARGER_ROWS), 0x62));
    CHECK(has_cmd_ev(DS12P_CHARGER_ROWS, COUNT(DS12P_CHARGER_ROWS), 0x63));
    CHECK(has_cmd_ev(DS12P_CHARGER_ROWS, COUNT(DS12P_CHARGER_ROWS), 0x64));
    CHECK(has_cmd_ev(DS12P_CHARGER_ROWS, COUNT(DS12P_CHARGER_ROWS), 0x60));
    CHECK(!has_cmd_ev(DS12P_POWER_ROWS, COUNT(DS12P_POWER_ROWS), 0x62));
    ag_init(&g, DS12P_GATE_CHARGER_ROWS, COUNT(DS12P_GATE_CHARGER_ROWS), false);
    CHECK(ag_may_serve(&g, 0x62));                             /* STATIC recorded block (one instance) */
    CHECK(ag_may_serve(&g, 0x60));

    printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
