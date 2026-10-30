/* CHIPLER P1_r2 - host contract test for the live SMBus engine.
 *
 * DS-REUSE-13: ports the *test intent* of the deepseek_original history that the frozen
 * 385c87d tests/host suite never carried over. Source of each block:
 *   - 08_Live_SMBus_Work/CODEX_BETA1_TEST_GAP_ANALYSIS.md  D01-G1..G3, D02-G1..G6,
 *     D03-G1..G8, D04-G1..G8, section 5 "tests proposed" (A-level, no production edits)
 *   - 15_Codex_Beta1_Verification/test_edges.c (141 independent checks: wire count>16,
 *     multiple FIRST, commandless read, NACK+STOP, idle expiry)
 *   - 08_Live_SMBus_Work/_scratch/{conformance,smoke,snapshot}.c (early probes)
 *
 * The production sources are compiled UNMODIFIED (this file only calls the public API).
 * Every expectation below is derived by hand from the frozen sources
 * 385c87da27394ddad81f4bc9e21be1cec8823ed2 (src/batt/smbus_target.c, live_wire.c,
 * live_prepare.c) and checked against the captured catalog, NOT copied from the
 * production PEC helpers: the independent CRC-8 below is the oracle.
 *
 * Scope: host-only. No hardware, no phone, no UF2. Nothing here qualifies a phone.
 */
#include "../../src/batt/smbus_target.h"
#include "../../src/batt/live_wire.h"
#include "../../src/batt/live_prepare.h"
#include "smbus_vectors.inc"
#include "owner_policy.h"
#include <stdio.h>
#include <string.h>

/* captured catalog tables, included for the "which commands are readable" oracle */
typedef struct { uint8_t cmd; st_shape_t shape; uint8_t count, data[32]; } captured_response_t;
typedef struct { uint8_t cmd, len; } captured_write_shape_t;
#include "../../src/batt/captured_responses.inc"

static unsigned checks, fails;
#define C(x) do { checks++; if (!(x)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* ---------------- independent SMBus PEC (CRC-8, poly 0x07, MSB-first) ---------------- */
static uint8_t crc8(uint8_t c, uint8_t b) {
    c ^= b;
    for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1));
    return c;
}
static uint8_t exp_pec_read(uint8_t cmd, const uint8_t *d, int n) {
    uint8_t c = crc8(crc8(0, (uint8_t)(0x0B << 1)), cmd);
    c = crc8(c, (uint8_t)((0x0B << 1) | 1));
    for (int i = 0; i < n; i++) c = crc8(c, d[i]);
    return c;
}
static uint8_t exp_pec_write(uint8_t cmd, const uint8_t *d, int n) {
    uint8_t c = crc8(crc8(0, (uint8_t)(0x0B << 1)), cmd);
    for (int i = 0; i < n; i++) c = crc8(c, d[i]);
    return c;
}

/* ---------------- instrumented IRQ guard (D01-G2/G3 "guard balance") ---------------- */
static unsigned g_enters, g_leaves, g_inside;
static uint32_t g_guard_enter(void *p) { (void)p; g_enters++; g_inside = 1; return 0; }
static void g_guard_exit(void *p, uint32_t v) { (void)p; (void)v; C(g_inside == 1); g_inside = 0; g_leaves++; }
static void guard_reset(void) { g_enters = g_leaves = g_inside = 0; }

static void mk(st_target_t *t) { st_init(t, 0x0B); C(st_set_guard(t, g_guard_enter, g_guard_exit, 0)); }
static void mk_word(st_target_t *t) {
    mk(t);
    C(st_add(t, 0x09, ST_SHAPE_WORD, 2));
    C(st_set_word(t, 0x09, 0x1122));
    C(st_seal(t));
}
static void mk_cap(st_target_t *t) {
    st_init(t, 0x0B);
    C(st_set_guard(t, g_guard_enter, g_guard_exit, 0));
    const batt_profile_t *p = profile_get(profile_find("CAP12PM-B1"));
    C(p && live_prepare(t, p, 50) && st_live_ready(t));
}
static void mk_block4(st_target_t *t, const uint8_t v[4]) {
    mk(t);
    C(st_add(t, 0x47, ST_SHAPE_BLOCK, 4));
    C(st_set_block(t, 0x47, v, 4));
    C(st_seal(t));
}
static bool readable_in_catalog(uint8_t cmd) {
    for (unsigned k = 0; k < sizeof captured_responses / sizeof captured_responses[0]; k++)
        if (captured_responses[k].cmd == cmd) return true;
    return false;
}
/* feed a command + body + PEC as one STOP-terminated write; body excludes cmd/PEC */
static st_result_t send_write(st_target_t *t, uint8_t cmd, const uint8_t *body, unsigned n) {
    uint8_t f[40];
    f[0] = cmd;
    memcpy(f + 1, body, n);
    f[n + 1] = exp_pec_write(cmd, body, (int)n);
    for (unsigned i = 0; i < n + 2; i++)
        if (!st_on_rx(t, f[i])) return t->last;
    return st_on_stop(t);
}

/* =====================================================================================
 * S0 - independent PEC over the committed golden vectors (24 PEC-bearing of 25)
 * ===================================================================================== */
static void s0_golden_pec_cross_check(void) {
    unsigned with_pec = 0;
    for (unsigned k = 0; k < SMBUS_VECTOR_COUNT; k++) {
        const smbus_vector_t *v = &SMBUS_VECTORS[k];
        if (!v->has_pec) { C(v->shape == SV_A8); continue; }
        with_pec++;
        uint8_t pec;
        if (v->shape == SV_WORD) {
            C(v->len == 2);
            pec = exp_pec_read(v->cmd, v->payload, 2);
        } else if (v->shape == SV_BLOCK) {
            uint8_t buf[33];
            C(v->len >= 1 && v->len <= 32);
            buf[0] = v->len;
            memcpy(buf + 1, v->payload, v->len);
            pec = exp_pec_read(v->cmd, buf, (int)v->len + 1);
        } else {
            C(v->shape == SV_WRITE);
            pec = exp_pec_write(v->cmd, v->payload, v->len);
        }
        C(pec == v->pec);
        /* the same oracle must agree with the production helper used by the engine */
        if (v->shape == SV_WRITE) C(st_pec_write(0x0B, v->cmd, v->payload, v->len) == pec);
        else if (v->shape == SV_WORD) C(st_pec_read(0x0B, v->cmd, v->payload, 2) == pec);
    }
    C(with_pec == 24);
    printf("  S0 golden vectors: %u PEC-bearing vectors re-derived with an independent CRC-8\n", with_pec);
}

/* =====================================================================================
 * S1 - every one of the 256 command bytes against the captured map: no flat fallback
 *      (history D02-G6 "no_flat_fallback_sweep"; the legacy 0x20..0x3F range must NOT
 *      be served just because old firmware had a flat register table)
 * ===================================================================================== */
static void s1_no_flat_fallback_sweep(void) {
    st_target_t t;
    st_init(&t, 0x0B);
    C(st_set_guard(&t, g_guard_enter, g_guard_exit, 0));
    const batt_profile_t *p = profile_get(profile_find("CAP12PM-B1"));
    C(p && live_prepare(&t, p, 50) && st_live_ready(&t));
    C(t.n_regs == 27);

    unsigned served = 0, legacy_served = 0, unexpected = 0, filled = 0;
    for (unsigned cmd = 0; cmd < 256; cmd++) {
        bool started = st_on_rx(&t, (uint8_t)cmd) && st_begin_read(&t);
        if (!started) { st_abort(&t); continue; }
        if (t.idle_fill) {   /* OWNER: unknown read answered with released-bus 0xFF, no data served, port stays up */
            uint8_t ff = 0; C(st_on_tx(&t, &ff) && ff == 0xFF); st_on_nack(&t); C(st_on_stop(&t) == ST_OK);
            filled++; continue;
        }
        served++;
        if (cmd >= 0x20 && cmd <= 0x3F) legacy_served++;
        if (!readable_in_catalog((uint8_t)cmd)) unexpected++;
        uint8_t b = 0;
        for (unsigned k = 0; k < t.wire_len; k++) C(st_on_tx(&t, &b));
        st_on_nack(&t);
        C(st_on_stop(&t) == ST_OK);
    }
    C(served == 22);
    C(filled == (OWNER ? 256u - 22u : 0u));
    C(legacy_served == 0);
    C(unexpected == 0);
    C(t.unknown_reads == 256u - 22u);
    printf("  S1 command sweep: %u/256 served, %u legacy-range, %u unknown -> %u unknown_reads\n",
           served, legacy_served, unexpected, t.unknown_reads);
}

/* =====================================================================================
 * S2 - A8 short-probe exception is exact; every near-miss is BAD_LENGTH + reads_short
 *      (history D04-G5 "a8_exception_negatives")
 * ===================================================================================== */
static void mk_a8(st_target_t *t, uint8_t len) {
    uint8_t blk[32];
    memset(blk, 0x33, sizeof blk);
    st_init(t, 0x0B);
    C(st_set_guard(t, g_guard_enter, g_guard_exit, 0));
    C(st_add(t, 0xA8, ST_SHAPE_BLOCK, len));
    C(st_set_block(t, 0xA8, blk, len));
    C(st_seal(t));
}
static void s2_a8_negatives(void) {
    st_target_t t;
    uint8_t b = 0;
    /* positive: A8 block24, one byte served, NACK, STOP -> ST_SHORT_A8 */
    mk_a8(&t, 24);
    C(st_on_rx(&t, 0xA8) && st_begin_read(&t));
    C(st_on_tx(&t, &b) && b == 24);
    st_on_nack(&t);
    C(st_on_stop(&t) == ST_SHORT_A8);
    C(t.reads_short == 1 && t.reads_done == 0);
    /* len != 24: BAD_LENGTH, not the A8 exception */
    mk_a8(&t, 23);
    C(st_on_rx(&t, 0xA8) && st_begin_read(&t));
    C(st_on_tx(&t, &b) && b == 23);
    st_on_nack(&t);
    C(st_on_stop(&t) == RD_PROBE(ST_BAD_LENGTH));
    C(t.reads_short == 1 && t.reads_done == 0);
    /* A8 len24 but two bytes asked: not the exception */
    mk_a8(&t, 24);
    C(st_on_rx(&t, 0xA8) && st_begin_read(&t));
    C(st_on_tx(&t, &b) && b == 24);
    C(st_on_tx(&t, &b));
    st_on_nack(&t);
    C(st_on_stop(&t) == ST_BAD_LENGTH);
    C(t.reads_short == 1 && t.reads_done == 0);
    /* block that is not A8, one byte asked: not the exception */
    { const uint8_t v[4] = { 1, 2, 3, 4 }; mk_block4(&t, v); }
    C(st_on_rx(&t, 0x47) && st_begin_read(&t));
    C(st_on_tx(&t, &b) && b == 4);
    st_on_nack(&t);
    C(st_on_stop(&t) == RD_PROBE(ST_BAD_LENGTH));
    C(t.reads_short == 1 && t.reads_done == 0);
    printf("  S2 A8 exception: 1 positive + 3 near-miss shapes assert exactly\n");
}

/* =====================================================================================
 * S3 - write contract: unknown / read-only / short / block-32 / 36th byte / bad PEC /
 *      NACK-on-write / queue full / record immutability / abort keeps queue
 *      (history D02-G3/G4, D03-G7, D04-G6/G7, C2..C4)
 * ===================================================================================== */
static bool queue_empty(st_target_t *t) { st_write_t w; return !st_pop_write(t, &w); }
static void s3_write_contract(void) {
    st_target_t t;
    const uint8_t two[2] = { 0x11, 0x22 };
    const uint8_t four[4] = { 1, 2, 3, 4 };

    /* unknown write command -> ST_UNSUPPORTED, nothing queued, writes_bad++ (C2 semantics) */
    mk_cap(&t);
    C(send_write(&t, 0xEE, two, 2) == WR_ERR(ST_UNSUPPORTED));
    C(queue_empty(&t));
    C(t.writes_ok == 0 && t.writes_bad == 1);
    /* write to a read-only WORD register -> ST_UNSUPPORTED, nothing queued */
    mk_cap(&t);
    C(send_write(&t, 0x09, two, 2) == WR_ERR(ST_UNSUPPORTED));
    C(queue_empty(&t) && t.writes_ok == 0);
    /* write to a read-only BLOCK register -> ST_UNSUPPORTED */
    mk_cap(&t);
    C(send_write(&t, 0x47, four, 4) == WR_ERR(ST_UNSUPPORTED));
    C(queue_empty(&t));
    /* 2-byte frame = short write: BAD_LENGTH, counted in BOTH bad_len and writes_bad (C3) */
    mk_cap(&t);
    C(st_on_rx(&t, 0x65) && st_on_rx(&t, 0x00));
    C(st_on_stop(&t) == WR_ERR(ST_BAD_LENGTH));
    C(t.bad_len == 1 && t.writes_bad == 1 && queue_empty(&t));
    /* valid 2-byte write (captured shape 0x65) commits */
    mk_cap(&t);
    C(send_write(&t, 0x65, two, 2) == ST_OK);
    { st_write_t w; C(st_pop_write(&t, &w) && w.cmd == 0x65 && w.len == 2 && !memcmp(w.body, two, 2)); }
    /* NACK during a write is ignored; the write still commits (D04-G7) */
    mk_cap(&t);
    C(st_on_rx(&t, 0x65) && st_on_rx(&t, two[0]) && st_on_rx(&t, two[1]));
    C(st_on_rx(&t, exp_pec_write(0x65, two, 2)));
    st_on_nack(&t);
    C(st_on_stop(&t) == ST_OK);
    C(t.writes_ok == 1);

    /* block-style 32-byte write = exactly 35 wire bytes accepted (D03-G7 / P05) */
    {
        uint8_t body[33];
        body[0] = 32;
        for (int k = 1; k < 33; k++) body[k] = (uint8_t)(0x40 + k);
        mk(&t);
        C(st_add(&t, 0x66, ST_SHAPE_WRITE, 32));
        C(st_seal(&t));
        C(send_write(&t, 0x66, body, 33) == ST_OK);
        st_write_t w;
        C(st_pop_write(&t, &w) && w.cmd == 0x66 && w.len == 33 && w.body[0] == 32);
        C(!memcmp(w.body + 1, body + 1, 32));
        C(t.writes_ok == 1);
        /* the 36th wire byte is refused and the frame is rejected as BAD_LENGTH */
        uint8_t frame[36];
        frame[0] = 0x66;
        memcpy(frame + 1, body, 33);
        frame[34] = 0x00; /* dummy PEC, never reached */
        for (unsigned i = 0; i < 35; i++) C(st_on_rx(&t, frame[i]));
        C(!st_on_rx(&t, 0x00));
        C(st_on_stop(&t) == WR_ERR(ST_BAD_LENGTH));
        C(t.writes_ok == 1 && queue_empty(&t));
    }
    /* bad PEC -> ST_BAD_PEC, nothing queued */
    mk_cap(&t);
    {
        uint8_t f[4] = { 0x65, two[0], two[1], (uint8_t)(exp_pec_write(0x65, two, 2) ^ 0xFF) };
        for (int i = 0; i < 4; i++) C(st_on_rx(&t, f[i]));
        C(st_on_stop(&t) == WR_ERR(ST_BAD_PEC));
        C(queue_empty(&t) && t.writes_ok == 0 && t.writes_bad == 1);
    }
    /* queue depth = ST_WRITE_QUEUE: N accepted, N+1 deterministic ST_QUEUE_FULL (R4) */
    mk_cap(&t);
    {
        for (unsigned k = 0; k < ST_WRITE_QUEUE; k++) C(send_write(&t, 0x65, two, 2) == ST_OK);
        C(send_write(&t, 0x65, two, 2) == WR_ERR(ST_QUEUE_FULL));
        C(t.queue_dropped == 1 && t.writes_ok == ST_WRITE_QUEUE);
        st_write_t w;
        for (unsigned k = 0; k < ST_WRITE_QUEUE; k++) C(st_pop_write(&t, &w) && w.cmd == 0x65);
        C(!st_pop_write(&t, &w));
    }
    /* records are immutable: setters and st_abort do not touch queued payload/sequence;
       abort KEEPS the queue (D01-G6, D03-G3) */
    mk_cap(&t);
    {
        const uint8_t a2[2] = { 0xA1, 0xA2 }, b2[2] = { 0xB1, 0xB2 };
        C(send_write(&t, 0x65, a2, 2) == ST_OK);
        C(send_write(&t, 0x65, b2, 2) == ST_OK);
        C(st_set_word(&t, 0x09, 0x3344)); /* control-loop update mid-queue */
        st_abort(&t);
        st_write_t w;
        C(st_pop_write(&t, &w) && w.cmd == 0x65 && w.len == 2 && w.sequence == 1 && !memcmp(w.body, a2, 2));
        C(st_pop_write(&t, &w) && w.cmd == 0x65 && w.len == 2 && w.sequence == 2 && !memcmp(w.body, b2, 2));
        C(!st_pop_write(&t, &w));
        C(t.writes_ok == 2);
    }
    /* STOP on an inactive target reports ST_ABORTED (D03-G5) */
    mk_word(&t);
    C(st_on_stop(&t) == ST_ABORTED);
    printf("  S3 write contract: unknown/read-only/short/35B/36B/badPEC/NACK/queue/immutability\n");
}

/* =====================================================================================
 * S4 - a read is served from one frozen frame even if the control loop updates the
 *      BLOCK value mid-read (D01-G1 "block_snapshot_mid_read"; the historical
 *      _scratch/snapshot.c probe generalised to the P1_r2 wire-copy design)
 * ===================================================================================== */
static void s4_block_snapshot_mid_read(void) {
    st_target_t t;
    const uint8_t a[4] = { 1, 2, 3, 4 }, b[4] = { 9, 9, 9, 9 };
    mk_block4(&t, a);
    guard_reset();
    C(st_on_rx(&t, 0x47) && st_begin_read(&t));
    C(g_enters == 0 && g_leaves == 0); /* IRQ path must not take the guard */
    uint32_t latched = t.latched_generation, gen0 = t.generation;
    uint8_t out[8];
    unsigned n = 0;
    C(st_on_tx(&t, &out[n]) && out[n++] == 4); /* count */
    C(st_on_tx(&t, &out[n++]));                 /* data[0] */
    C(st_on_tx(&t, &out[n++]));                 /* data[1] */
    guard_reset();
    C(st_set_block(&t, 0x47, b, 4)); /* publish new value while the read is in flight */
    C(g_enters == 1 && g_leaves == 1 && g_inside == 0);
    C(t.generation == gen0 + 1);
    C(t.latched_generation == latched); /* latching is fixed at begin_read */
    C(st_on_tx(&t, &out[n++]));         /* data[2] */
    C(st_on_tx(&t, &out[n++]));         /* data[3] */
    C(st_on_tx(&t, &out[n++]));         /* PEC */
    st_on_nack(&t);
    C(st_on_stop(&t) == ST_OK);
    C(n == 6); /* count + 4 data + PEC */
    C(out[0] == 4 && !memcmp(out + 1, a, 4));
    C(out[5] == exp_pec_read(0x47, out, 5));
    /* the next read sees the new value */
    C(st_on_rx(&t, 0x47) && st_begin_read(&t));
    C(st_on_tx(&t, &out[0]) && out[0] == 4);
    for (int i = 1; i < 6; i++) C(st_on_tx(&t, &out[i]));
    st_on_nack(&t);
    C(st_on_stop(&t) == ST_OK);
    C(!memcmp(out + 1, b, 4));
    printf("  S4 block snapshot: mid-read update never mixes; PEC of old frame preserved\n");
}

/* =====================================================================================
 * S5 - guard balance on failure paths and setup/IRQ separation (D01-G2/G3/G5, D05-G1/G3)
 * ===================================================================================== */
static void s5_guard_and_gating(void) {
    st_target_t t;
    mk(&t);
    C(st_add(&t, 0x09, ST_SHAPE_WORD, 2));
    C(st_set_word(&t, 0x09, 0x1122));
    C(st_add(&t, 0x47, ST_SHAPE_BLOCK, 4));
    C(st_set_block(&t, 0x47, (const uint8_t *)"\1\2\3\4", 4));
    C(st_seal(&t));
    /* failing setters still enter/leave exactly once (no leaked IRQ masking) */
    guard_reset();
    C(!st_set_word(&t, 0xEE, 0x1234)); /* unknown command */
    C(g_enters == 1 && g_leaves == 1 && g_inside == 0);
    C(!st_set_block(&t, 0x47, NULL, 4)); /* NULL payload */
    C(g_enters == 2 && g_leaves == 2 && g_inside == 0);
    C(!st_set_block(&t, 0x09, (const uint8_t *)"\1\2\3\4", 4)); /* wrong shape */
    C(g_enters == 3 && g_leaves == 3 && g_inside == 0);
    C(st_set_block(&t, 0x47, (const uint8_t *)"\1\2\3\4", 3) == false); /* wrong len */
    C(g_enters == 4 && g_leaves == 4 && g_inside == 0);
    /* st_abort takes the guard even when idle (documented D01-G3) */
    guard_reset();
    st_abort(&t);
    C(g_enters == 1 && g_leaves == 1 && g_inside == 0);
    /* a full IRQ transaction never takes the guard (D01-G5) */
    C(st_set_word(&t, 0x09, 0x5566));
    guard_reset();
    C(st_on_rx(&t, 0x09) && st_begin_read(&t));
    { uint8_t b = 0; C(st_on_tx(&t, &b)); C(st_on_tx(&t, &b)); C(st_on_tx(&t, &b)); }
    st_on_nack(&t);
    C(st_on_stop(&t) == ST_OK);
    C(g_enters == 0 && g_leaves == 0);
    /* gating: seal needs addr 0x0B; live_ready needs a guard; profile rejects locked ones */
    st_init(&t, 0x0A);
    C(st_set_guard(&t, g_guard_enter, g_guard_exit, 0));
    C(st_add(&t, 0x09, ST_SHAPE_WORD, 2) && st_set_word(&t, 0x09, 0) && !st_seal(&t));
    st_init(&t, 0x0B);
    C(st_add(&t, 0x09, ST_SHAPE_WORD, 2) && st_set_word(&t, 0x09, 0) && st_seal(&t));
    C(!st_live_ready(&t)); /* sealed but no IRQ guard installed */
    C(!st_set_guard(&t, g_guard_enter, g_guard_exit, 0)); /* guard is setup-only */
    C(!live_prepare(&t, profile_get(profile_find("P12PM-r0")), 50));
    printf("  S5 guard/gating: balanced on every failure path, IRQ path guard-free\n");
}

/* =====================================================================================
 * S6 - live_wire rejection matrix + NACK/STOP ordering + expiry edges
 *      (D02-G5, D03-G2/G6/G8/G9/G10, D04-G2/G3, test_edges.c)
 * ===================================================================================== */
static void s6_wire_matrix(void) {
    st_target_t t;
    live_wire_t w;
    live_reply_t r;

    /* count > 16 is a FIFO-batch overrun, not an overlong frame -> isolate (no fabricated reply) */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.count = 17;
      C(!live_wire_event(&w, &e, 1, &r) && w.blocked && w.fault == ST_ABORTED && !r.have_tx); }
    /* two FIRST_DATA_BYTE flags in one batch is ambiguous */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.count = 2; e.rx[0] = 0x0809; e.rx[1] = 0x0809;
      C(!live_wire_event(&w, &e, 1, &r) && w.blocked); }
    /* a byte without FIRST while the engine is idle = missed START */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.count = 1; e.rx[0] = 0x09;
      C(!live_wire_event(&w, &e, 1, &r) && w.blocked); }
    /* STOP + data bytes while a read is in flight cannot be ordered -> isolate */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.rx[0] = 0x0809; e.count = 1;
      C(live_wire_event(&w, &e, 1, &r)); }
    { live_event_t e = {0}; e.request = true;
      C(live_wire_event(&w, &e, 2, &r) && t.reading); }
    { live_event_t e = {0}; e.rx[0] = 0x09; e.count = 1; e.stop = true;
      C(!live_wire_event(&w, &e, 3, &r) && w.blocked); }
    /* STOP + FIRST + active transaction = ambiguity (new START vs old STOP) */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.rx[0] = 0x0809; e.count = 1;
      C(live_wire_event(&w, &e, 1, &r)); }
    { live_event_t e = {0}; e.rx[0] = 0x0809; e.count = 1; e.stop = true;
      C(!live_wire_event(&w, &e, 2, &r) && w.blocked); }
    /* request + stop in one batch */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.request = true; e.stop = true;
      C(!live_wire_event(&w, &e, 1, &r) && w.blocked); }
    /* NACK in one IRQ, STOP in the next: normal read end, must count as completed (D04-G2) */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.rx[0] = 0x0809; e.count = 1;
      C(live_wire_event(&w, &e, 1, &r)); }
    { live_event_t e = {0}; e.request = true;
      C(live_wire_event(&w, &e, 2, &r) && r.have_tx && r.tx == 0x22);
      C(live_wire_event(&w, &e, 3, &r) && r.tx == 0x11);
      C(live_wire_event(&w, &e, 4, &r)); }
    { live_event_t e = {0}; e.nack = true;
      C(live_wire_event(&w, &e, 5, &r)); }
    { live_event_t e = {0}; e.stop = true;
      C(live_wire_event(&w, &e, 6, &r)); }
    C(t.reads_done == 1 && !w.blocked);
    /* STOP with no NACK after a partial read: incomplete, isolate, not a completed read (D04-G3) */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.rx[0] = 0x0809; e.count = 1;
      C(live_wire_event(&w, &e, 1, &r)); }
    { live_event_t e = {0}; e.request = true;
      C(live_wire_event(&w, &e, 2, &r)); }
    { live_event_t e = {0}; e.stop = true;
      C(!live_wire_event(&w, &e, 3, &r) && w.blocked && t.aborts == 1 && t.reads_done == 0); }
    /* blocked is sticky: no later event is served without live_wire_init */
    { live_event_t e = {0}; e.rx[0] = 0x0809; e.count = 1;
      C(!live_wire_event(&w, &e, 4, &r) && !r.have_tx && w.blocked); }
    /* commandless read on a cold bus: no invented default pointer */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.request = true;
      C(!live_wire_event(&w, &e, 1, &r) && w.blocked && !r.have_tx); }
    /* expiry: exact boundary is not expired, +1 us is; an idle engine never expires */
    mk_word(&t);
    live_wire_init(&w, &t, 1000);
    { live_event_t e = {0}; e.rx[0] = 0x0809; e.count = 1;
      C(live_wire_event(&w, &e, 1000, &r)); }
    C(!live_wire_expire(&w, 1100, 100));   /* (now-last)==idle -> false */
    C(live_wire_expire(&w, 1101, 100) && w.blocked && w.fault == ST_ABORTED);
    C(!t.active && t.nw == 0);
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    C(!live_wire_expire(&w, 0xFFFFFFFFu, 100)); /* no open transaction */
    printf("  S6 wire matrix: 9 rejection shapes + NACK/STOP split + expiry boundary\n");
}

/* =====================================================================================
 * S7 - R3 pointer retention/clearing semantics at engine level
 *      (P1_r2 R3; asserted directly instead of only through the MMIO model)
 * ===================================================================================== */
static void s7_pointer_policy(void) {
    st_target_t t;
    live_wire_t w;
    live_reply_t r;
    /* command-only STOP of a known readable command retains the pointer */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.rx[0] = 0x0809; e.count = 1; e.stop = true;
      C(live_wire_event(&w, &e, 1, &r)); }
    C(t.ptr_valid && t.ptr == 0x09);
    C(st_resume_pointer(&t) && t.active && t.nw == 1 && t.w[0] == 0x09);
    C(st_begin_read(&t));
    C(!st_resume_pointer(&t)); /* already active */
    { uint8_t got[3]; for (int i = 0; i < 3; i++) C(st_on_tx(&t, &got[i]));
      C(got[0] == 0x22 && got[1] == 0x11 && got[2] == exp_pec_read(0x09, got, 2)); }
    st_on_nack(&t);
    C(st_on_stop(&t) == ST_OK);
    C(t.ptr_valid); /* retained after a completed known read */
    /* cold bus: nothing to resume */
    mk_word(&t);
    C(!t.ptr_valid && !st_resume_pointer(&t));
    /* unknown command in a command-only STOP is not retained */
    mk_word(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.rx[0] = 0x0871; e.count = 1; e.stop = true;
      C(live_wire_event(&w, &e, 1, &r)); }
    C(!t.ptr_valid && !st_resume_pointer(&t));
    /* any accepted write clears the pointer (prepared capture target has write shapes) */
    {
        st_target_t c;
        live_wire_t w2;
        const uint8_t body[2] = { 0xAA, 0xBB };
        mk_cap(&c);
        live_wire_init(&w2, &c, 0);
        live_event_t e0 = {0};
        e0.rx[0] = 0x0808; e0.count = 1; e0.stop = true;
        C(live_wire_event(&w2, &e0, 1, &r) && c.ptr_valid && c.ptr == 0x08);
        live_event_t e1 = {0};
        e1.rx[0] = 0x0800 | 0x65; e1.rx[1] = 0xAA; e1.rx[2] = 0xBB;
        e1.rx[3] = exp_pec_write(0x65, body, 2); e1.count = 4; e1.stop = true;
        C(live_wire_event(&w2, &e1, 2, &r));
        C(!c.ptr_valid && !st_resume_pointer(&c));
    }
    printf("  S7 pointer policy: retain after known read/command-only STOP, clear on unknown/write\n");
}

/* =====================================================================================
 * S8 - counter semantics that feed the bus telemetry (D04-G1/G8, C1..C3)
 *      These assert the CURRENT frozen behaviour; the report flags which are still
 *      open policy questions (not endorsements).
 * ===================================================================================== */
static void s8_counter_semantics(void) {
    st_target_t t;
    mk_word(&t);
    C(st_on_rx(&t, 0x09) && st_begin_read(&t));
    C(t.rx_bytes == 1);
    { uint8_t b = 0; for (int i = 0; i < 3; i++) C(st_on_tx(&t, &b)); }
    C(t.tx_bytes == 3);
    /* overread is counted per failing call, not per transaction (C1 semantics preserved) */
    { uint8_t b = 0; for (int i = 0; i < 3; i++) C(!st_on_tx(&t, &b)); }
    C(t.overreads == 3 && t.overread);
    st_on_nack(&t);
    C(st_on_stop(&t) == ST_OVERREAD);
    C(t.reads_done == 0 && t.tx_bytes == 3);
    /* 35-byte frame split across IRQ batches 16/16/3 (FIFO-16 is a batch limit, not a frame
       limit; historical P05 probe - never in the frozen ctest) */
    {
        uint8_t body[33];
        body[0] = 32;
        for (int k = 1; k < 33; k++) body[k] = (uint8_t)k;
        uint8_t f[35];
        f[0] = 0x66;
        memcpy(f + 1, body, 33);
        f[34] = exp_pec_write(0x66, body, 33);
        st_target_t q;
        live_wire_t w;
        live_reply_t r;
        mk(&q);
        C(st_add(&q, 0x66, ST_SHAPE_WRITE, 32) && st_seal(&q));
        live_wire_init(&w, &q, 0);
        for (int batch = 0; batch < 3; batch++) {
            live_event_t e = {0};
            unsigned off = batch == 0 ? 0 : batch == 1 ? 16 : 32;
            unsigned n = batch == 0 ? 16 : batch == 1 ? 16 : 3;
            for (unsigned k = 0; k < n; k++)
                e.rx[k] = (uint16_t)(f[off + k] | ((k == 0 && batch == 0) ? 0x0800 : 0));
            e.count = (uint8_t)n;
            e.stop = (batch == 2);
            C(live_wire_event(&w, &e, (uint32_t)(batch + 1), &r));
        }
        C(!w.blocked && q.writes_ok == 1);
        st_write_t sw;
        C(st_pop_write(&q, &sw) && sw.len == 33 && sw.body[0] == 32 && !memcmp(sw.body + 1, body + 1, 32));
    }
    printf("  S8 counters: rx/tx bytes exact, overread per-call documented, 35B write split 16/16/3\n");
}

/* =====================================================================================
 * S9 - restart-write through live_wire: a new FIRST byte drops the open partial write,
 *      only the second frame is queued (D03-G1, positive path)
 * ===================================================================================== */
static void s9_restart_write_through_wire(void) {
    st_target_t t;
    live_wire_t w;
    live_reply_t r;
    mk_cap(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.rx[0] = 0x0800 | 0x65; e.rx[1] = 0x11; e.count = 2; /* open partial 0x65 */
      C(live_wire_event(&w, &e, 1, &r)); }
    { const uint8_t b[2] = { 0xAA, 0xBB };
      live_event_t e = {0}; e.rx[0] = 0x0800 | 0x83; e.rx[1] = b[0]; e.rx[2] = b[1];
      e.rx[3] = exp_pec_write(0x83, b, 2); e.count = 4;
      C(live_wire_event(&w, &e, 2, &r)); } /* FIRST while active -> restart, old frame dropped */
    C(t.aborts == 1);
    { live_event_t e = {0}; e.stop = true; C(live_wire_event(&w, &e, 3, &r)); }
    { st_write_t sw; C(st_pop_write(&t, &sw) && sw.cmd == 0x83 && sw.len == 2); C(!st_pop_write(&t, &sw)); }
    C(t.writes_ok == 1);
    printf("  S9 restart-write: open 0x65 frame dropped (aborts=1), only 0x83 queued\n");
}

/* =====================================================================================
 * S10 - A8 short probe through live_wire is accepted (not isolated) and serves one byte
 * ===================================================================================== */
static void s10_a8_through_wire(void) {
    st_target_t t;
    live_wire_t w;
    live_reply_t r;
    mk_cap(&t);
    live_wire_init(&w, &t, 0);
    { live_event_t e = {0}; e.rx[0] = 0x0800 | 0xA8; e.count = 1;
      C(live_wire_event(&w, &e, 1, &r)); }
    { live_event_t e = {0}; e.request = true;
      C(live_wire_event(&w, &e, 2, &r) && r.have_tx && r.tx == 24); }
    { live_event_t e = {0}; e.nack = true; e.stop = true;
      C(live_wire_event(&w, &e, 3, &r)); }
    C(!w.blocked && t.reads_short == 1 && t.reads_done == 0);
    printf("  S10 A8 via wire: one count byte, short probe accepted, no fabricated frame\n");
}

int main(void) {
    s0_golden_pec_cross_check();
    s1_no_flat_fallback_sweep();
    s2_a8_negatives();
    s3_write_contract();
    s4_block_snapshot_mid_read();
    s5_guard_and_gating();
    s6_wire_matrix();
    s7_pointer_policy();
    s8_counter_semantics();
    s9_restart_write_through_wire();
    s10_a8_through_wire();
    printf("DS-REUSE-13 live contract: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
