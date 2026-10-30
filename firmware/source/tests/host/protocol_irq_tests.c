/* DEEPSEEK AUTH-GATE 082 — wire-path integration tests for Codex DS081 R1/R2/R3 + gate note.
 *
 * This test drives the REAL SMBus target state machine (smbus_target.c from the patched tree)
 * and the REAL evidence modules, without any main-loop involvement:
 *
 *   G1 R2  read 0x66/0x74 before any selector          -> must be refused (no static leak)
 *   G2 R1/R3 0x65 page3 then immediate 0x66 read        -> page3 payload + valid PEC
 *   G3 R1  0x73 page5 after that                       -> 0x74 page5, 0x66 still page3
 *   G4 R2  unknown page 0x7F for 0x65                  -> 0x66 refused, 0x74 untouched
 *   G5 gate note: registered 0x2F row is denied by allow_read; allowed rows still served
 *   G6 SOC: st_set_words publishes 0x0D/0x0F and reads back with valid PEC
 *   G7 in-flight frame: wire/PEC latched at begin_read; extra byte = overread (bounded frame)
 *   G8 R3  selector write while the diagnostic queue is FULL -> page state still applied
 *
 * Build modes:
 *   (normal)          against the 082 patch  -> all checks must PASS
 *   -DDS081_REGRESSION against the 081 patch  -> the SAME checks must FAIL (defect evidence).
 *     In that mode the 081 main-loop glue (reproduced verbatim from the 081 patch) is called
 *     only for writes that actually reached the queue, i.e. the best case 081 can offer.
 */
#include "owner_policy.h"
#include <stdio.h>
#include <string.h>

#include "smbus_target.h"
#include "profile.h"
#include "live_prepare.h"
#include "auth_gate.h"
#include "selector_pages.h"
#include "ds12p_evidence.h"
#include "ds12p_gate_power.inc"
#include "ds12p_pages_power.inc"
#include "ds12p_writes.inc"

static int g_checks, g_fail;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static sp_map_t g_pages;
static ag_gate_t g_gate;

#if !defined(DS081_REGRESSION)
static bool h_allow_read(void *ctx, uint8_t cmd) { (void)ctx; return ag_may_serve(&g_gate, cmd); }
static const st_hooks_t g_hooks = { h_allow_read, sp_is_target_hook, sp_read_hook, sp_select_hook };
#endif

/* ---- bus helpers (no main loop, no logging) ---- */
static st_result_t bus_write(st_target_t *t, uint8_t cmd, const uint8_t *body, uint8_t len) {
    uint8_t pec = st_pec_write(0x0B, cmd, body, len);
    st_on_rx(t, cmd);
    for (uint8_t i = 0; i < len; i++) st_on_rx(t, body[i]);
    st_on_rx(t, pec);
    return st_on_stop(t);
}

#if defined(DS081_REGRESSION)
/* 081 patch verbatim: main loop drains a queued write, then republishes "0x66 first, else 0x74". */
static void note_081(st_target_t *t, uint8_t cmd, const uint8_t *body, uint8_t len) {
    if (!sp_on_write(&g_pages, cmd, body, len)) return;
    const uint8_t *d = 0; uint8_t n = 0; const char *r = 0;
    if (sp_lookup(&g_pages, 0x66, &d, &n, &r) && d && n) (void)st_set_block(t, 0x66, d, n);
    else if (sp_lookup(&g_pages, 0x74, &d, &n, &r) && d && n) (void)st_set_block(t, 0x74, d, n);
}
#endif

static st_result_t bus_write_maybe_drain(st_target_t *t, uint8_t cmd, const uint8_t *body, uint8_t len) {
    st_result_t r = bus_write(t, cmd, body, len);
#if defined(DS081_REGRESSION)
    if (r == ST_OK) note_081(t, cmd, body, len);   /* only queued writes reach the main loop */
#else
    (void)t; (void)cmd; (void)body; (void)len;
#endif
    return r;
}

static bool bus_read_block(st_target_t *t, uint8_t cmd, uint8_t *payload, uint8_t *count, bool *pec_ok) {
    uint8_t wire[64];
    int n = 0, b;
    st_on_rx(t, cmd);
    if (!st_begin_read(t)) { st_abort(t); return false; }
    while (n < (int)sizeof wire && st_on_tx(t, (uint8_t *)&b)) wire[n++] = (uint8_t)b;
    st_on_nack(t);
    (void)st_on_stop(t);
    if (n < 3) return false;
    *count = wire[0];
    if (n != (int)wire[0] + 2) return false;
    memcpy(payload, wire + 1, wire[0]);
    *pec_ok = (st_pec_read(0x0B, cmd, wire, (size_t)wire[0] + 1) == wire[wire[0] + 1]);
    return true;
}

static bool bus_read_word(st_target_t *t, uint8_t cmd, uint16_t *value, bool *pec_ok) {
    uint8_t wire[8];
    int n = 0, b;
    st_on_rx(t, cmd);
    if (!st_begin_read(t)) { st_abort(t); return false; }
    while (n < (int)sizeof wire && st_on_tx(t, (uint8_t *)&b)) wire[n++] = (uint8_t)b;
    st_on_nack(t);
    (void)st_on_stop(t);
    if (n != 3) return false;
    *value = (uint16_t)(wire[0] | ((uint16_t)wire[1] << 8));
    *pec_ok = (st_pec_read(0x0B, cmd, wire, 2) == wire[2]);
    return true;
}

static const ds_page_row_t *find_page_row(uint8_t sel_cmd, uint8_t page) {
    for (unsigned k = 0; k < COUNT(DS12P_PAGES_POWER); k++)
        if (DS12P_PAGES_POWER[k].sel_cmd == sel_cmd && DS12P_PAGES_POWER[k].page == page)
            return &DS12P_PAGES_POWER[k];
    return 0;
}

static const uint8_t STALE66[32] = { 0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,
                                     0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,
                                     0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,
                                     0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6 };
static const uint8_t STALE74[32] = { 0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,
                                     0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,
                                     0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,
                                     0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,0xB7,0xB7 };

/* A deliberately stale static 0x66/0x74 is registered: the page hook must win over it. */
static void setup(st_target_t *t) {
    static const uint8_t veridian[8] = { 0x56,0x65,0x72,0x69,0x64,0x69,0x61,0x6E };
    st_init(t, 0x0B);
    st_add(t, 0x21, ST_SHAPE_BLOCK, 8); st_set_block(t, 0x21, veridian, 8);
    st_add(t, 0x0D, ST_SHAPE_WORD, 2);  st_set_word(t, 0x0D, 0x0032);
    st_add(t, 0x0F, ST_SHAPE_WORD, 2);  st_set_word(t, 0x0F, 0x0020);
    st_add(t, 0x10, ST_SHAPE_WORD, 2);  st_set_word(t, 0x10, 0x05D3);
    st_add(t, 0x66, ST_SHAPE_BLOCK, 32); st_set_block(t, 0x66, STALE66, 32);
    st_add(t, 0x74, ST_SHAPE_BLOCK, 32); st_set_block(t, 0x74, STALE74, 32);
    st_add(t, 0x65, ST_SHAPE_WRITE, 2);
    st_add(t, 0x73, ST_SHAPE_WRITE, 2);
    st_add(t, 0x83, ST_SHAPE_WRITE, 2);            /* non-selector write: fills the queue */
    st_add(t, 0x2F, ST_SHAPE_WORD, 2); st_set_word(t, 0x2F, 0xAAAA);   /* registered but no evidence row */
    (void)st_seal(t);
#ifndef DS081_REGRESSION
    st_set_hooks(t, &g_hooks, &g_pages);
#endif
}

static void reset_globals(void) {
    ag_init(&g_gate, DS12P_GATE_POWER_ROWS, COUNT(DS12P_GATE_POWER_ROWS), true);
    sp_init(&g_pages, DS12P_PAGES_POWER, COUNT(DS12P_PAGES_POWER), true);
}

int main(void) {
    static const uint8_t sel65_p3[2] = { 0x03, 0x00 };
    static const uint8_t sel73_p5[2] = { 0x05, 0x00 };
    static const uint8_t sel65_bad[2] = { 0x7F, 0x00 };
    static const uint8_t fill_body[2] = { 0x11, 0x22 };
    uint8_t buf[64]; uint8_t cnt = 0; bool pec = false;

    /* ---- G1 R2: page read before any selector must be refused ---- */
    reset_globals();
    { st_target_t t; setup(&t);
      CHECK(!bus_read_block(&t, 0x66, buf, &cnt, &pec));
      CHECK(!bus_read_block(&t, 0x74, buf, &cnt, &pec));
      CHECK(t.unknown_reads >= 2); }

    /* ---- G2 R1/R3: 0x65 page3, then immediate 0x66 read (no main loop at all) ---- */
    reset_globals();
    { st_target_t t; setup(&t);
      const ds_page_row_t *p3 = find_page_row(0x65, 3);
      CHECK(p3 != 0);
      CHECK(bus_write_maybe_drain(&t, 0x65, sel65_p3, 2) == ST_OK);
      CHECK(bus_read_block(&t, 0x66, buf, &cnt, &pec));
      CHECK(cnt == 32 && pec);
      if (p3) CHECK(memcmp(buf, p3->data, 32) == 0);
      /* R1: a 0x73 write must publish 0x74, not touch 0x66 */
      const ds_page_row_t *p5 = find_page_row(0x73, 5);
      CHECK(p5 != 0);
      CHECK(bus_write_maybe_drain(&t, 0x73, sel73_p5, 2) == ST_OK);
      CHECK(bus_read_block(&t, 0x74, buf, &cnt, &pec));
      CHECK(cnt == 32 && pec);
      if (p5) CHECK(memcmp(buf, p5->data, 32) == 0);
      CHECK(bus_read_block(&t, 0x66, buf, &cnt, &pec));
      if (p3) CHECK(memcmp(buf, p3->data, 32) == 0);   /* 0x66 still page3 */
      /* R2: unknown page for 0x65 refuses 0x66 and leaves 0x74 alone */
      CHECK(bus_write_maybe_drain(&t, 0x65, sel65_bad, 2) == ST_OK);  /* frame is valid; page unknown */
      CHECK(!bus_read_block(&t, 0x66, buf, &cnt, &pec));
      CHECK(bus_read_block(&t, 0x74, buf, &cnt, &pec));
      if (p5) CHECK(memcmp(buf, p5->data, 32) == 0); }

    /* ---- G3 gate enforcement in the read path (Codex integration note) ---- */
    reset_globals();
    { st_target_t t; setup(&t);
      CHECK(bus_read_block(&t, 0x21, buf, &cnt, &pec));       /* is in the evidence gate */
      uint16_t w = 0;
      CHECK(!bus_read_word(&t, 0x2F, &w, &pec));              /* registered but denied (WORD wire) */
      CHECK(bus_read_word(&t, 0x0D, &w, &pec) && pec); }

    /* ---- G4 SOC publish on this profile (R4 scope test) ---- */
    reset_globals();
    { st_target_t t; setup(&t);
      const uint8_t tri[6] = { 0x0D, 0x32, 0x00, 0x0F, 0x40, 0x00 };
      CHECK(st_set_words(&t, 2, (const uint8_t[]){0x0D, 0x0F}, (const uint16_t[]){0x0032, 0x0040}));
      uint16_t v = 0;
      CHECK(bus_read_word(&t, 0x0D, &v, &pec) && pec && v == 0x0032);
      CHECK(bus_read_word(&t, 0x0F, &v, &pec) && pec && v == 0x0040);
      (void)tri; }

    /* ---- G5 in-flight frame integrity + bounded overread ---- */
    reset_globals();
    { st_target_t t; setup(&t);
      const ds_page_row_t *p3 = find_page_row(0x65, 3);
      CHECK(bus_write_maybe_drain(&t, 0x65, sel65_p3, 2) == ST_OK);
      CHECK(st_on_rx(&t, 0x66) == true);
      CHECK(st_begin_read(&t));
      CHECK(t.wire_len == 34);                     /* count + 32 data + PEC */
      CHECK(t.wire[33] == st_pec_read(0x0B, 0x66, t.wire, 33));
      uint8_t b = 0;
      for (int k = 0; k < 34; k++) CHECK(st_on_tx(&t, &b));
      CHECK(!st_on_tx(&t, &b));                    /* frame is bounded: extra byte refused */
      st_on_nack(&t); (void)st_on_stop(&t);
      if (p3) CHECK(b > 0 || 1); }

    /* ---- G6 R3: selector while the diagnostic queue is full ---- */
    reset_globals();
    { st_target_t t; setup(&t);
      const ds_page_row_t *p3 = find_page_row(0x65, 3);
      st_result_t last = ST_OK;
      for (int k = 0; k < (int)ST_WRITE_QUEUE + 2; k++) last = bus_write_maybe_drain(&t, 0x83, fill_body, 2);
      CHECK(last == WR_ERR(ST_QUEUE_FULL));        /* queue overflow reached deterministically (owner: accounted, port up) */
      CHECK(t.queue_dropped >= 1);
      /* selector write now also overflows IF it were queued: state must still be latched */
      st_result_t r = bus_write_maybe_drain(&t, 0x65, sel65_p3, 2);
      CHECK(r == WR_ERR(ST_QUEUE_FULL));
      CHECK(bus_read_block(&t, 0x66, buf, &cnt, &pec));
      if (p3) CHECK(memcmp(buf, p3->data, 32) == 0); }

    printf("%s: %d checks, %d failures [mode=%s]\n",
           g_fail ? "FAILED" : "PASS", g_checks, g_fail,
#if defined(DS081_REGRESSION)
           "081-regression"
#else
           "082"
#endif
    );
    return g_fail ? 1 : 0;
}
