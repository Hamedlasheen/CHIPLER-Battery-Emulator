#include "ina226_core.h"
#include <math.h>
static void latch_fault(ina226_dev_t *d, uint16_t flags);

static i226_status_t fail(ina226_dev_t *d, i226_status_t s, const char *step, uint8_t reg) {
    d->configured = false;
    d->status = s; d->fail_step = step; d->fail_reg = reg;
    return s;
}
static bool rd16(ina226_dev_t *d, uint8_t reg, uint16_t *v) {
    uint8_t b[2];
    if (!d->bus->rd(d->bus->ctx, reg, b, 2)) return false;
    *v = (uint16_t)((uint16_t)b[0] << 8 | b[1]);
    /* Includes initialization readbacks: no MASK read may silently discard AFF. */
    if (reg == I226_R_MASK && (*v & I226_MASK_AFF)) latch_fault(d, *v);
    return true;
}
static bool wr16(ina226_dev_t *d, uint8_t reg, uint16_t v) {
    uint8_t b[3] = {reg, (uint8_t)(v >> 8), (uint8_t)v};
    return d->bus->wr(d->bus->ctx, b, 3);
}
/* write + read-back under mask; distinguishes transport failure from read-back mismatch */
static i226_status_t wr_verify(ina226_dev_t *d, uint8_t reg, uint16_t v, uint16_t mask, const char *step) {
    uint16_t r;
    if (!wr16(d, reg, v)) return fail(d, I226_ERR_TRANSPORT, step, reg);
    if (!rd16(d, reg, &r)) return fail(d, I226_ERR_TRANSPORT, step, reg);
    if ((r & mask) != (v & mask)) return fail(d, I226_ERR_READBACK, step, reg);
    return I226_OK;
}

i226_status_t i226_compute_cal(float r, float lsb_req, uint16_t *cal, float *lsb_actual) {
    if (!isfinite(r) || !(r > 0.0f) || !isfinite(lsb_req) || !(lsb_req > 0.0f)) return I226_ERR_PARAM;
    double c = 0.00512 / ((double)lsb_req * (double)r);
    if (!(c >= 0.5) || c > 32767.4) return I226_ERR_PARAM;          /* CAL must round into 1..0x7FFF */
    uint16_t ci = (uint16_t)lrint(c);
    if (ci < 1) return I226_ERR_PARAM;
    double lsb = 0.00512 / ((double)ci * (double)r);
    /* current register (signed 16) must cover the full shunt range, else it overflows first */
    if (32768.0 * lsb < (I226_SHUNT_FS_V / (double)r) * 0.999) return I226_ERR_PARAM;
    *cal = ci; *lsb_actual = (float)lsb;
    return I226_OK;
}

i226_status_t i226_sol_limit(float trip_a, float r, uint16_t *limit) {
    if (!isfinite(trip_a) || !(trip_a > 0.0f) || !isfinite(r) || !(r > 0.0f)) return I226_ERR_PARAM;
    double counts = (double)trip_a * (double)r / 2.5e-6;
    double fl = floor(counts + 1e-9);                     /* conservative: never above the request */
    if (fl < 1.0 || fl > 32767.0) return I226_ERR_PARAM;
    *limit = (uint16_t)fl;
    return I226_OK;
}

i226_status_t ina226_init(ina226_dev_t *d, const ina226_bus_t *bus, const i226_cfg_t *cfg) {
    *d = (ina226_dev_t){0};
    d->bus = bus;
    d->status = I226_ERR_NOT_INIT;
    if (!bus || !bus->wr || !bus->rd || !bus->now_ms || !cfg) return fail(d, I226_ERR_PARAM, "args", 0);
    if (cfg->config & I226_CONFIG_RST) return fail(d, I226_ERR_PARAM, "config value", I226_R_CONFIG);
    uint16_t cal, lim; float lsb;
    if (i226_compute_cal(cfg->r_shunt_ohm, cfg->current_lsb_a, &cal, &lsb) != I226_OK)
        return fail(d, I226_ERR_PARAM, "calibration", I226_R_CAL);
    if (i226_sol_limit(cfg->sol_trip_a, cfg->r_shunt_ohm, &lim) != I226_OK)
        return fail(d, I226_ERR_PARAM, "SOL limit", I226_R_ALERT);

    /* 1. identity before any write (strict AND) */
    uint16_t v;
    if (!rd16(d, I226_R_MFR_ID, &v)) return fail(d, I226_ERR_NO_DEVICE, "read FEh", I226_R_MFR_ID);
    d->mfr_id = v; d->mfr_read = true;
    if (!rd16(d, I226_R_DIE_ID, &v)) return fail(d, I226_ERR_TRANSPORT, "read FFh", I226_R_DIE_ID);
    d->die_id = v; d->die_read = true;
    if (d->mfr_id != I226_MFR_TI) return fail(d, I226_ERR_MFR_ID, "check FEh", I226_R_MFR_ID);
    if (d->die_id != 0x2260u && d->die_id != 0x2261u) return fail(d, I226_ERR_DIE_ID, "check FFh", I226_R_DIE_ID);

    /* 2. reset, bounded wait, default check */
    if (!wr16(d, I226_R_CONFIG, I226_CONFIG_RST)) return fail(d, I226_ERR_TRANSPORT, "write RST", I226_R_CONFIG);
    if (bus->delay_ms) bus->delay_ms(bus->ctx, 2);
    if (!rd16(d, I226_R_CONFIG, &v)) return fail(d, I226_ERR_TRANSPORT, "read CONFIG after RST", I226_R_CONFIG);
    if (v != I226_CONFIG_DEFAULT) return fail(d, I226_ERR_RESET, "CONFIG after RST", I226_R_CONFIG);

    /* 3. alerts off first, then configuration, each read back */
    if (wr_verify(d, I226_R_MASK, 0x0000, I226_MASK_RW_MASK, "MASK disable") != I226_OK) return d->status;
    if (wr_verify(d, I226_R_CONFIG, cfg->config, I226_CONFIG_RW_MASK, "CONFIG") != I226_OK) return d->status;
    if (wr_verify(d, I226_R_CAL, cal, I226_CAL_RW_MASK, "CAL") != I226_OK) return d->status;
    if (wr_verify(d, I226_R_ALERT, lim, 0xFFFF, "ALERT LIMIT") != I226_OK) return d->status;
    if (wr_verify(d, I226_R_MASK, I226_MASK_SOL_LATCH, I226_MASK_RW_MASK, "MASK SOL|LEN") != I226_OK) return d->status;

    d->config = cfg->config; d->cal = cal; d->alert_limit = lim; d->mask = I226_MASK_SOL_LATCH;
    d->r_shunt = cfg->r_shunt_ohm; d->current_lsb = lsb;
    d->calibration_verified = cfg->calibration_verified;
    d->protection_qualified = false;
    d->configured = true;
    d->status = I226_OK; d->fail_step = NULL; d->fail_reg = 0;
    return I226_OK;
}

i226_status_t ina226_reinit(ina226_dev_t *d, const ina226_bus_t *bus, const i226_cfg_t *cfg) {
    bool fault = d->fault_latched;
    uint16_t flags = d->fault_flags;
    uint32_t seq = d->seq, errors = d->transport_errors, overflows = d->overflow_count;
    i226_status_t st = ina226_init(d, bus, cfg);
    d->fault_latched |= fault;
    d->fault_flags |= flags;
    d->seq = seq; d->transport_errors += errors; d->overflow_count += overflows;
    return st;
}

static void latch_fault(ina226_dev_t *d, uint16_t flags) {
    d->fault_flags |= flags;
    d->fault_latched = true;
}
void ina226_alert_pin_event(ina226_dev_t *d) { latch_fault(d, d->fault_flags); }
void ina226_fault_ack(ina226_dev_t *d) { d->fault_latched = false; d->fault_flags = 0; }

bool ina226_read(ina226_dev_t *d, i226_sample_t *s, uint32_t now_ms) {
    s->valid = false;
    s->new_conversion = false;
    s->temperature_valid = false;
    if (!d->configured) { if (d->status == I226_OK) d->status = I226_ERR_NOT_INIT; return false; }
    uint16_t mask, cal, config, limit, sh, bus, cur, pw;
    uint32_t t0 = now_ms;
    s->t_start_ms = t0; s->t_end_ms = t0;
#define ACQ_END() (s->t_end_ms = d->bus->now_ms(d->bus->ctx))
    /* MASK first: the only place flags are read (reading clears AFF/CVRF in latch mode) */
    if (!rd16(d, I226_R_MASK, &mask)) { ACQ_END(); d->transport_errors++; d->status = I226_ERR_TRANSPORT; d->fail_step = "read MASK"; d->fail_reg = I226_R_MASK; return false; }
    if (mask & I226_MASK_AFF) latch_fault(d, mask);
    if ((mask & I226_MASK_RW_MASK) != d->mask) {      /* enable bits lost: sensor was reset */
        ACQ_END(); fail(d, I226_ERR_SENSOR_RESET, "MASK enable bits changed", I226_R_MASK);
        return false;
    }
    if (!rd16(d, I226_R_CAL, &cal)) { ACQ_END(); d->transport_errors++; d->status = I226_ERR_TRANSPORT; d->fail_step = "read CAL"; d->fail_reg = I226_R_CAL; return false; }
    if ((cal & I226_CAL_RW_MASK) != d->cal) { ACQ_END(); fail(d, I226_ERR_SENSOR_RESET, "CAL changed", I226_R_CAL); return false; }
    const uint8_t check_regs[2] = {I226_R_CONFIG, I226_R_ALERT};
    uint16_t *check_dst[2] = {&config, &limit};
    for (int k = 0; k < 2; k++) {
        if (!rd16(d, check_regs[k], check_dst[k])) {
            ACQ_END(); d->transport_errors++; d->status = I226_ERR_TRANSPORT;
            d->fail_step = "read integrity"; d->fail_reg = check_regs[k]; return false;
        }
    }
    if ((config & I226_CONFIG_RW_MASK) != (d->config & I226_CONFIG_RW_MASK) || limit != d->alert_limit) {
        ACQ_END(); fail(d, I226_ERR_SENSOR_RESET, "CONFIG/ALERT changed",
                       limit != d->alert_limit ? I226_R_ALERT : I226_R_CONFIG); return false;
    }
    const uint8_t regs[4] = {I226_R_SHUNT, I226_R_BUS, I226_R_CURRENT, I226_R_POWER};
    uint16_t *dst[4] = {&sh, &bus, &cur, &pw};
    for (int k = 0; k < 4; k++) {
        if (!rd16(d, regs[k], dst[k])) {
            ACQ_END(); d->transport_errors++; d->status = I226_ERR_TRANSPORT; d->fail_step = "read sample"; d->fail_reg = regs[k];
            return false;
        }
    }
    ACQ_END();
    if (mask & I226_MASK_OVF) { d->overflow_count++; d->status = I226_ERR_OVERFLOW; d->fail_step = "OVF"; d->fail_reg = I226_R_MASK; return false; }
    /* A successful transfer is not a new ADC conversion. Keep previous data invalid. */
    if (!(mask & I226_MASK_CVRF)) {
        d->status = ina226_stale(d, s->t_end_ms) ? I226_ERR_STALE : I226_WAIT_CONVERSION;
        d->fail_step = "no new conversion"; d->fail_reg = I226_R_MASK; return false;
    }
    /* CVRF was observed at acquisition start. Slow acquisition must not refresh age. */
    if ((uint32_t)(s->t_end_ms - t0) > I226_STALE_MS) {
        d->status = I226_ERR_STALE; d->fail_step = "acquisition deadline"; d->fail_reg = 0; return false;
    }
    /* complete sample: publish */
    s->raw_shunt = (int16_t)sh; s->raw_bus = bus; s->raw_current = (int16_t)cur; s->raw_power = pw;
    s->mask_flags = mask;
    s->v_bus = i226_bus_v(bus);
    s->i = (int16_t)cur * d->current_lsb;
    s->p_signed = s->v_bus * s->i;
    s->new_conversion = (mask & I226_MASK_CVRF) != 0;
    s->uncalibrated = !d->calibration_verified;
    s->seq = ++d->seq;
    s->valid = true;
    if (s->new_conversion) { d->last_new_ms = now_ms; d->have_new = true; }
    d->status = I226_OK; d->fail_step = NULL; d->fail_reg = 0;
#undef ACQ_END
    return true;
}

bool ina226_stale(const ina226_dev_t *d, uint32_t now_ms) {
    if (!d->configured || !d->have_new) return true;
    return (uint32_t)(now_ms - d->last_new_ms) > I226_STALE_MS;
}

const char *i226_status_str(i226_status_t s) {
    static const char *n[] = {"OK", "NO_DEVICE", "TRANSPORT", "MFR_ID_MISMATCH", "DIE_ID_MISMATCH", "RESET_FAILED",
                              "READBACK_MISMATCH", "BAD_PARAMETER", "NOT_INITIALISED", "SENSOR_RESET_DETECTED", "OVERFLOW", "WAIT_CONVERSION", "STALE"};
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}
