/* V02-01 / V02-02 host tests: INA228 init fault injection, control-cycle EN ordering,
 * bounded/non-blocking log ring and bounded bus-log drain. Included by test_main.c. */
#include "../../src/hal/ina228_core.h"
#include "../../src/log/log_ring.h"
#include "../../src/ctrl/ctrl_cycle.h"

/* ---------- mock INA228 on an injectable bus ---------- */
typedef struct {
    uint16_t reg[64];
    int writes, fail_write_n;        /* fail the n-th write (1-based), 0 = none */
    uint8_t fail_read_reg; bool fail_read;
    uint8_t stuck_reg; bool stuck;   /* register ignores writes (read-back mismatch) */
    bool rst_sticks;                 /* RST bit never self-clears */
    bool absent;
} mock_ina_t;

static bool m_wr(void *ctx, const uint8_t *b, size_t n) {
    mock_ina_t *m = ctx;
    if (m->absent || n != 3) return false;
    m->writes++;
    if (m->fail_write_n && m->writes == m->fail_write_n) return false;
    uint8_t r = b[0]; uint16_t v = (uint16_t)(b[1] << 8 | b[2]);
    if (m->stuck && r == m->stuck_reg) return true;                 /* ACKed but not stored */
    if (r == INA_R_CONFIG && (v & INA_CONFIG_RST)) { memset(m->reg, 0, sizeof m->reg);
        m->reg[INA_R_MFG_ID] = 0x5449; m->reg[INA_R_DEV_ID] = 0x2281;
        m->reg[INA_R_CONFIG] = m->rst_sticks ? INA_CONFIG_RST : 0; return true; }
    if (r == INA_R_SHUNT_CAL || r == INA_R_BOVL) v &= 0x7FFF;
    m->reg[r & 63] = v;
    return true;
}
static bool m_rd(void *ctx, uint8_t reg, uint8_t *buf, size_t n) {
    mock_ina_t *m = ctx;
    if (m->absent || (m->fail_read && reg == m->fail_read_reg)) return false;
    uint16_t v = m->reg[reg & 63];
    if (n == 2) { buf[0] = v >> 8; buf[1] = (uint8_t)v; }
    else for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i == 0 ? v >> 8 : i == 1 ? v : 0);
    if (reg == INA_R_DIAG_ALRT && n == 2) buf[1] |= 0x02;
    return true;
}
static void mock_reset(mock_ina_t *m) {
    memset(m, 0, sizeof *m);
    m->reg[INA_R_MFG_ID] = 0x5449; m->reg[INA_R_DEV_ID] = 0x2281;
}

static void test_ina228_init(void) {
    mock_ina_t m; ina228_dev_t d; ina228_reading_t r;
    ina228_bus_t bus = {m_wr, m_rd, 0, &m};
    /* healthy device: READY only after the OV limit is written + read back */
    mock_reset(&m);
    CHECK(ina228_dev_init(&d, &bus, 0.005f, 4.0f) == INA_OK && !d.ready);
    CHECK(!ina228_dev_read(&d, &r) && !r.ok);                       /* no MEASURED before READY */
    CHECK(ina228_dev_set_ov(&d, 4.45f) == INA_OK && d.ready && d.status == INA_OK);
    CHECK(m.reg[INA_R_CONFIG] == INA_CONFIG_VALUE && m.reg[INA_R_ADC_CONFIG] == INA_ADC_CONFIG_VALUE);
    CHECK(m.reg[INA_R_SHUNT_CAL] == d.shunt_cal && m.reg[INA_R_BOVL] == (uint16_t)(4.45f / 3.125e-3f));
    CHECK(ina228_dev_read(&d, &r) && r.ok && r.new_sample);
    /* absent / wrong identity */
    mock_reset(&m); m.absent = true;
    CHECK(ina228_dev_init(&d, &bus, 0.005f, 4.0f) == INA_ERR_NO_DEVICE && !d.ready);
    mock_reset(&m); m.reg[INA_R_MFG_ID] = 0x1234;
    CHECK(ina228_dev_init(&d, &bus, 0.005f, 4.0f) == INA_ERR_ID);
    mock_reset(&m); m.reg[INA_R_DEV_ID] = 0x2370;                  /* INA237 */
    CHECK(ina228_dev_init(&d, &bus, 0.005f, 4.0f) == INA_ERR_ID);
    /* successful ID reads followed by each individual failed write (reset, CONFIG, ADC, CAL, BOVL) */
    ina_status_t expect[5] = {INA_ERR_RESET, INA_ERR_CONFIG, INA_ERR_ADC_CONFIG, INA_ERR_SHUNT_CAL, INA_ERR_OV_LIMIT};
    for (int k = 1; k <= 5; k++) {
        mock_reset(&m); m.fail_write_n = k;
        ina_status_t st = ina228_dev_init(&d, &bus, 0.005f, 4.0f);
        if (st == INA_OK) st = ina228_dev_set_ov(&d, 4.45f);
        CHECK(st == expect[k - 1]);
        CHECK(!d.ready);
        CHECK(!ina228_dev_read(&d, &r) && !r.ok);
        /* OV limit cannot "repair" a failed init */
        if (k < 5) CHECK(ina228_dev_set_ov(&d, 4.45f) != INA_OK && !d.ready);
    }
    /* reset bit never clears */
    mock_reset(&m); m.rst_sticks = true;
    CHECK(ina228_dev_init(&d, &bus, 0.005f, 4.0f) == INA_ERR_RESET && !d.ready);
    /* write ACKed but register not stored -> read-back mismatch, for each critical register */
    uint8_t regs[4] = {INA_R_CONFIG, INA_R_ADC_CONFIG, INA_R_SHUNT_CAL, INA_R_BOVL};
    for (int k = 0; k < 4; k++) {
        mock_reset(&m); m.stuck = true; m.stuck_reg = regs[k];
        ina_status_t st = ina228_dev_init(&d, &bus, 0.005f, 4.0f);
        if (st == INA_OK) st = ina228_dev_set_ov(&d, 4.45f);
        CHECK(st == INA_ERR_READBACK && !d.ready);
    }
    /* read-back read fails */
    mock_reset(&m); m.fail_read = true; m.fail_read_reg = INA_R_ADC_CONFIG;
    CHECK(ina228_dev_init(&d, &bus, 0.005f, 4.0f) == INA_ERR_READBACK && !d.ready);
    /* invalid calibration / OV argument */
    mock_reset(&m);
    CHECK(ina228_dev_init(&d, &bus, 100.0f, 4.0f) == INA_ERR_SHUNT_CAL && !d.ready);
    mock_reset(&m); ina228_dev_init(&d, &bus, 0.005f, 4.0f);
    CHECK(ina228_dev_set_ov(&d, -1.0f) == INA_ERR_OV_LIMIT && !d.ready);
    /* READY device: a later read failure is reported, not hidden */
    mock_reset(&m); ina228_dev_init(&d, &bus, 0.005f, 4.0f); ina228_dev_set_ov(&d, 4.45f);
    m.fail_read = true; m.fail_read_reg = INA_R_CURRENT;
    CHECK(!ina228_dev_read(&d, &r) && !r.ok);
    /* not initialised */
    ina228_dev_t z; memset(&z, 0, sizeof z);
    CHECK(ina228_dev_set_ov(&z, 4.45f) == INA_ERR_NOT_INIT && !z.ready);
    CHECK(strcmp(ina_status_str(INA_ERR_READBACK), "READBACK_MISMATCH") == 0);
}

/* ---------- V02-02: log ring ---------- */
static void test_log_ring(void) {
    static char mem[256];
    log_ring_t lr; lr_init(&lr, mem, sizeof mem);
    CHECK(lr_push(&lr, "hello\n", 6) && lr_used(&lr) == 6);
    char out[300]; size_t n = lr_pop(&lr, out, 4);            /* bounded drain */
    CHECK(n == 4 && memcmp(out, "hell", 4) == 0 && lr_used(&lr) == 2);
    n = lr_pop(&lr, out, 100); CHECK(n == 2 && lr_used(&lr) == 0);
    /* stopped consumer: ring fills, further lines are DROPPED (counted), push never blocks */
    char line[40]; memset(line, 'x', sizeof line);
    int accepted = 0;
    for (int i = 0; i < 100; i++) accepted += lr_push(&lr, line, sizeof line);
    CHECK(accepted == 6 && lr.dropped == 94 && lr_used(&lr) == 240);
    CHECK(!lr_push(&lr, line, 17) && lr.dropped == 95);         /* partial lines never stored */
    /* oversize line rejected */
    CHECK(!lr_push(&lr, mem, sizeof mem + 1));
    /* drain resumes; wrap-around keeps order */
    lr_init(&lr, mem, sizeof mem);
    for (int i = 0; i < 300; i++) {
        char t[8]; int k = snprintf(t, sizeof t, "%03d\n", i % 1000);
        CHECK(lr_push(&lr, t, (size_t)k));
        char o[8]; CHECK(lr_pop(&lr, o, 4) == 4 && memcmp(o, t, 4) == 0);
    }
    /* formatted push with a sink that is never drained: bounded time, drops counted */
    lr_init(&lr, mem, sizeof mem);
    for (int i = 0; i < 1000; i++) lr_printf(&lr, 1234, "SAFE", "fault %d value=%.3f", i, 1.5);
    CHECK(lr_used(&lr) <= sizeof mem && lr.dropped > 900);
}

/* ---------- V02-02: control cycle drives EN before any logging ---------- */
typedef struct { int seq; int en_seq; bool en_val; int en_writes; } hal_mock_t;
static void hm_en(void *ctx, bool on) { hal_mock_t *h = ctx; h->en_seq = ++h->seq; h->en_val = on; h->en_writes++; }

static void test_ctrl_cycle_order(void) {
    safety_t s; hal_mock_t h; safety_limits_t lim = {4.45f, 3.30f, 3.50f, 10000, 200}; memset(&h, 0, sizeof h);
    ctrl_hal_t hal = {hm_en, &h};
    static char mem[128]; log_ring_t lr; lr_init(&lr, mem, sizeof mem);
    /* fill the log ring completely: the USB host is not reading */
    char line[32]; memset(line, 'y', sizeof line);
    while (lr_push(&lr, line, sizeof line)) {}
    uint32_t dropped0 = lr.dropped;
    /* a qualified (test-only) profile forced ON, then the monitor disappears -> FAULT */
    batt_profile_t tp; memset(&tp, 0, sizeof tp); tp.id = "TEST-VERIFIED"; tp.status = PROF_VERIFIED;
    safety_init(&s, &lim);
    s.state = OUT_ON;
    safety_inputs_t in; memset(&in, 0, sizeof in); in.q = TQ_ABSENT;
    ctrl_event_t ev = ctrl_cycle(&s, 1000, &in, &tp, &hal);
    CHECK(h.en_writes == 1 && h.en_val == false);            /* EN written low inside the cycle */
    CHECK(ev.entered_fault && s.state == OUT_FAULT);
    int en_seq = h.en_seq;
    /* logging happens AFTER ctrl_cycle returned; with a full ring it is dropped, not waited for */
    ctrl_log_event(&lr, 1000, &ev, &s);
    CHECK(h.en_seq == en_seq && lr.dropped > dropped0);
    /* overcurrent with a measured sample: EN low, fault value recorded */
    safety_init(&s, &lim); s.state = OUT_ON; memset(&h, 0, sizeof h);
    in.q = TQ_MEASURED; in.v_out = 4.0f; in.i_out = 9.0f;
    ev = ctrl_cycle(&s, 2000, &in, &tp, &hal);
    CHECK(h.en_writes == 1 && !h.en_val && s.state == OUT_FAULT);
    /* unverified profile while ON: EN low on the same cycle */
    safety_init(&s, &lim); s.state = OUT_ON; memset(&h, 0, sizeof h);
    in.i_out = 0.5f;
    ev = ctrl_cycle(&s, 3000, &in, profile_get(profile_find("P12PM-r0")), &hal);
    CHECK(h.en_writes == 1 && !h.en_val);
    /* healthy OFF state: EN written low every cycle */
    safety_init(&s, &lim); memset(&h, 0, sizeof h);
    for (int i = 0; i < 10; i++) ctrl_cycle(&s, 4000 + i * 10, &in, &tp, &hal);
    CHECK(h.en_writes == 10 && !h.en_val);
}

/* ---------- V02-02: bounded bus-log drain ---------- */
static int s_drained;
static void drain_cb(void *ctx, const txn_rec_t *r) { (void)ctx; (void)r; s_drained++; }
static void test_bus_log_budget(void) {
    static target_engine_t e; te_init(&e, profile_get(profile_find("BENCH-r0")));
    for (int i = 0; i < 40; i++) { te_on_receive(&e, 0x20, i); te_on_finish(&e); }
    s_drained = 0;
    int n = te_drain(&e, 8, drain_cb, 0);
    CHECK(n == 8 && s_drained == 8);
    n = te_drain(&e, 8, drain_cb, 0); CHECK(n == 8 && s_drained == 16);
    while ((n = te_drain(&e, 8, drain_cb, 0)) > 0) CHECK(n <= 8);
    CHECK(s_drained == 40);
}

/* PM-ID-001-r2: INA226 identity verdict (strict AND) + raw scaling (bench/ina226_id.h) */
#include "../../bench/ina226_id.h"
static void test_ina226_id_verdict(void) {
    CHECK(ina226_id_verdict(true, 0x4127, 0x5449, 0x2260) == ID_PASS);
    CHECK(ina226_id_verdict(true, 0x4127, 0x5449, 0x2261) == ID_PASS);           /* documented rev */
    /* Codex negative cases: FE alone or FF alone must NOT pass */
    CHECK(ina226_id_verdict(true, 0x4127, 0x5449, 0x0000) == ID_DIE_MISMATCH);   /* missing die */
    CHECK(ina226_id_verdict(true, 0x4127, 0x5449, 0x1234) == ID_DIE_MISMATCH);   /* wrong die */
    CHECK(ina226_id_verdict(true, 0x4127, 0x1234, 0x2260) == ID_MFR_MISMATCH);   /* wrong mfr */
    CHECK(ina226_id_verdict(true, 0x4127, 0x0000, 0x2261) == ID_MFR_MISMATCH);
    CHECK(ina226_id_verdict(true, 0x4127, 0x0000, 0x0000) == ID_MFR_MISMATCH);   /* all-zero IDs */
    CHECK(ina226_id_verdict(true, 0x4000, 0x5449, 0x2260) == ID_CONFIG_MISMATCH);
    CHECK(ina226_id_verdict(true, 0xFFFF, 0xFFFF, 0xFFFF) == ID_CONFIG_MISMATCH); /* stuck-high */
    CHECK(ina226_id_verdict(true, 0x0000, 0x0000, 0x0000) == ID_CONFIG_MISMATCH);
    CHECK(ina226_id_verdict(false, 0x4127, 0x5449, 0x2260) == ID_TRANSPORT);      /* transport first */
    CHECK(ina226_id_verdict(false, 0, 0, 0) == ID_TRANSPORT);
    CHECK(ina226_shunt_uv(0x0640) == 4000.0f);    /* 1600 * 2.5 uV = 4 mV = 0.4 A on 10 mOhm */
    CHECK(ina226_shunt_uv(0xFFFF) == -2.5f);      /* two's complement */
    CHECK(ina226_shunt_uv(0x7FFF) == 81917.5f);   /* full scale */
    CHECK(ina226_shunt_uv(0x8000) == -81920.0f);  /* negative full scale */
    CHECK(ina226_bus_mv(3200) == 4000.0f);        /* 4.000 V */
}
