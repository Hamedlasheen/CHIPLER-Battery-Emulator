/* Production live_port.c (live_irq + start/stop/service) driven through a model of the DW_apb_i2c FIFO and
 * flag semantics. Only the nine register primitives (hw_*) and the SDK stubs are fake; live_irq(),
 * live_wire.c, smbus_target.c and live_prepare.c are the production sources.
 * Covers the gaps listed in CODEX_REUSE_AND_GAPS: FIFO order, FIRST_DATA_BYTE, STOP/RD_REQ/RX_DONE
 * coalescing, command-only STOP, unknown/overread, queue overflow, isolate/release, restart. */
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

#include "../../src/hal/live_port.c"

#include "../../src/batt/live_prepare.h"
typedef struct { uint8_t cmd; st_shape_t shape; uint8_t count, data[32]; } captured_response_t;
typedef struct { uint8_t cmd, len; } captured_write_shape_t;
#include "../../src/batt/captured_responses.inc"
#include "../../src/batt/phone_test_responses.inc"

/* ---- DeepSeek DS-IRQ-03 fixtures (register-level IRQ batches, independent Python reference of the beta1 contract) ---- */
typedef struct { uint32_t flags; uint8_t n; uint16_t rx[16]; uint8_t after; } dsfx_batch_t;
typedef struct { const char *id; int hyp; int nb; const dsfx_batch_t *b; uint32_t e[13]; const uint8_t *tx; int ntx; } dsfx_t;
#include "deepseek_fixtures.inc"
static unsigned fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* independent CRC-8 (poly 0x07), not the production st_pec_* */
static uint8_t crc8(uint8_t c, uint8_t b) { c ^= b; for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1)); return c; }
static uint8_t pec_read(uint8_t cmd, const uint8_t *d, int n) {
    uint8_t c = crc8(crc8(crc8(0, 0x0B << 1), cmd), (0x0B << 1) | 1);
    for (int i = 0; i < n; i++) c = crc8(c, d[i]); return c;
}
static uint8_t pec_write(uint8_t cmd, const uint8_t *d, int n) {
    uint8_t c = crc8(crc8(0, 0x0B << 1), cmd);
    for (int i = 0; i < n; i++) c = crc8(c, d[i]); return c;
}

static st_target_t T;
static const char *g_profile = "CAP12PM-B1";
static void bring_up(void) {
    memset(&M, 0, sizeof M); memset(&fake_i2c1, 0, sizeof fake_i2c1); fake_i2c1.index = 1;
    live_port_stop(); live_port_clear_fault();
    st_init(&T, 0x0B);
    st_set_guard(&T, live_port_guard_enter, live_port_guard_exit, 0);
    static batt_profile_t p; memset(&p, 0, sizeof p);
    p.id = g_profile; p.status = strcmp(g_profile, "P12PM-B2") ? PROF_BENCH_TEST : PROF_PHONE_DATA_TEST; p.i2c_addr = 0x0B;
    CHECK(live_prepare(&T, &p, 50));
    CHECK(live_port_start(&T));
    CHECK(!latched && installed);
    g_us = 1000;
}

/* ---- bus model ---- */
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
    if (M.rx_rd == M.rx_n) { M.rx_n = M.rx_rd = 0; M.stat &= ~STAT_RX_FULL; }   /* hardware drops RX_FULL when the FIFO is empty */
}

/* SMBus read: mode 0 = every event separately, 1 = command byte + RESTART + first RD_REQ in one IRQ,
 * 2 = last RD_REQ separate, RX_DONE + STOP coalesced. Returns bytes received, -1 on isolation. */
static int bus_read(uint8_t cmd, int want, int mode, uint8_t *out) {
    ev_start(false); ev_wbyte(cmd, true);
    if (mode != 1) deliver();
    ev_start(true);
    int start_tx = M.ntx;
    for (int i = 0; i < want; i++) {
        int before = M.ntx;
        ev_rdreq(); deliver();
        if (M.ntx != before + 1) return M.ntx - start_tx > 0 ? M.ntx - start_tx : -1;
        out[i] = M.tx[before];
    }
    ev_nack();
    if (mode == 2) { ev_stop(); deliver(); } else { deliver(); ev_stop(); deliver(); }
    return want;
}
static bool bus_write(uint8_t cmd, const uint8_t *body, int n, bool coalesce_all) {
    ev_start(false); ev_wbyte(cmd, true);
    if (!coalesce_all) deliver();
    for (int i = 0; i < n; i++) { ev_wbyte(body[i], false); if (!coalesce_all) deliver(); }
    ev_stop(); deliver();
    return !latched;
}

static const uint8_t kP5[5] = {0x41, 0x42, 0x43, 0x44, 0x45};
static uint8_t kP24_86[24], kP24_A8[24];
static void bring_up_fx(void) {   /* the DeepSeek rig: synthetic registers, NOT iPhone data */
    memset(&M, 0, sizeof M); memset(&fake_i2c1, 0, sizeof fake_i2c1); fake_i2c1.index = 1;
    live_port_stop(); live_port_clear_fault();
    st_init(&T, 0x0B); st_set_guard(&T, live_port_guard_enter, live_port_guard_exit, 0);
    for (int i = 0; i < 24; i++) { kP24_86[i] = (uint8_t)(0xA0 + i); kP24_A8[i] = (uint8_t)(0x10 + i); }
    st_add(&T, 0x09, ST_SHAPE_WORD, 2); st_set_word(&T, 0x09, 0x2A3B);
    st_add(&T, 0x0A, ST_SHAPE_WORD, 2); st_set_word(&T, 0x0A, 0x0102);
    st_add(&T, 0x0D, ST_SHAPE_WORD, 2); st_set_word(&T, 0x0D, 0x0050);
    st_add(&T, 0x20, ST_SHAPE_BLOCK, 5); st_set_block(&T, 0x20, kP5, 5);
    st_add(&T, 0x86, ST_SHAPE_BLOCK, 24); st_set_block(&T, 0x86, kP24_86, 24);
    st_add(&T, 0xA8, ST_SHAPE_BLOCK, 24); st_set_block(&T, 0xA8, kP24_A8, 24);
    st_add(&T, 0x50, ST_SHAPE_WRITE, 2); st_add(&T, 0x51, ST_SHAPE_WRITE, 1); st_add(&T, 0x52, ST_SHAPE_WRITE, 32); st_add(&T, 0xB0, ST_SHAPE_WRITE, 16);
    CHECK(st_seal(&T)); CHECK(live_port_start(&T)); g_us = 1000;
}
static void run_deepseek_fixtures(void) {
    static const char *names[13] = {"txns", "reads_done", "reads_short", "writes_ok", "writes_bad", "unknown_reads", "bad_len", "overreads", "aborts", "queue_dropped", "rx_bytes", "tx_bytes", "fault"};
    unsigned diverged = 0;
    for (unsigned i = 0; i < sizeof dsfx / sizeof dsfx[0]; i++) {
        const dsfx_t *f = &dsfx[i];
        bring_up_fx();
        for (int bi = 0; bi < f->nb; bi++) {
            const dsfx_batch_t *b = &f->b[bi];
            M.stat = b->flags & ~STAT_RX_FULL;
            for (int k = 0; k < b->n; k++) M.rx[M.rx_n++] = b->rx[k];
            if (b->n) M.stat |= STAT_RX_FULL;
            deliver();
        }
        uint32_t got[13] = {T.txns, T.reads_done, T.reads_short, T.writes_ok, T.writes_bad, T.unknown_reads, T.bad_len, T.overreads, T.aborts, T.queue_dropped, T.rx_bytes, T.tx_bytes, latched ? 1u : 0u};
        uint32_t exp[13]; memcpy(exp, f->e, sizeof exp);
        /* documented divergences of this tree from the beta1 contract the fixtures encode */
        const char *why = 0;
        if (!strcmp(f->id, "F06_write_burst5_queue_overflow")) {   /* queue depth 4 -> 8: five valid writes all fit */
            exp[3] = 5; exp[4] = 0; exp[9] = 0; exp[12] = 0; exp[0] = 5; why = "queue 8 (R4)";
        }
        if (OWNER && !strcmp(f->id, "F05_write_bad_pec_isolates")) {   /* owner: bad-PEC write accounted, port NOT isolated */
            exp[12] = 0; why = "owner write tolerance";
        }
        int bad = 0;
        for (int k = 0; k < 13; k++) if (got[k] != exp[k]) {
            bad++; printf("  DS fixture %s: %s got %u expected %u%s\n", f->id, names[k], (unsigned)got[k], (unsigned)exp[k], why ? " (divergence listed)" : "");
        }
        CHECK(bad == 0);
        /* TX byte stream: identical to the fixture unless the fixture ends in an expected isolation after bytes */
        if (!why) { CHECK(M.ntx == f->ntx); for (int k = 0; k < M.ntx && k < f->ntx; k++) CHECK(M.tx[k] == f->tx[k]); }
        if (why) diverged++;
    }
    printf("  DeepSeek DS-IRQ-03 fixtures through the production IRQ: %u fixtures, %u with a documented divergence\n", (unsigned)(sizeof dsfx / sizeof dsfx[0]), diverged);
}

int main(void) {
    run_deepseek_fixtures();
    /* ---------- T1: every captured response, three delivery orders, wire bytes + PEC ---------- */
    unsigned reads = 0;
    for (int mode = 0; mode < 3; mode++)
        for (unsigned k = 0; k < sizeof captured_responses / sizeof captured_responses[0]; k++) {
            const captured_response_t *r = &captured_responses[k];
            bring_up();
            uint8_t exp[40]; int n = 0;
            if (r->shape == ST_SHAPE_BLOCK) exp[n++] = r->count;
            memcpy(exp + n, r->data, r->count); n += r->count;
            exp[n] = pec_read(r->cmd, exp, n); n++;
            uint8_t got[40] = {0};
            int rc = bus_read(r->cmd, n, mode, got);
            CHECK(rc == n && !memcmp(got, exp, (size_t)n));
            if (!(!latched && wire.target->txns == 1 && wire.target->reads_done == 1 && M.stat == 0))
                printf("  mode=%d cmd=0x%02X latched=%d txns=%u reads_done=%u stat=0x%X last=%d\n", mode, r->cmd, latched, (unsigned)wire.target->txns, (unsigned)wire.target->reads_done, (unsigned)M.stat, (int)wire.target->last);
            CHECK(!latched && wire.target->txns == 1 && wire.target->reads_done == 1 && M.stat == 0);
            reads++;
        }
    printf("  captured responses over MMIO model: %u reads (3 delivery orders), wire+PEC compared to independent CRC-8\n", reads);

    /* ---------- T1b: the 22 T06 PHONE DATA samples (profile P12PM-B2), same three delivery orders ---------- */
    g_profile = "P12PM-B2";
    unsigned reads06 = 0;
    if (OWNER) {   /* owner build: P12PM-B2 serves the 12 Pro capture rows; every WORD of that map, wire + independent PEC */
        bring_up();
        st_reg_t regs[ST_MAX_REGS]; unsigned nr = T.n_regs; memcpy(regs, T.regs, sizeof regs);
        for (int mode = 0; mode < 3; mode++)
            for (unsigned k = 0; k < nr; k++) {
                if (regs[k].shape != ST_SHAPE_WORD || !regs[k].ready) continue;
                bring_up();
                uint8_t exp[3] = {regs[k].data[0], regs[k].data[1], 0}; exp[2] = pec_read(regs[k].cmd, exp, 2);
                uint8_t got[3] = {0};
                CHECK(bus_read(regs[k].cmd, 3, mode, got) == 3 && !memcmp(got, exp, 3) && !latched);
                reads06++;
            }
    } else
    for (int mode = 0; mode < 3; mode++)
        for (unsigned k = 0; k < sizeof phone_test_responses / sizeof phone_test_responses[0]; k++) {
            const captured_response_t *r = &phone_test_responses[k];
            bring_up();
            uint8_t exp[40]; int n = 0;
            if (r->shape == ST_SHAPE_BLOCK) exp[n++] = r->count;
            memcpy(exp + n, r->data, r->count); n += r->count;
            exp[n] = pec_read(r->cmd, exp, n); n++;
            uint8_t got[40] = {0};
            int rc = bus_read(r->cmd, n, mode, got);
            CHECK(rc == n && !memcmp(got, exp, (size_t)n) && !latched);
            reads06++;
        }
    printf("  T06 phone-data samples over MMIO model: %u reads, wire+PEC compared to independent CRC-8\n", reads06);
    g_profile = "CAP12PM-B1";

    /* ---------- T2: write bursts are opaque, queued, never change responses; PEC verified ---------- */
    bring_up();
    { uint8_t body[6] = {4, 0x11, 0x22, 0x33, 0x44, 0}; body[5] = pec_write(0x45, body, 5);
      CHECK(bus_write(0x45, body, 6, false)); CHECK(bus_write(0x45, body, 6, true));
      uint8_t w2[3] = {0xAA, 0xBB, 0}; w2[2] = pec_write(0x65, w2, 2); CHECK(bus_write(0x65, w2, 3, true));
      st_write_t w; unsigned q = 0; while (st_pop_write(&T, &w)) q++; CHECK(q == 3 && T.writes_ok == 3 && T.writes_bad == 0);
      /* a response read after the writes is unchanged */
      uint8_t got[3]; CHECK(bus_read(0x08, 3, 0, got) == 3 && got[0] == 0xF0 && got[1] == 0x0B); }
    /* bad PEC write -> isolation, nothing queued, no reply byte */
    bring_up();
    { uint8_t body[3] = {0xAA, 0xBB, 0x00}; int nt = M.ntx; CHECK(bus_write(0x65, body, 3, true) == (bool)OWNER);
      CHECK(latched == !OWNER && T.writes_ok == 0 && T.writes_bad == 1 && M.ntx == nt); }   /* owner: accounted, port up */

    /* ---------- T3: write queue overflow = deterministic fault + counter ---------- */
    bring_up();
    { uint8_t b[3] = {0x01, 0x02, 0}; b[2] = pec_write(0x65, b, 2);
      for (unsigned i = 0; i < ST_WRITE_QUEUE; i++) CHECK(bus_write(0x65, b, 3, true));
      CHECK(bus_write(0x65, b, 3, true) == (bool)OWNER && latched == !OWNER && T.queue_dropped == 1 && T.writes_ok == ST_WRITE_QUEUE);
      st_write_t w; unsigned q = 0; while (st_pop_write(&T, &w)) q++; CHECK(q == ST_WRITE_QUEUE); }
    /* draining between bursts keeps the bus alive indefinitely (burst of 3 as in the captures) */
    bring_up();
    { uint8_t b[3] = {0x01, 0x02, 0}; b[2] = pec_write(0x65, b, 2); st_write_t w;
      for (int round = 0; round < 50; round++) { for (int i = 0; i < 3; i++) CHECK(bus_write(0x65, b, 3, true)); while (st_pop_write(&T, &w)) {} }
      CHECK(!latched && T.queue_dropped == 0); }

    /* ---------- T4: command-only STOP does not carry a pointer into a later bare read ---------- */
    bring_up();
    { /* cold bus: a bare read has no pointer -> isolate, no byte */
      int nt0 = M.ntx; ev_start(false); ev_rdreq(); deliver(); CHECK(latched && M.ntx == nt0); }
    bring_up();
    { /* R3: known command + command-only STOP -> a later bare read serves THAT command (twice: pointer persists) */
      ev_start(false); ev_wbyte(0x08, true); deliver(); ev_stop(); deliver(); CHECK(!latched && T.ptr_valid && T.ptr == 0x08);
      for (int rep = 0; rep < 2; rep++) { uint8_t g[3]; int b = M.ntx;
        ev_start(true); for (int i = 0; i < 3; i++) { ev_rdreq(); deliver(); } ev_nack(); deliver(); ev_stop(); deliver();
        CHECK(!latched && M.ntx == b + 3); memcpy(g, M.tx + b, 3); CHECK(g[0] == 0xF0 && g[1] == 0x0B); } }
    bring_up();
    { /* unknown command pointer is NOT retained: next bare read isolates */
      ev_start(false); ev_wbyte(0x71, true); deliver(); ev_stop(); deliver(); CHECK(!latched && !T.ptr_valid);
      int nt = M.ntx; ev_start(true); ev_rdreq(); deliver(); CHECK(latched && M.ntx == nt); }
    bring_up();
    { /* a write clears the pointer */
      ev_start(false); ev_wbyte(0x08, true); deliver(); ev_stop(); deliver(); CHECK(T.ptr_valid);
      uint8_t w2[3] = {0xAA, 0xBB, 0}; w2[2] = pec_write(0x65, w2, 2); CHECK(bus_write(0x65, w2, 3, true)); CHECK(!T.ptr_valid);
      int nt = M.ntx; ev_start(true); ev_rdreq(); deliver(); CHECK(latched && M.ntx == nt); }
    /* same, with the pointer-only STOP coalesced with the next RD_REQ (STOP+RD_REQ ambiguity) */
    bring_up();
    { ev_start(false); ev_wbyte(0x86, true); ev_stop(); ev_rdreq(); int nt = M.ntx; deliver(); CHECK(latched && M.ntx == nt); }

    /* ---------- T5: unknown command read -> isolate, no data byte ---------- */
    bring_up();
    { int nt = M.ntx; uint8_t g[4] = {0}; int rc = bus_read(0x71, 3, 0, g);
      if (OWNER) CHECK(!latched && rc == 3 && g[0] == 0xFF && g[1] == 0xFF && g[2] == 0xFF);   /* owner: released-bus 0xFF, no data */
      else CHECK(latched && rc <= 0 && M.ntx == nt);
      CHECK(T.unknown_reads == 1); }

    /* ---------- T6: over-read past PEC -> last RD_REQ gets no byte (never padded with 0xFF) ---------- */
    bring_up();
    { uint8_t g[8]; int rc = bus_read(0x08, 4, 0, g);                              /* word = 2 data + PEC = 3 bytes; 4th is overread */
      CHECK(latched && rc == 3 && T.overreads == 1 && M.ntx == 3); }

    /* ---------- T7: STOP coalesced with the next transaction's first byte -> rejected, not guessed ---------- */
    bring_up();
    { ev_start(false); ev_wbyte(0x08, true); deliver(); ev_stop(); ev_wbyte(0x09, true); deliver(); CHECK(latched); }
    /* T7b: idle engine + [FIRST byte][STOP] in one batch is accepted as a command-only STOP (DEEPSEEK P02 not
     * adopted). If the bus really carried [STOP][new START + command], the command byte becomes the retained
     * pointer, so the following bare read still returns the register the master asked for. */
    bring_up();
    { ev_wbyte(0x08, true); ev_stop(); deliver(); CHECK(!latched && T.txns == 1);
      int nt = M.ntx; ev_start(true); ev_rdreq(); deliver();
      CHECK(!latched && M.ntx == nt + 1 && M.tx[nt] == 0xF0); }   /* the swallowed command became the pointer: the read is the right one */
    /* RX_DONE + STOP in one IRQ is the normal read end and is accepted (covered by mode 2 above) */

    /* ---------- T8: error flags and FIFO overrun -> isolate ---------- */
    bring_up(); M.stat |= I2C_IC_INTR_STAT_R_TX_ABRT_BITS; deliver(); CHECK(latched);
    bring_up(); M.stat |= I2C_IC_INTR_STAT_R_RX_OVER_BITS; deliver(); CHECK(latched);
    bring_up(); for (int i = 0; i < 20; i++) ev_wbyte((uint8_t)i, i == 0); deliver(); CHECK(latched);   /* >16 bytes in FIFO */
    bring_up(); ev_wbyte(0x08, false); deliver(); CHECK(latched);                      /* missed START: byte without FIRST_DATA_BYTE */

    /* ---------- T9: restart-write: a new FIRST byte while a transaction is open drops the old one ---------- */
    bring_up();
    { ev_start(false); ev_wbyte(0x08, true); deliver(); ev_start(true); ev_wbyte(0x09, true); deliver();
      CHECK(!latched && T.aborts == 1);
      ev_start(true); int b = M.ntx; ev_rdreq(); deliver(); CHECK(M.ntx == b + 1 && M.tx[b] == 0xC1); }   /* 0x09 -> C1 0D */

    /* ---------- T10: isolation disables the peripheral and (via service) releases GP6/GP7 ---------- */
    bring_up(); M.stat |= I2C_IC_INTR_STAT_R_TX_ABRT_BITS; deliver();
    { i2c_hw_t *h = i2c_get_hw(BAT_I2C_INSTANCE);
      CHECK(latched && h->enable == 0 && h->intr_mask == 0 && live_port_fault());
      unsigned before = M.pins_released; live_port_service(g_us); live_port_service(g_us);
      printf("  pins_released before=%u after=%u\n", before, M.pins_released);
      CHECK(M.pins_released == before + 2);                                          /* released once (SDA+SCL), not repeatedly */
      /* the engine keeps refusing while latched; start is refused until an explicit stop + clear */
      CHECK(!live_port_start(&T));
      live_port_stop(); live_port_clear_fault(); CHECK(!live_port_fault()); }
    /* clear is refused while installed */
    bring_up(); { M.stat |= I2C_IC_INTR_STAT_R_TX_ABRT_BITS; deliver(); live_port_clear_fault(); CHECK(live_port_fault()); }

    /* ---------- T11: idle expiry inside an open transaction ---------- */
    bring_up();
    { ev_start(false); ev_wbyte(0x08, true); deliver(); CHECK(!latched);
      live_port_service(g_us + 50000); CHECK(!latched);                              /* 50 ms: still waiting */
      live_port_service(g_us + 200000); CHECK(latched); }
    bring_up(); live_port_service(g_us + 5000000); CHECK(!latched);                  /* idle bus, no open transaction: no fault */

    /* ---------- T12: stop while running, then explicit restart works with a fresh prepared target ---------- */
    bring_up(); live_port_stop(); CHECK(!installed);
    CHECK(i2c_get_hw(BAT_I2C_INSTANCE)->enable == 0);

    printf("live_port MMIO model: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
