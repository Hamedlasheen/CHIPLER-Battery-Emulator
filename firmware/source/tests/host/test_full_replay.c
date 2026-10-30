/* AG-REPLAY: full replay of EVERY valid battery frame (addr 0x0B, decoder status 0 = valid / 1 = short-A8) of
 * src/batt/capture_data.inc (8323 decoded frames, six original logic-analyzer captures) through the PRODUCTION
 * live_irq() (src/hal/live_port.c + live_wire.c + smbus_target.c + live_prepare.c), using the same DW_apb_i2c
 * FIFO/flag model as tests/host/test_live_port_mmio.c. Only the hw_* register primitives and SDK stubs are fake.
 *
 * Delivery policies (how bus events are grouped into IRQ invocations):
 *   P0  every bus event in its own IRQ (START, cmd byte, RESTART, each RD_REQ, RX_DONE, STOP, each write byte)
 *   P1  read: START + command byte + RESTART + first RD_REQ in ONE IRQ; write: START + all bytes in ONE IRQ
 *   P2  late ISR: read ends with RX_DONE+STOP in one IRQ, write ends with last byte(PEC)+STOP in one IRQ; the next
 *       transaction's START + first byte is delivered in the very next IRQ (no main-loop gap)
 *   P3  (extra stress, NOT in the brief's three) very late ISR: the previous transaction's tail (RX_DONE+STOP or
 *       PEC+STOP) is still pending when the next START + first byte arrive -> all in ONE IRQ. Run per boundary.
 *
 * Expected responses are built from the REGISTERED sample of the active profile (captured_responses.inc /
 * phone_test_responses.inc) + an INDEPENDENT CRC-8 (poly 0x07), never from the capture's own bytes.
 * Each profile is replayed as a full pass: the command sets of CAP12PM-B1 and P12PM-B2 are identical, so the
 * brief's "pick the profile that registers the command" rule selects CAP12PM-B1 for every frame; the P12PM-B2
 * pass is run in addition so both profiles are covered. Unknown commands -> expected isolation with 0 TX bytes.
 * Valid writes -> queued opaque (record compared to the capture bytes). A synthetic bad-PEC copy of every valid
 * write (PEC ^ 0x5A) is replayed right after it -> expected isolation, 0 TX, nothing queued.
 * NOT RUN ON HARDWARE: this is a host model of the peripheral, not evidence of electrical behaviour. */
#define LIVE_HW_ACCESS_OVERRIDE 1
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/regs/i2c.h"
#include "board_config.h"
#include "owner_policy.h"
/* Review 102: OWNER oracle. The owner build answers an unknown read with released-bus 0xFF (no isolation), accepts a
 * count-only read of any BLOCK as the short probe, and drops (accounts, does not isolate) a rejected write. Its
 * P12PM-B2 serves the 12 Pro capture rows, not the T06 table of PROFS[1], so the T06 pass runs for CAP12PM-B1 only.
 * Review 105 R3: P12PM-B2 is replayed against its OWN recording instead (replay_p12() below: original 12 Pro battery
 * boot capture, independent oracle, P0..P3, WORD + BLOCK + selector pages, negative controls). */
#define NPROF (OWNER ? 1 : 2)

#define STAT_RX_FULL 0x04u
static struct {
    uint16_t rx[64]; int rx_n, rx_rd;
    uint32_t stat;
    uint8_t tx[256]; int ntx;
    unsigned pins_released;
    uint32_t abort_src;
} M;
i2c_inst_t fake_i2c0 = {.index = 0}, fake_i2c1 = {.index = 1};
static uint32_t g_us;
uint32_t time_us_32(void) { return g_us; }
void gpio_init(unsigned p) { if (p == PIN_BAT_SDA || p == PIN_BAT_SCL) M.pins_released++; }
void gpio_set_dir(unsigned p, bool o) { (void)p; (void)o; }
void gpio_put(unsigned p, bool v) { (void)p; (void)v; }
void gpio_disable_pulls(unsigned p) { (void)p; }
void gpio_set_function(unsigned p, unsigned f) { (void)p; (void)f; }
uint32_t save_and_disable_interrupts(void) { return 0; }
void restore_interrupts(uint32_t s) { (void)s; }
static void (*g_handler)(void);
static bool g_irq_en;
void irq_set_enabled(unsigned n, bool e) { (void)n; g_irq_en = e; }
void irq_set_exclusive_handler(unsigned n, void (*h)(void)) { (void)n; g_handler = h; }
void irq_remove_handler(unsigned n, void (*h)(void)) { (void)n; (void)h; g_handler = 0; }
void irq_set_priority(unsigned n, unsigned p) { (void)n; (void)p; }
void i2c_init(i2c_inst_t *i, unsigned hz) { (void)i; (void)hz; }
void i2c_deinit(i2c_inst_t *i) { (void)i; }
void i2c_set_slave_mode(i2c_inst_t *i, bool s, uint8_t a) { (void)i; (void)s; (void)a; }

#define LIVE_HW static inline
LIVE_HW uint32_t hw_stat(i2c_hw_t *h) { (void)h; return M.stat; }
LIVE_HW uint32_t hw_rx_avail(i2c_hw_t *h) { (void)h; return (uint32_t)(M.rx_n - M.rx_rd); }
LIVE_HW uint32_t hw_rx_pop(i2c_hw_t *h) { (void)h; return M.rx[M.rx_rd++]; }
LIVE_HW void hw_tx_push(i2c_hw_t *h, uint8_t b) { (void)h; M.tx[M.ntx++] = b; }
LIVE_HW void hw_clr_rd_req(i2c_hw_t *h) { (void)h; M.stat &= ~I2C_IC_INTR_STAT_R_RD_REQ_BITS; }
LIVE_HW void hw_clr_rx_done(i2c_hw_t *h) { (void)h; M.stat &= ~I2C_IC_INTR_STAT_R_RX_DONE_BITS; }
LIVE_HW void hw_clr_stop(i2c_hw_t *h) { (void)h; M.stat &= ~I2C_IC_INTR_STAT_R_STOP_DET_BITS; }
LIVE_HW void hw_clr_start(i2c_hw_t *h) { (void)h; M.stat &= ~I2C_IC_INTR_STAT_R_START_DET_BITS; }
LIVE_HW void hw_clr_restart(i2c_hw_t *h) { (void)h; M.stat &= ~I2C_IC_INTR_STAT_R_RESTART_DET_BITS; }
LIVE_HW uint32_t hw_abort_source(i2c_hw_t *h) { (void)h; return M.abort_src; }

#include "hal/live_port.c"

#include "batt/live_prepare.h"
typedef struct { uint8_t cmd; st_shape_t shape; uint8_t count, data[32]; } captured_response_t;
typedef struct { uint8_t cmd, len; } captured_write_shape_t;
#include "batt/captured_responses.inc"
#include "batt/phone_test_responses.inc"
typedef struct { uint8_t addr; uint16_t nw, nr; uint8_t flags, expected, w[35], r[35]; } capture_frame_t;
#include "batt/capture_data.inc"

static unsigned fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* independent CRC-8 (poly 0x07), not the production st_pec_* */
static uint8_t crc8(uint8_t c, uint8_t b) { c ^= b; for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1)); return c; }
static uint8_t pec_read(uint8_t cmd, const uint8_t *d, int n) {
    uint8_t c = crc8(crc8(crc8(0, 0x0B << 1), cmd), (0x0B << 1) | 1);
    for (int i = 0; i < n; i++) c = crc8(c, d[i]);
    return c;
}
static uint8_t pec_write(const uint8_t *w, int n_incl_cmd) {
    uint8_t c = crc8(0, 0x0B << 1);
    for (int i = 0; i < n_incl_cmd; i++) c = crc8(c, w[i]);
    return c;
}

/* ---------------- profiles ---------------- */
typedef struct { const char *id; const captured_response_t *rows; unsigned n; } prof_t;
static const prof_t PROFS[2] = {
    {"CAP12PM-B1", captured_responses, sizeof captured_responses / sizeof captured_responses[0]},
    {"P12PM-B2", phone_test_responses, sizeof phone_test_responses / sizeof phone_test_responses[0]},
};
static const captured_response_t *prof_find(const prof_t *p, uint8_t cmd) {
    for (unsigned k = 0; k < p->n; k++) if (p->rows[k].cmd == cmd) return &p->rows[k];
    return 0;
}
static bool write_registered(uint8_t cmd, int nw) {   /* captured_writes[] shape check, independent of the engine */
    for (unsigned k = 0; k < sizeof captured_writes / sizeof captured_writes[0]; k++)
        if (captured_writes[k].cmd == cmd) {
            int body = nw - 2; unsigned len = captured_writes[k].len;
            return len == 2 ? body == 2 : body == (int)len + 1;
        }
    return false;
}

static st_target_t T;
static const prof_t *g_prof;
static unsigned bringups;
static void bring_up(void) {
    memset(&M, 0, sizeof M); memset(&fake_i2c1, 0, sizeof fake_i2c1); fake_i2c1.index = 1;
    live_port_stop(); live_port_clear_fault();
    st_init(&T, 0x0B);
    st_set_guard(&T, live_port_guard_enter, live_port_guard_exit, 0);
    static batt_profile_t p; memset(&p, 0, sizeof p);
    p.id = g_prof->id; p.status = strcmp(g_prof->id, "P12PM-B2") ? PROF_BENCH_TEST : PROF_PHONE_DATA_TEST; p.i2c_addr = 0x0B;
    CHECK(live_prepare(&T, &p, 50));
    CHECK(live_port_start(&T));
    CHECK(!latched && installed);
    g_us = 1000; bringups++;
}

/* ---------------- bus model ---------------- */
static void ev_start(bool restart) { M.stat |= restart ? I2C_IC_INTR_STAT_R_RESTART_DET_BITS : I2C_IC_INTR_STAT_R_START_DET_BITS; }
static void ev_wbyte(uint8_t b, bool first) { M.rx[M.rx_n++] = (uint16_t)(b | (first ? 0x800u : 0)); M.stat |= STAT_RX_FULL; }
static void ev_rdreq(void) { M.stat |= I2C_IC_INTR_STAT_R_RD_REQ_BITS; }
static void ev_nack(void) { M.stat |= I2C_IC_INTR_STAT_R_RX_DONE_BITS; }
static void ev_stop(void) { M.stat |= I2C_IC_INTR_STAT_R_STOP_DET_BITS; }
static void deliver(void) {
    i2c_hw_t *h = i2c_get_hw(BAT_I2C_INSTANCE);
    if (!h->enable) { M.stat = 0; M.rx_n = M.rx_rd = 0; return; }       /* peripheral off: nothing is seen */
    g_us += 50;
    if (g_irq_en && g_handler) g_handler();
    if (M.rx_rd == M.rx_n) { M.rx_n = M.rx_rd = 0; M.stat &= ~STAT_RX_FULL; }
}

/* Drive one frame. tail_pending: leave the final tail (RX_DONE+STOP / PEC+STOP) un-delivered (P3 only).
 * head_already: the START + first byte were already queued in the model by the caller (P3 merge). */
static void drive_read(int pol, uint8_t cmd, int nr, bool tail_pending) {
    switch (pol) {
    case 0:
        ev_start(false); deliver(); ev_wbyte(cmd, true); deliver(); ev_start(true); deliver();
        for (int i = 0; i < nr; i++) { ev_rdreq(); deliver(); }
        ev_nack(); deliver(); ev_stop(); deliver(); break;
    case 1:
        ev_start(false); ev_wbyte(cmd, true); ev_start(true); ev_rdreq(); deliver();
        for (int i = 1; i < nr; i++) { ev_rdreq(); deliver(); }
        ev_nack(); deliver(); ev_stop(); deliver(); break;
    default:   /* 2, 3 */
        ev_start(false); ev_wbyte(cmd, true); deliver();
        ev_start(true);
        for (int i = 0; i < nr; i++) { ev_rdreq(); deliver(); }
        ev_nack(); ev_stop(); if (!tail_pending) deliver(); break;
    }
}
static void drive_write(int pol, const uint8_t *w, int nw, bool tail_pending) {
    switch (pol) {
    case 0:
        ev_start(false); deliver();
        for (int i = 0; i < nw; i++) { ev_wbyte(w[i], i == 0); deliver(); }
        ev_stop(); deliver(); break;
    case 1:
        ev_start(false); for (int i = 0; i < nw; i++) ev_wbyte(w[i], i == 0); deliver();
        ev_stop(); deliver(); break;
    default:
        ev_start(false); ev_wbyte(w[0], true); deliver();
        for (int i = 1; i < nw - 1; i++) { ev_wbyte(w[i], false); deliver(); }
        ev_wbyte(w[nw - 1], false); ev_stop(); if (!tail_pending) deliver(); break;
    }
}

/* ---------------- grading ---------------- */
enum { O_SERVED, O_EXP_ISO, O_UNEXP_ISO, O_WRONG, O_FABR, O_MISSED_ISO, O_N };
static const char *ONAME[O_N] = {"served-correct", OWNER ? "expected-reject (owner: drop/0xFF)" : "expected-isolation", "UNEXPECTED isolation", "wrong byte", "fabricated byte", "missed isolation"};
enum { K_READ, K_SHORTA8, K_WRITE, K_BADPEC, K_UNKNOWN, K_N };
static const char *KNAME[K_N] = {"read (full)", "read short-A8", "write valid PEC", "write bad PEC (synthetic)", "read unknown cmd"};
static unsigned tab[4][2][K_N][O_N];
static unsigned residual[4][2];     /* non-isolated frame left flags/FIFO/txn state behind */
static unsigned capture_eq_sample[2], capture_ne_sample[2];
static unsigned unexp_logged;

static void log_unexpected(int pol, int pi, unsigned fi, const char *what) {
    if (unexp_logged++ >= 40) return;
    st_result_t r; uint32_t irq, ab; live_port_diagnostic(&r, &irq, &ab);
    printf("  UNEXPECTED pol=P%d prof=%s frame=%u %s fault=%d last_irq=0x%X\n", pol, PROFS[pi].id, fi, what, (int)r, (unsigned)irq);
}

/* Build the expected wire bytes from the registered sample (independent CRC-8). Returns full wire length. */
static int expected_wire(const captured_response_t *r, uint8_t *exp) {
    int n = 0;
    if (r->shape == ST_SHAPE_BLOCK) exp[n++] = r->count;
    memcpy(exp + n, r->data, r->count); n += r->count;
    exp[n] = pec_read(r->cmd, exp, n);
#ifdef REPLAY_MUTATE   /* grader self-check only: corrupt the expected PEC of one command -> must show as "wrong byte" */
    if (r->cmd == 0x86) exp[n] ^= 1;
#endif
    return n + 1;
}

/* Grade a read frame given the TX bytes the IRQ produced and whether the port isolated. */
static int grade_read(const capture_frame_t *f, const captured_response_t *reg, const uint8_t *tx, int ntx, bool iso, int *kind) {
    uint8_t exp[40]; int full = 0, want;
    bool expect_iso;
    if (!reg) {
        *kind = K_UNKNOWN; expect_iso = true; want = 0;
        if (OWNER) {   /* released-bus 0xFF on every requested byte, port stays up */
            for (int k = 0; k < ntx; k++) if (tx[k] != 0xFF) return O_FABR;
            return iso ? O_UNEXP_ISO : (ntx == f->nr ? O_EXP_ISO : O_FABR);
        }
    }
    else {
        full = expected_wire(reg, exp);
        bool probe = OWNER ? (reg->shape == ST_SHAPE_BLOCK && f->nr == 1) : (reg->cmd == 0xA8 && full == 26 && f->nr == 1);
        if (f->expected == 1) { *kind = K_SHORTA8; want = 1; expect_iso = !probe; }
        else if (probe) { *kind = K_SHORTA8; want = 1; expect_iso = false; }   /* owner: count-only probe of a block */
        else { *kind = K_READ; want = full; expect_iso = f->nr != full; }   /* length mismatch capture vs profile */
        if (expect_iso) want = f->nr < full ? f->nr : full;                  /* bytes legitimately sent before the fault */
    }
    for (int k = 0; k < ntx; k++) {
        if (k >= want && !(expect_iso && reg && k < full)) return O_FABR;
        if (k < full && tx[k] != exp[k]) return O_WRONG;
    }
    if (expect_iso) return iso ? (reg ? O_EXP_ISO : (ntx == 0 ? O_EXP_ISO : O_FABR)) : O_MISSED_ISO;
    if (iso) return O_UNEXP_ISO;
    return ntx == want ? O_SERVED : O_UNEXP_ISO;
}

static bool frame_valid(const capture_frame_t *f) { return f->addr == 11 && (f->expected == 0 || f->expected == 1); }

/* Drain the queue (main-loop consumer) and check the record of a write that should have been accepted. */
static bool drain_check(const uint8_t *w, int nw, bool expect_one) {
    st_write_t rec; unsigned q = 0; bool match = false;
    while (st_pop_write(&T, &rec)) {
        q++;
        if (expect_one && rec.cmd == w[0] && rec.len == nw - 2 && !memcmp(rec.body, w + 1, (size_t)(nw - 2))) match = true;
    }
    return expect_one ? (q == 1 && match) : q == 0;
}

static void run_write(int pol, int pi, unsigned fi, const uint8_t *w, int nw, bool bad) {
    int kind = bad ? K_BADPEC : K_WRITE;
    bool expect_ok = !bad && write_registered(w[0], nw) && pec_write(w, nw - 1) == w[nw - 1];
    int nt = M.ntx;
    drive_write(pol, w, nw, false);
    bool iso = latched;
    int o;
    if (M.ntx != nt) o = O_FABR;
    else if (expect_ok) o = iso ? O_UNEXP_ISO : (drain_check(w, nw, true) ? O_SERVED : O_WRONG);
    else if (OWNER) o = iso ? O_UNEXP_ISO : (drain_check(w, nw, false) ? O_EXP_ISO : O_FABR);   /* owner: dropped, port up */
    else o = iso ? (drain_check(w, nw, false) ? O_EXP_ISO : O_FABR) : O_MISSED_ISO;
    if (o == O_UNEXP_ISO) log_unexpected(pol, pi, fi, bad ? "bad-pec write" : "write");
    tab[pol][pi][kind][o]++;
    if (!iso && (M.stat != 0 || T.active)) residual[pol][pi]++;
    if (iso) bring_up();
}

static void run_read(int pol, int pi, unsigned fi, const capture_frame_t *f) {
    const captured_response_t *reg = prof_find(&PROFS[pi], f->w[0]);
    int nt = M.ntx;
    drive_read(pol, f->w[0], f->nr, false);
    bool iso = latched; int kind;
    int o = grade_read(f, reg, M.tx + nt, M.ntx - nt, iso, &kind);
    if (o == O_UNEXP_ISO || o == O_WRONG || o == O_FABR) { char b[48]; snprintf(b, sizeof b, "read cmd=0x%02X nr=%u", f->w[0], f->nr); log_unexpected(pol, pi, fi, b); }
    tab[pol][pi][kind][o]++;
    if (!iso && (M.stat != 0 || T.active)) residual[pol][pi]++;
    /* informational: does the capture's own payload equal the registered sample? (live values differ by session) */
    if (pol == 0 && reg && kind == K_READ) {
        uint8_t exp[40]; int full = expected_wire(reg, exp);
        if (full == f->nr && !memcmp(exp, f->r, (size_t)full)) capture_eq_sample[pi]++; else capture_ne_sample[pi]++;
    }
    if (iso) bring_up();
    if (M.ntx > 200) M.ntx = 0;   /* keep the TX log bounded; grading uses deltas */
}

static void run_stream(int pol, int pi) {
    g_prof = &PROFS[pi]; bring_up();
    for (unsigned i = 0; i < CAPTURE_FRAME_COUNT; i++) {
        const capture_frame_t *f = &capture_frames[i];
        if (!frame_valid(f)) continue;
        if (f->flags & 1) run_read(pol, pi, i, f);
        else {
            run_write(pol, pi, i, f->w, f->nw, false);
            uint8_t bad[35]; memcpy(bad, f->w, f->nw); bad[f->nw - 1] ^= 0x5A;
            run_write(pol, pi, i, bad, f->nw, true);
        }
        if (M.ntx > 200) M.ntx = 0;
        live_port_service(g_us);   /* main loop: idle-expiry check (never fires here: gaps are 50 us) */
    }
}

/* ---------------- P3: per boundary, previous tail coalesced with next START + first byte ---------------- */
static unsigned p3_pairs[2][2], p3_iso[2][2], p3_served_prev_tx[2][2], p3_wrong[2][2];
static void run_p3(int pi) {
    g_prof = &PROFS[pi];
    unsigned prev = ~0u;
    for (unsigned i = 0; i < CAPTURE_FRAME_COUNT; i++) {
        if (!frame_valid(&capture_frames[i])) { prev = ~0u; continue; }   /* only directly consecutive valid frames */
        if (prev == ~0u) { prev = i; continue; }
        const capture_frame_t *a = &capture_frames[prev], *b = &capture_frames[i];
        bring_up();
        int ka = (a->flags & 1) ? 0 : 1, kb = (b->flags & 1) ? 0 : 1;
        int nt = M.ntx;
        if (ka == 0) drive_read(3, a->w[0], a->nr, true); else drive_write(3, a->w, a->nw, true);
        /* previous frame's TX must still be exactly right up to the pending tail */
        if (ka == 0) {
            const captured_response_t *reg = prof_find(g_prof, a->w[0]); uint8_t exp[40]; int full = reg ? expected_wire(reg, exp) : 0;
            int want = a->expected == 1 ? 1 : full;
            if (OWNER && !reg) { want = a->nr; memset(exp, 0xFF, sizeof exp); }          /* owner 0xFF fill */
            else if (OWNER && reg && reg->shape == ST_SHAPE_BLOCK && a->nr == 1) want = 1;  /* owner count probe */
            if (M.ntx - nt == want && !memcmp(M.tx + nt, exp, (size_t)want)) p3_served_prev_tx[pi][ka]++; else p3_wrong[pi][ka]++;
        } else p3_served_prev_tx[pi][ka]++;
        /* next frame's START + first byte arrive before the ISR ran for the tail */
        ev_start(false); ev_wbyte(b->w[0], true); nt = M.ntx; deliver();
        p3_pairs[pi][ka]++;
        if (latched) p3_iso[pi][ka]++;
        if (M.ntx != nt) p3_wrong[pi][ka]++;   /* a fabricated byte at the boundary */
        (void)kb; prev = i;
    }
}

#if OWNER && CHIPLER_12P_REFERENCE
/* ---------------- review 105 R3: the phone's own profile (P12PM-B2, 12 Pro capture rows) against the original battery ----
 * Frames: tests/host/p12p_power_frames.inc, generated from Claude's text decode of the original 12 Pro boot capture
 * (POWER). The served data comes from src/batt/ds12p_*.inc (DeepSeek generator, another decode of the same .sr), so the
 * oracle does not share code or tables with the firmware. The target is armed by live_setup_prepare(), the same function
 * battery_bus.c uses (evidence gate + selector pages + hooks), and driven through the production live_irq() with the
 * delivery policies P0..P2 and P3 (every boundary: previous tail + next START + first byte in ONE IRQ).
 * Oracle per read frame: the TX bytes must equal one response the ORIGINAL battery sent for the same command (and, for the
 * selector targets 0x66/0x74, the same selector page latched by the preceding 0x65/0x73 write in the capture); the PEC is
 * checked with the independent crc8 above. Count probe: the one byte the battery sent. Write: no TX byte, no isolation.
 * Dynamic words (0x0D, 0x09, ...) change inside the recording: membership in the recorded set is the strongest claim
 * possible for them; static words/blocks have one recorded payload, i.e. exact equality.
 * 0x0D/0x0F/0x09 are rewritten by main.c (owner_words) once replies run; this replay covers the armed map, which is what
 * owner_initial_words() starts from (bus_order_tests covers that step). */
typedef struct { char kind; uint8_t cmd, n, b[34]; const char *t; } p12_frame_t;
#include "p12p_power_frames.inc"
#include "batt/live_setup.h"
static live_setup_t S12;
static void bring_up12(void) {
    memset(&M, 0, sizeof M); memset(&fake_i2c1, 0, sizeof fake_i2c1); fake_i2c1.index = 1;
    live_port_stop(); live_port_clear_fault();
    st_init(&T, 0x0B);
    st_set_guard(&T, live_port_guard_enter, live_port_guard_exit, 0);
    const batt_profile_t *p = profile_get(profile_find("P12PM-B2"));
    bool evid = false;
    CHECK(p && live_setup_prepare(&S12, &T, p, 50, &evid) && evid);
    CHECK(live_port_start(&T));
    CHECK(!latched && installed);
    g_us = 1000;
}
static uint16_t sel_key(uint8_t cmd, uint16_t s65, uint16_t s73) { return cmd == 0x66 ? s65 : cmd == 0x74 ? s73 : 0xFFFF; }
/* the selector page each frame index sees (in capture order) */
static uint16_t g_key[P12_FRAME_COUNT];
static int g_w65[P12_FRAME_COUNT], g_w73[P12_FRAME_COUNT];   /* index of the latest 0x65 / 0x73 write before frame i, -1 none */
static void build_keys(void) {
    uint16_t s65 = 0xFFFE, s73 = 0xFFFE;   /* no page latched yet */
    int w65 = -1, w73 = -1;
    for (unsigned i = 0; i < P12_FRAME_COUNT; i++) {
        const p12_frame_t *f = &P12_FRAMES[i];
        g_w65[i] = w65; g_w73[i] = w73;
        if (f->kind == 'W' && f->n >= 4 && f->cmd == 0x65) { s65 = (uint16_t)(f->b[1] | f->b[2] << 8); w65 = (int)i; }
        if (f->kind == 'W' && f->n >= 4 && f->cmd == 0x73) { s73 = (uint16_t)(f->b[1] | f->b[2] << 8); w73 = (int)i; }
        g_key[i] = sel_key(f->cmd, s65, s73);
    }
}
/* is tx[0..n) one of the responses the original battery sent for (cmd, key)? exact = only one distinct payload exists */
static bool recorded(uint8_t cmd, uint16_t key, const uint8_t *tx, int n, bool *exact) {
    bool hit = false, same = true; const p12_frame_t *first = 0;
    for (unsigned i = 0; i < P12_FRAME_COUNT; i++) {
        const p12_frame_t *f = &P12_FRAMES[i];
        if (f->kind != 'R' || f->cmd != cmd || g_key[i] != key) continue;
        if (!first) first = f; else if (f->n != first->n || memcmp(f->b, first->b, f->n)) same = false;
        if (f->n == n && !memcmp(f->b, tx, (size_t)n)) hit = true;
    }
    *exact = first && same;
    return hit;
}
enum { M_NONE, M_WORD_0x10, M_BLOCK_0xA8, M_NO_SELECTOR };
typedef struct { unsigned reads, exact, set, known, probes, writes, wrong, fabricated, iso, residual, pec_bad, wrong_cmd[256]; } r12_t;
/* Review 110 N1 (fixed in r12): the firmware served the 4.323753 s bit-slipped copy of page 0x65/0x0B -> 0x66 until r11.
 * These are those old bytes: the oracle must reject them (negative control), and the served page must now be recorded. */
static const uint8_t N1_OLD_SLIPPED[34] = {0x20,0x5E,0x90,0x00,0x00,0x54,0x8D,0x00,0x00,0xF6,0x89,0x00,0x00,0x9C,0x8C,0x00,0x00,
    0xDA,0x44,0x00,0x00,0x4D,0x44,0x00,0x00,0x09,0x42,0x00,0x00,0x7E,0x44,0x00,0x00,0x70};
static void mutate_map(int mut) {
    if (mut == M_WORD_0x10) { uint16_t v; CHECK(st_get_word(&T, 0x10, &v) && st_set_word(&T, 0x10, (uint16_t)(v ^ 1))); }
    if (mut == M_BLOCK_0xA8)
        for (unsigned k = 0; k < T.n_regs; k++)
            if (T.regs[k].cmd == 0xA8) { uint8_t d[32]; memcpy(d, T.regs[k].data, T.regs[k].len); d[3] ^= 0x10; CHECK(st_set_block(&T, 0xA8, d, T.regs[k].len)); }
}
static unsigned wrong_logged;
static void grade12(unsigned i, const uint8_t *tx, int ntx, r12_t *R) {
    const p12_frame_t *f = &P12_FRAMES[i];
    bool bad;
    if (f->kind == 'P') { R->probes++; bad = ntx != 1 || tx[0] != f->b[0]; }
    else {
        R->reads++;
        bool exact = false;
        bad = ntx != f->n || !recorded(f->cmd, g_key[i], tx, ntx, &exact);
        if (!bad) { if (exact) R->exact++; else R->set++; }
        if (f->cmd == 0x66 && g_key[i] == 0x000B) R->known += ntx == 34 && !memcmp(tx, N1_OLD_SLIPPED, 34);   /* must stay 0 */
        if (ntx == f->n && pec_read(f->cmd, tx, ntx - 1) != tx[ntx - 1]) R->pec_bad++;
    }
    if (!bad) return;
    R->wrong++; R->wrong_cmd[f->cmd]++;
    if (wrong_logged++ < 8) {
        printf("  frame %u t=%ss %c cmd=0x%02X key=0x%04X tx(%d):", i, f->t, f->kind, f->cmd, g_key[i], ntx);
        for (int k = 0; k < ntx && k < 12; k++) printf(" %02X", tx[k]);
        printf(" | recorded:"); for (int k = 0; k < f->n && k < 12; k++) printf(" %02X", f->b[k]); printf("\n");
    }
}
/* one frame through the IRQ model; returns the number of TX bytes it produced */
static int drive12(int pol, unsigned i, bool tail_pending, int mut) {
    const p12_frame_t *f = &P12_FRAMES[i];
    int nt = M.ntx;
    if (f->kind == 'W') {
        if (mut == M_NO_SELECTOR && (f->cmd == 0x65 || f->cmd == 0x73)) return 0;   /* drop the page selects */
        drive_write(pol, f->b, f->n, tail_pending);
        st_write_t w; while (st_pop_write(&T, &w)) {}
    } else drive_read(pol, f->cmd, f->kind == 'P' ? 1 : f->n, tail_pending);
    return M.ntx - nt;
}
/* P0..P2: the whole recording as one continuous stream (selector state carried by the wire path) */
static void run_p12(int pol, int mut, r12_t *R) {
    memset(R, 0, sizeof *R);
    bring_up12(); mutate_map(mut);
    for (unsigned i = 0; i < P12_FRAME_COUNT; i++) {
        const p12_frame_t *f = &P12_FRAMES[i];
        int nt = M.ntx, n = drive12(pol, i, false, mut);
        if (f->kind == 'W') { R->writes++; if (n) R->fabricated++; }
        else grade12(i, M.tx + nt, n, R);
        if (latched) { R->iso++; bring_up12(); mutate_map(mut); }
        if (!latched && (M.stat != 0 || T.active)) R->residual++;
        if (M.ntx > 200) M.ntx = 0;
        live_port_service(g_us);
    }
}
/* P3, as the T06 pass: per boundary of consecutive frames a fresh target (selector pages restored by re-sending the
 * capture's latest 0x65/0x73 writes), frame a with its tail pending, then b's START + first byte in the SAME IRQ.
 * Fail-closed contract: a's bytes are graded by the oracle, the boundary IRQ may isolate (counted), it never sends a byte. */
static void run_p3_12(r12_t *R, unsigned *pairs, unsigned *iso) {
    memset(R, 0, sizeof *R); *pairs = *iso = 0;
    for (unsigned i = 1; i < P12_FRAME_COUNT; i++) {
        unsigned a = i - 1;
        bring_up12();
        if (g_w65[a] >= 0) drive12(0, (unsigned)g_w65[a], false, M_NONE);
        if (g_w73[a] >= 0) drive12(0, (unsigned)g_w73[a], false, M_NONE);
        CHECK(!latched);
        int nt = M.ntx, n = drive12(3, a, true, M_NONE);
        if (P12_FRAMES[a].kind == 'W') { R->writes++; if (n) R->fabricated++; }
        else grade12(a, M.tx + nt, n, R);
        ev_start(false); ev_wbyte(P12_FRAMES[i].cmd, true); nt = M.ntx; deliver();
        (*pairs)++;
        if (latched) (*iso)++;
        if (M.ntx != nt) R->fabricated++;
    }
}
/* Review 111 N2 (applied in r13): selector page 0x65/4 -> 0x66 of the POWER table. The text-decode oracle above never saw
 * this page (both recorded reads are PEC-BAD in that decode), so it is checked here directly through live_irq(): the bytes
 * are the RAW capture decoded with a 3-sample glitch filter at 4.272404 s and 5.254880 s (captured PEC 0x09), identical to the
 * DS073 f3 log and the CHARGER table; the old r7..r12 bytes are the unfiltered, bit-slipped decode. */
static const uint8_t N2_RAW_PAGE4[34] = {0x20,0xDB,0x53,0x01,0x00,0x6E,0x49,0x01,0x00,0x42,0x3D,0x01,0x00,0xEA,0x21,0x01,0x00,
    0x33,0x07,0x01,0x00,0x3B,0xFA,0x00,0x00,0x25,0xF0,0x00,0x00,0xC3,0xE1,0x00,0x00,0x09};
static const uint8_t N2_OLD_SLIPPED[34] = {0x20,0xDB,0x53,0x01,0x00,0x6E,0x49,0x00,0x00,0x21,0x1E,0x00,0x00,0x75,0x10,0x00,0x00,
    0x19,0x03,0x00,0x00,0x1D,0x7D,0x00,0x00,0x12,0x78,0x00,0x00,0x61,0x70,0x00,0x00,0xAF};
static void page4_n2(void) {
    for (int pol = 0; pol < 3; pol++) {
        bring_up12();
        uint8_t w[4] = {0x65, 0x04, 0x00, 0}; w[3] = pec_write(w, 3);
        int nt = M.ntx; drive_write(pol, w, 4, false); CHECK(M.ntx == nt && !latched);
        nt = M.ntx; drive_read(pol, 0x66, 34, false);
        CHECK(!latched && M.ntx - nt == 34);
        CHECK(!memcmp(M.tx + nt, N2_RAW_PAGE4, 34));                    /* the original battery's bytes and PEC */
        CHECK(memcmp(M.tx + nt, N2_OLD_SLIPPED, 34) != 0);              /* negative control: never the slipped copy */
        CHECK(pec_read(0x66, M.tx + nt, 33) == M.tx[nt + 33]);          /* independent crc8 */
    }
    printf("N2: page 0x65/4 -> 0x66 through live_irq (P0..P2) = raw capture bytes, PEC 0x09; old slipped bytes never served\n");
}
static void replay_p12(void) {
    build_keys();
    { bool ex; CHECK(!recorded(0x66, 0x000B, N1_OLD_SLIPPED, 34, &ex));   /* N1 negative control: the old bytes are not a recording */
      unsigned n0b = 0; for (unsigned i = 0; i < P12_FRAME_COUNT; i++) n0b += P12_FRAMES[i].kind == 'R' && P12_FRAMES[i].cmd == 0x66 && g_key[i] == 0x000B;
      CHECK(n0b == 1); }
    unsigned nr = 0, np = 0, nw = 0;
    for (unsigned i = 0; i < P12_FRAME_COUNT; i++) { nr += P12_FRAMES[i].kind == 'R'; np += P12_FRAMES[i].kind == 'P'; nw += P12_FRAMES[i].kind == 'W'; }
    printf("\nreview 105 R3 - P12PM-B2 (owner) vs the original 12 Pro battery, POWER boot capture: %u reads, %u count probes, %u writes\n", nr, np, nw);
    CHECK(nr == 946 && np == 55 && nw == 155);
    printf("| policy | reads | exact (static) | in recorded set (dynamic) | old N1 bytes served | probes | writes | wrong | fabricated | isolations | residual | PEC bad |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n");
    static r12_t R;
    for (int pol = 0; pol < 3; pol++) {
        run_p12(pol, M_NONE, &R);
        printf("| P%d | %u | %u | %u | %u | %u | %u | %u | %u | %u | %u | %u |\n", pol, R.reads, R.exact, R.set, R.known, R.probes, R.writes, R.wrong, R.fabricated, R.iso, R.residual, R.pec_bad);
        CHECK(R.reads == nr && R.probes == np && R.writes == nw);
        CHECK(R.wrong == 0 && R.fabricated == 0 && R.iso == 0 && R.residual == 0 && R.pec_bad == 0 && R.known == 0);
    }
    unsigned pairs, iso;
    run_p3_12(&R, &pairs, &iso);
    printf("| P3 per boundary | %u | %u | %u | %u | %u | %u | %u | %u | %u of %u boundaries (fail-closed) | - | %u |\n", R.reads, R.exact, R.set, R.known, R.probes, R.writes, R.wrong, R.fabricated, iso, pairs, R.pec_bad);
    CHECK(pairs == P12_FRAME_COUNT - 1 && R.reads == nr - (P12_FRAMES[P12_FRAME_COUNT - 1].kind == 'R') && R.known == 0);
    CHECK(R.wrong == 0 && R.fabricated == 0 && R.pec_bad == 0);   /* the boundary may isolate (fail-closed), never send */
    /* negative controls: a corrupted map / lost selector state MUST be caught by this oracle */
    wrong_logged = ~0u / 2;   /* the expected failures below are counted, not listed */
    unsigned n10 = 0, nA8 = 0, n66 = 0;
    for (unsigned i = 0; i < P12_FRAME_COUNT; i++) if (P12_FRAMES[i].kind == 'R') {
        n10 += P12_FRAMES[i].cmd == 0x10; nA8 += P12_FRAMES[i].cmd == 0xA8; n66 += P12_FRAMES[i].cmd == 0x66 || P12_FRAMES[i].cmd == 0x74; }
    run_p12(1, M_WORD_0x10, &R);
    printf("mutation WORD 0x10 bit flip: wrong=%u (0x10 reads %u)\n", R.wrong, n10);
    CHECK(R.wrong_cmd[0x10] == n10 && R.wrong == n10);
    run_p12(2, M_BLOCK_0xA8, &R);
    printf("mutation BLOCK 0xA8 byte flip: wrong=%u (0xA8 full reads %u)\n", R.wrong, nA8);
    CHECK(R.wrong_cmd[0xA8] == nA8 && nA8 > 0);
    run_p12(0, M_NO_SELECTOR, &R);
    printf("mutation selector writes dropped: wrong=%u (0x66/0x74 reads %u)\n", R.wrong, n66);
    CHECK(R.wrong_cmd[0x66] + R.wrong_cmd[0x74] > 0);
}
#endif

int main(void) {
    unsigned nread = 0, nshort = 0, nwrite = 0, pec_ok = 0;
    for (unsigned i = 0; i < CAPTURE_FRAME_COUNT; i++) {
        const capture_frame_t *f = &capture_frames[i];
        if (!frame_valid(f)) continue;
        if (f->flags & 1) { if (f->expected == 1) nshort++; else nread++; }
        else { nwrite++; if (pec_write(f->w, f->nw - 1) == f->w[f->nw - 1]) pec_ok++; }
    }
    printf("capture_data.inc: %u frames; valid battery frames: reads=%u shortA8=%u writes=%u (write PEC ok by independent CRC: %u)\n",
           (unsigned)CAPTURE_FRAME_COUNT, nread, nshort, nwrite, pec_ok);
    CHECK(nread == 3444 && nshort == 19 && nwrite == 86 && pec_ok == 86);
    { unsigned same = 0; for (unsigned k = 0; k < PROFS[0].n; k++) same += prof_find(&PROFS[1], PROFS[0].rows[k].cmd) != 0;
      printf("profile command sets: CAP12PM-B1=%u P12PM-B2=%u common=%u\n", PROFS[0].n, PROFS[1].n, same); }

    for (int pol = 0; pol < 3; pol++)
        for (int pi = 0; pi < NPROF; pi++) run_stream(pol, pi);

    printf("\n| policy | profile | frame kind | frames |");
    for (int o = 0; o < O_N; o++) printf(" %s |", ONAME[o]);
    printf("\n|---|---|---|---|"); for (int o = 0; o < O_N; o++) printf("---|"); printf("\n");
    unsigned bad_total = 0;
    for (int pol = 0; pol < 3; pol++)
        for (int pi = 0; pi < NPROF; pi++) {
            unsigned sum[O_N] = {0}, tot = 0;
            for (int k = 0; k < K_N; k++) {
                unsigned n = 0; for (int o = 0; o < O_N; o++) { n += tab[pol][pi][k][o]; sum[o] += tab[pol][pi][k][o]; }
                tot += n;
                if (!n) continue;
                printf("| P%d | %s | %s | %u |", pol, PROFS[pi].id, KNAME[k], n);
                for (int o = 0; o < O_N; o++) printf(" %u |", tab[pol][pi][k][o]);
                printf("\n");
            }
            printf("| **P%d** | **%s** | **all** | **%u** |", pol, PROFS[pi].id, tot);
            for (int o = 0; o < O_N; o++) printf(" **%u** |", sum[o]);
            printf("\n");
            bad_total += sum[O_UNEXP_ISO] + sum[O_WRONG] + sum[O_FABR] + sum[O_MISSED_ISO];
            CHECK(sum[O_UNEXP_ISO] == 0 && sum[O_WRONG] == 0 && sum[O_FABR] == 0 && sum[O_MISSED_ISO] == 0);
            CHECK(residual[pol][pi] == 0);
        }
    printf("\nresidual state after non-isolated frames (flags/FIFO/open txn): P0=%u/%u P1=%u/%u P2=%u/%u\n",
           residual[0][0], residual[0][1], residual[1][0], residual[1][1], residual[2][0], residual[2][1]);
    printf("capture payload == registered sample (informational, P0): CAP12PM-B1 %u equal / %u differ; P12PM-B2 %u equal / %u differ\n",
           capture_eq_sample[0], capture_ne_sample[0], capture_eq_sample[1], capture_ne_sample[1]);
    printf("bring_up calls (initial + after each isolation): %u\n", bringups);

    /* extra stress P3 */
    for (int pi = 0; pi < NPROF; pi++) run_p3(pi);
    printf("\nP3 (very late ISR: prev tail + next START+first byte in ONE IRQ), per boundary of directly consecutive valid frames:\n");
    for (int pi = 0; pi < NPROF; pi++)
        printf("  %s: after-read boundaries=%u isolated=%u | after-write boundaries=%u isolated=%u | prev TX correct=%u wrong/fabricated=%u\n",
               PROFS[pi].id, p3_pairs[pi][0], p3_iso[pi][0], p3_pairs[pi][1], p3_iso[pi][1],
               p3_served_prev_tx[pi][0] + p3_served_prev_tx[pi][1], p3_wrong[pi][0] + p3_wrong[pi][1]);
    for (int pi = 0; pi < NPROF; pi++) CHECK(p3_wrong[pi][0] + p3_wrong[pi][1] == 0);   /* fail-closed: never a wrong byte */

#if OWNER && CHIPLER_12P_REFERENCE
    replay_p12();
    page4_n2();
#endif
    printf("\nfull replay: %u checks, %u failures, %u bad outcomes in P0..P2: %s\n", checks, fails, bad_total, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
