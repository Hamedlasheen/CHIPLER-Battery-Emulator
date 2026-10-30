/* CR-006-INA226 Phase A host tests H01-H09 (Codex acceptance matrix). Included from test_main.c. */
#include "../../src/hal/ina226_core.h"
#include <math.h>
#include <string.h>
#include "../../bench/ina226_bench_policy.h"
#include "../../src/log/log_ring.h"

/* ---------- mock INA226 register file on an injectable bus ---------- */
typedef struct {
    uint16_t reg[256];
    bool absent;                 /* nothing answers */
    int  fail_write_n;           /* 1-based index of write to fail (0 = never) */
    int  writes;
    int  fail_read_reg;          /* -1 = none */
    int  fail_read_after;        /* fail reads of fail_read_reg only after this many reads of it */
    int  reads_of_reg;
    bool rst_sticks;             /* RST bit never clears */
    int  stuck_reg;              /* -1 = none: writes ignored */
    bool latch_aff_clear_on_mask_read;
    uint16_t cvrf_seq[8]; int cvrf_n, cvrf_i;   /* CVRF pattern returned by MASK reads */
    uint32_t now, read_tick_ms;
    ina226_dev_t *fault_target; int fault_on_reg;
    int  mask_reads;
} m226_t;

static void m226_defaults(m226_t *m) {
    memset(m, 0, sizeof *m);
    m->fail_read_reg = -1; m->stuck_reg = -1; m->fault_on_reg = -1; m->latch_aff_clear_on_mask_read = true;
    m->reg[I226_R_CONFIG] = 0x4127; m->reg[I226_R_MFR_ID] = 0x5449; m->reg[I226_R_DIE_ID] = 0x2260;
}
static void m226_por(m226_t *m) {            /* sensor power-on reset: registers to defaults */
    uint16_t fe = m->reg[0xFE], ff = m->reg[0xFF];
    memset(m->reg, 0, sizeof m->reg);
    m->reg[I226_R_CONFIG] = 0x4127; m->reg[0xFE] = fe; m->reg[0xFF] = ff;
}
static bool m226_wr(void *ctx, const uint8_t *b, size_t n) {
    m226_t *m = ctx;
    if (m->absent) return false;
    m->writes++;
    if (m->fail_write_n && m->writes == m->fail_write_n) return false;
    if (n != 3) return n == 1;
    uint8_t r = b[0]; uint16_t v = (uint16_t)(b[1] << 8 | b[2]);
    if (r == m->stuck_reg) return true;
    if (r == I226_R_CONFIG && (v & 0x8000)) {      /* reset */
        m226_por(m);
        if (m->rst_sticks) m->reg[I226_R_CONFIG] = 0xC127;
        return true;
    }
    if (r == I226_R_CONFIG) v = (uint16_t)(0x4000 | (v & 0x0FFF));
    if (r == I226_R_CAL) v &= 0x7FFF;
    if (r == I226_R_MASK) v = (uint16_t)((v & 0xFC03) | (m->reg[I226_R_MASK] & 0x001C));
    if (r == 0xFE || r == 0xFF) return true;       /* read-only */
    m->reg[r] = v;
    return true;
}
static bool m226_rd(void *ctx, uint8_t r, uint8_t *b, size_t n) {
    m226_t *m = ctx;
    m->now += m->read_tick_ms;
    if (m->fault_target && r == m->fault_on_reg) ina226_alert_pin_event(m->fault_target);
    if (m->absent || n != 2) return false;
    if (r == m->fail_read_reg && ++m->reads_of_reg > m->fail_read_after) return false;
    uint16_t v = m->reg[r];
    if (r == I226_R_MASK) {
        m->mask_reads++;
        if (m->cvrf_n) { v = (uint16_t)((v & ~I226_MASK_CVRF) | (m->cvrf_seq[m->cvrf_i % m->cvrf_n] ? I226_MASK_CVRF : 0)); m->cvrf_i++; }
        if (m->latch_aff_clear_on_mask_read) m->reg[I226_R_MASK] &= (uint16_t)~(I226_MASK_AFF | I226_MASK_CVRF);
    }
    b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v;
    return true;
}
static void m226_delay(void *ctx, uint32_t ms) { (void)ctx; (void)ms; }

static uint32_t m226_now(void *ctx) { return ((m226_t *)ctx)->now; }
static bool m226_read_at(ina226_dev_t *d, i226_sample_t *s, uint32_t now) {
    ((m226_t *)d->bus->ctx)->now = now; return ina226_read(d, s, now);
}

static const i226_cfg_t CFG10 = {0.010f, 0.000250f, 0x4127, 0.60f, false};

static void m226_set_sample(m226_t *m, int16_t sh, uint16_t bus, int16_t cur, uint16_t pw, bool cvrf) {
    m->reg[I226_R_SHUNT] = (uint16_t)sh; m->reg[I226_R_BUS] = bus;
    m->reg[I226_R_CURRENT] = (uint16_t)cur; m->reg[I226_R_POWER] = pw;
    m->reg[I226_R_MASK] = (uint16_t)((m->reg[I226_R_MASK] & ~I226_MASK_CVRF) | (cvrf ? I226_MASK_CVRF : 0));
}

static void test_ina226_core(void) {
    m226_t m; ina226_dev_t d; i226_sample_t s;
    ina226_bus_t bus = {m226_wr, m226_rd, m226_delay, &m, m226_now};

    /* nominal init: H04 values */
    m226_defaults(&m);
    CHECK(ina226_init(&d, &bus, &CFG10) == I226_OK && d.configured);
    CHECK(d.cal == 0x0800 && m.reg[I226_R_CAL] == 0x0800);
    CHECK(d.alert_limit == 0x0960 && m.reg[I226_R_ALERT] == 0x0960);    /* 0.60 A -> 6 mV -> 2400 */
    CHECK((m.reg[I226_R_MASK] & 0xFC03) == 0x8001);
    CHECK(fabsf(d.current_lsb - 0.00025f) < 1e-9f);
    CHECK(!d.calibration_verified && !d.protection_qualified);

    /* H01 identity: FE/FF strict AND, zeros, FFFF, NAK at each identity step */
    const uint16_t fes[] = {0x5449, 0x1234, 0x0000, 0xFFFF, 0x5449, 0x5449, 0x5449};
    const uint16_t ffs[] = {0x1234, 0x2260, 0x0000, 0xFFFF, 0x0000, 0x2262, 0x2280};
    for (int k = 0; k < 7; k++) {
        m226_defaults(&m); m.reg[0xFE] = fes[k]; m.reg[0xFF] = ffs[k];
        CHECK(ina226_init(&d, &bus, &CFG10) != I226_OK && !d.configured);
        CHECK(m.writes == 0);                                   /* no write before identity passes */
    }
    m226_defaults(&m); m.reg[0xFF] = 0x2261;
    CHECK(ina226_init(&d, &bus, &CFG10) == I226_OK);            /* documented die revision */
    m226_defaults(&m); m.absent = true;
    CHECK(ina226_init(&d, &bus, &CFG10) == I226_ERR_NO_DEVICE && !d.configured && !d.mfr_read);
    m226_defaults(&m); m.fail_read_reg = 0xFF;
    CHECK(ina226_init(&d, &bus, &CFG10) == I226_ERR_TRANSPORT && d.mfr_read && !d.die_read);

    /* H02 every write fails in turn; read-back mismatch; RST stuck; non-default after reset */
    for (int k = 1; k <= 6; k++) {
        m226_defaults(&m); m.fail_write_n = k;
        CHECK(ina226_init(&d, &bus, &CFG10) == I226_ERR_TRANSPORT && !d.configured && d.fail_step);
    }
    const int stuck[] = {I226_R_CAL, I226_R_ALERT, I226_R_MASK};
    for (int k = 0; k < 3; k++) {
        m226_defaults(&m); m.stuck_reg = stuck[k];
        i226_status_t st = ina226_init(&d, &bus, &CFG10);
        /* stuck MASK keeps 0 -> the "MASK disable" write reads back fine, the SOL|LEN write must fail */
        CHECK(st == I226_ERR_READBACK && !d.configured && d.fail_reg == stuck[k]);
    }
    m226_defaults(&m); m.rst_sticks = true;
    CHECK(ina226_init(&d, &bus, &CFG10) == I226_ERR_RESET && !d.configured);
    {   /* CONFIG write ignored with a non-default request -> read-back mismatch */
        i226_cfg_t c = CFG10; c.config = 0x4527;              /* AVG = 4 */
        m226_defaults(&m); m.stuck_reg = I226_R_CONFIG;
        /* RST also ignored when stuck: CONFIG stays 4127 so reset check passes, then CONFIG mismatch */
        CHECK(ina226_init(&d, &bus, &c) == I226_ERR_READBACK && d.fail_reg == I226_R_CONFIG);
    }

    /* H03 parameter rejection before any bus access */
    const float bad_r[] = {0.0f, -0.01f, NAN, INFINITY};
    for (int k = 0; k < 4; k++) {
        i226_cfg_t c = CFG10; c.r_shunt_ohm = bad_r[k];
        m226_defaults(&m);
        CHECK(ina226_init(&d, &bus, &c) == I226_ERR_PARAM && m.writes == 0 && m.mask_reads == 0);
    }
    uint16_t cal, lim; float lsb;
    CHECK(i226_compute_cal(0.010f, 1e-9f, &cal, &lsb) == I226_ERR_PARAM);   /* CAL overflow */
    CHECK(i226_compute_cal(0.010f, 2.0f, &cal, &lsb) == I226_ERR_PARAM);    /* CAL 0.256 rounds to 0 */
    CHECK(i226_compute_cal(0.010f, 0.0001f, &cal, &lsb) == I226_ERR_PARAM); /* current reg < shunt range */
    CHECK(i226_compute_cal(0.0102f, 0.00025f, &cal, &lsb) == I226_OK && cal == 2008);
    CHECK(fabsf(lsb - 0.00512f / (2008.0f * 0.0102f)) < 1e-9f);            /* actual LSB from rounded CAL */
    CHECK(i226_sol_limit(0.0f, 0.01f, &lim) == I226_ERR_PARAM);
    CHECK(i226_sol_limit(100.0f, 0.01f, &lim) == I226_ERR_PARAM);           /* > 81.9 mV not representable */
    CHECK(i226_sol_limit(0.6f, 0.01f, &lim) == I226_OK && lim == 2400);
    CHECK(i226_sol_limit(0.60099f, 0.01f, &lim) == I226_OK && lim == 2403); /* floor: never above request */
    { i226_cfg_t c = CFG10; c.config = 0xC127; m226_defaults(&m);
      CHECK(ina226_init(&d, &bus, &c) == I226_ERR_PARAM); }               /* RST bit in config value */

    /* H04 conversions */
    m226_defaults(&m); ina226_init(&d, &bus, &CFG10);
    m226_set_sample(&m, 1600, 3200, 1600, 0, true);
    CHECK(m226_read_at(&d, &s, 1000) && s.valid && s.new_conversion);
    CHECK(fabsf(i226_shunt_v(s.raw_shunt) - 0.004f) < 1e-7f);
    CHECK(fabsf(s.i - 0.4f) < 1e-6f && fabsf(s.v_bus - 4.0f) < 1e-6f && fabsf(s.p_signed - 1.6f) < 1e-5f);
    CHECK(fabsf(i226_shunt_v((int16_t)0xFFFF) + 2.5e-6f) < 1e-12f);
    CHECK(s.uncalibrated && !s.temperature_valid);

    /* H05 zero / negative / full-scale / overflow */
    m226_set_sample(&m, 0, 0, 0, 0, true);
    CHECK(m226_read_at(&d, &s, 1010) && s.i == 0.0f && s.v_bus == 0.0f);
    m226_set_sample(&m, -1600, 3200, -1600, 0, true);
    CHECK(m226_read_at(&d, &s, 1020) && s.i < -0.399f && s.p_signed < -1.59f);  /* reverse: signed */
    m226_set_sample(&m, 32767, 0x7FFF, 32767, 0, true);
    CHECK(m226_read_at(&d, &s, 1030) && fabsf(s.i - 8.19175f) < 1e-3f);
    m226_set_sample(&m, -32768, 0, -32768, 0, true);
    CHECK(m226_read_at(&d, &s, 1040) && fabsf(s.i + 8.192f) < 1e-3f);
    uint32_t seq_before = s.seq; float i_before = s.i;
    m.reg[I226_R_MASK] |= I226_MASK_OVF;
    CHECK(!m226_read_at(&d, &s, 1050) && !s.valid && d.status == I226_ERR_OVERFLOW);
    CHECK(s.seq == seq_before && s.i == i_before);              /* not republished as fresh */
    m.reg[I226_R_MASK] &= (uint16_t)~I226_MASK_OVF;
    {   /* measured R: non-ideal CAL path */
        i226_cfg_t c = CFG10; c.r_shunt_ohm = 0.0102f;
        m226_defaults(&m); CHECK(ina226_init(&d, &bus, &c) == I226_OK && d.cal == 2008);
        m226_set_sample(&m, 1600, 3200, 1569, 0, true);        /* chip current reg for 0.392 A */
        CHECK(m226_read_at(&d, &s, 0) && fabsf(s.i - 1569 * d.current_lsb) < 1e-7f);
    }

    /* H06 conversion-ready / age / failed read does not publish half a sample */
    m226_defaults(&m); ina226_init(&d, &bus, &CFG10);
    CHECK(ina226_stale(&d, 0));                                 /* nothing yet */
    m226_set_sample(&m, 100, 3200, 100, 0, false);
    CHECK(!m226_read_at(&d, &s, 10) && !s.valid && !s.new_conversion && ina226_stale(&d, 10));
    m226_set_sample(&m, 100, 3200, 100, 0, true);
    CHECK(m226_read_at(&d, &s, 20) && s.new_conversion && !ina226_stale(&d, 20));
    CHECK(!ina226_stale(&d, 120) && ina226_stale(&d, 121));     /* 100 ms window */
    seq_before = s.seq;
    m.fail_read_reg = I226_R_CURRENT; m.fail_read_after = 0;
    m226_set_sample(&m, 999, 999, 999, 0, true);
    CHECK(!m226_read_at(&d, &s, 30) && !s.valid && s.seq == seq_before && s.raw_shunt == 100);
    CHECK(d.status == I226_ERR_TRANSPORT && d.fail_reg == I226_R_CURRENT && d.configured);
    CHECK(ina226_stale(&d, 130) && !ina226_stale(&d, 119));
    m.fail_read_reg = -1;

    /* H07 alert latch: AFF captured, cleared by MASK read in the chip but held in software */
    m226_defaults(&m); ina226_init(&d, &bus, &CFG10);
    m226_set_sample(&m, 2500, 3200, 2500, 0, true);
    m.reg[I226_R_MASK] |= I226_MASK_AFF;
    CHECK(m226_read_at(&d, &s, 0) && d.fault_latched && (d.fault_flags & I226_MASK_AFF));
    CHECK((m.reg[I226_R_MASK] & I226_MASK_AFF) == 0);           /* chip latch cleared by the read */
    m226_set_sample(&m, 100, 3200, 100, 0, true);
    CHECK(m226_read_at(&d, &s, 10) && d.fault_latched);          /* no automatic re-arm */
    ina226_fault_ack(&d);
    CHECK(!d.fault_latched);
    ina226_alert_pin_event(&d);                                 /* pin seen low before any MASK read */
    CHECK(d.fault_latched);
    ina226_fault_ack(&d);
    m226_set_sample(&m, 100, 3200, 100, 0, true);               /* CVRF alone never trips */
    CHECK(m226_read_at(&d, &s, 20) && !d.fault_latched);

    /* H08 unplug / sensor reset during the loop: bounded, invalid, re-init required */
    m.absent = true;
    CHECK(!m226_read_at(&d, &s, 30) && !s.valid && d.status == I226_ERR_TRANSPORT);
    m.absent = false;
    m226_por(&m);                                               /* replugged: registers at defaults */
    CHECK(!m226_read_at(&d, &s, 40) && d.status == I226_ERR_SENSOR_RESET && !d.configured);
    CHECK(!m226_read_at(&d, &s, 50) && !s.valid);                /* stays unusable until re-init */
    CHECK(ina226_init(&d, &bus, &CFG10) == I226_OK && d.configured);
    m.reg[I226_R_CAL] = 0x0001;                                 /* CAL changed behind our back */
    CHECK(!m226_read_at(&d, &s, 60) && d.status == I226_ERR_SENSOR_RESET && !d.configured);

    /* H09 identity of the sample type: no temperature, uncalibrated unless verified */
    {   i226_cfg_t c = CFG10; c.calibration_verified = true;
        m226_defaults(&m); ina226_init(&d, &bus, &c);
        m226_set_sample(&m, 10, 10, 10, 0, true);
        CHECK(m226_read_at(&d, &s, 0) && !s.uncalibrated && !s.temperature_valid && !d.protection_qualified);
    }
    /* Codex r2 F01: recovery success/failure cannot acknowledge a latched fault. */
    m226_defaults(&m); CHECK(ina226_init(&d, &bus, &CFG10) == I226_OK);
    ina226_alert_pin_event(&d); m.absent = true;
    CHECK(ina226_reinit(&d, &bus, &CFG10) != I226_OK && d.fault_latched);
    m.absent = false;
    CHECK(ina226_reinit(&d, &bus, &CFG10) == I226_OK && d.fault_latched);
    m226_set_sample(&m, 100, 3200, 100, 0, true); m.reg[I226_R_MASK] |= I226_MASK_AFF;
    CHECK(m226_read_at(&d, &s, 0) && d.fault_latched && (d.fault_flags & I226_MASK_AFF));
    uint32_t kept_seq = d.seq;
    m226_por(&m); CHECK(!m226_read_at(&d, &s, 10) && !d.configured && d.fault_latched);
    CHECK(ina226_reinit(&d, &bus, &CFG10) == I226_OK && d.fault_latched && d.seq == kept_seq);
    ina226_fault_ack(&d); CHECK(!d.fault_latched && d.fault_flags == 0);
    /* Fault arrives during a register read, rather than only before the loop. */
    m.fault_target = &d; m.fault_on_reg = I226_R_CURRENT;
    m226_set_sample(&m, 100, 3200, 100, 0, true);
    CHECK(m226_read_at(&d, &s, 20) && d.fault_latched);
    m.fault_target = NULL; ina226_fault_ack(&d);
    m.fault_target = &d; m.fault_on_reg = I226_R_DIE_ID;
    CHECK(ina226_reinit(&d, &bus, &CFG10) == I226_OK && d.fault_latched);
    m.fault_target = NULL; ina226_fault_ack(&d);
    /* F02: each integrity register independently corrupted. */
    const int corrupt[] = {I226_R_CONFIG, I226_R_CAL, I226_R_ALERT, I226_R_MASK};
    for (int k = 0; k < 4; k++) {
        m226_defaults(&m); CHECK(ina226_init(&d, &bus, &CFG10) == I226_OK);
        m226_set_sample(&m, 100, 3200, 100, 0, true); m.reg[corrupt[k]] ^= 1;
        CHECK(!m226_read_at(&d, &s, 0) && !s.valid && !d.configured && d.status == I226_ERR_SENSOR_RESET);
        CHECK(d.fail_reg == corrupt[k]);
    }
    /* Every initialization readback fails in turn, including final MASK enable. */
    const int failregs[] = {I226_R_CONFIG,I226_R_MASK,I226_R_CONFIG,I226_R_CAL,I226_R_ALERT,I226_R_MASK};
    const int failafter[] = {0,0,1,0,0,1};
    for (int k = 0; k < 6; k++) {
        m226_defaults(&m); m.fail_read_reg = failregs[k]; m.fail_read_after = failafter[k];
        CHECK(ina226_init(&d, &bus, &CFG10) == I226_ERR_TRANSPORT && !d.configured && d.fail_reg == failregs[k]);
    }
    /* F03: no-first conversion; exact stale boundary; wraparound; recovery. */
    m226_defaults(&m); ina226_init(&d, &bus, &CFG10); s.seq = 0;
    m226_set_sample(&m, 100, 3200, 100, 0, false);
    CHECK(!m226_read_at(&d, &s, 0) && !s.valid && d.seq == 0 && d.status == I226_ERR_STALE);
    m226_set_sample(&m, 100, 3200, 100, 0, true); CHECK(m226_read_at(&d, &s, 10));
    kept_seq = s.seq;
    CHECK(!m226_read_at(&d, &s, 110) && !s.valid && s.seq == kept_seq && d.status == I226_WAIT_CONVERSION);
    CHECK(!m226_read_at(&d, &s, 111) && !s.valid && s.seq == kept_seq && d.status == I226_ERR_STALE);
    m226_set_sample(&m, 101, 3200, 101, 0, true); CHECK(m226_read_at(&d, &s, 112) && s.valid && s.seq == kept_seq + 1);
    m226_set_sample(&m, 100, 3200, 100, 0, true); CHECK(m226_read_at(&d, &s, UINT32_MAX - 49));
    CHECK(!ina226_stale(&d, 50) && ina226_stale(&d, 51));
    /* F04: actual bus acquisition time, partial failure time and expired acquisition. */
    m.read_tick_ms = 2;
    m226_set_sample(&m, 100, 3200, 100, 0, true);
    CHECK(m226_read_at(&d, &s, 1000) && s.t_start_ms == 1000 && s.t_end_ms == 1016);
    kept_seq = s.seq; m.fail_read_reg = I226_R_CURRENT; m.fail_read_after = 0;
    CHECK(!m226_read_at(&d, &s, 1020) && s.t_end_ms == 1034 && s.seq == kept_seq && !s.valid);
    m.fail_read_reg = -1; m.read_tick_ms = 13;
    m226_set_sample(&m, 100, 3200, 100, 0, true);
    CHECK(!m226_read_at(&d, &s, 1040) && s.t_end_ms == 1144 && d.status == I226_ERR_STALE && s.seq == kept_seq);
    /* USB blocked: full log ring drops lines while scheduled sensor polls continue. */
    { char memory[160]; log_ring_t log; lr_init(&log, memory, sizeof memory);
      uint32_t next = 0, missed = 0, polls = 0;
      m226_defaults(&m); ina226_init(&d, &bus, &CFG10);
      for (uint32_t t = 0; t < 1000; t++) {
          if (!i226_poll_due(t, &next, &missed)) continue;
          m226_set_sample(&m, 100, 3200, 100, 0, true);
          CHECK(m226_read_at(&d, &s, t)); polls++;
          lr_printf(&log, t, "DRV", "blocked USB sample %lu", (unsigned long)s.seq);
      }
      CHECK(polls == 50 && missed == 0 && log.dropped > 0 && d.seq == 50);
      CHECK(lr_used(&log) <= sizeof memory);
      CHECK(i226_poll_due(1105, &next, &missed) && missed == 5 && next == 1120);
      CHECK(!i226_poll_due(1106, &next, &missed));
      next = UINT32_MAX - 9; missed = 0;
      CHECK(i226_poll_due(UINT32_MAX - 9, &next, &missed) && next == 10);
      CHECK(!i226_poll_due(9, &next, &missed) && i226_poll_due(10, &next, &missed));
    }
    CHECK(strcmp(i226_status_str(I226_ERR_SENSOR_RESET), "SENSOR_RESET_DETECTED") == 0);
    CHECK(strcmp(i226_status_str((i226_status_t)99), "?") == 0);
}
