#include "ina228_core.h"

static bool rd16(ina228_dev_t *d, uint8_t reg, uint16_t *v) {
    uint8_t b[2];
    if (!d->bus->rd(d->bus->ctx, reg, b, 2)) return false;
    *v = (uint16_t)(b[0] << 8 | b[1]);
    return true;
}
static bool wr16(ina228_dev_t *d, uint8_t reg, uint16_t v) {
    uint8_t b[3] = {reg, (uint8_t)(v >> 8), (uint8_t)v};
    return d->bus->wr(d->bus->ctx, b, 3);
}
/* write, then read back and compare under mask */
static bool wr_verify(ina228_dev_t *d, uint8_t reg, uint16_t v, uint16_t mask, bool *readback_bad) {
    uint16_t r;
    *readback_bad = false;
    if (!wr16(d, reg, v)) return false;
    if (!rd16(d, reg, &r) || (r & mask) != (v & mask)) { *readback_bad = true; return false; }
    return true;
}
static int32_t s20(const uint8_t *b) {          /* 24-bit register, 20-bit value in [23:4] */
    int32_t v = ((int32_t)b[0] << 16) | ((int32_t)b[1] << 8) | b[2];
    if (v & 0x800000) v |= (int32_t)0xFF000000;
    return v >> 4;
}

static ina_status_t fail(ina228_dev_t *d, ina_status_t s) { d->ready = false; d->status = s; return s; }

ina_status_t ina228_dev_init(ina228_dev_t *d, const ina228_bus_t *bus, float r_shunt, float i_max) {
    d->bus = bus; d->ready = false; d->devid = 0; d->shunt_cal = 0; d->current_lsb = 0;
    uint16_t v;
    bool bad;
    if (!rd16(d, INA_R_MFG_ID, &v)) return fail(d, INA_ERR_NO_DEVICE);
    if (v != 0x5449) return fail(d, INA_ERR_ID);                       /* "TI" */
    if (!rd16(d, INA_R_DEV_ID, &v)) return fail(d, INA_ERR_NO_DEVICE);
    d->devid = v;
    if ((v >> 4) != 0x228) return fail(d, INA_ERR_ID);
    /* reset; RST self-clears */
    if (!wr16(d, INA_R_CONFIG, INA_CONFIG_RST)) return fail(d, INA_ERR_RESET);
    if (d->bus->delay_ms) d->bus->delay_ms(d->bus->ctx, 2);
    if (!rd16(d, INA_R_CONFIG, &v) || (v & INA_CONFIG_RST)) return fail(d, INA_ERR_RESET);
    if (!wr_verify(d, INA_R_CONFIG, INA_CONFIG_VALUE, 0xFFF0, &bad)) return fail(d, bad ? INA_ERR_READBACK : INA_ERR_CONFIG);
    if (!wr_verify(d, INA_R_ADC_CONFIG, INA_ADC_CONFIG_VALUE, 0xFFFF, &bad)) return fail(d, bad ? INA_ERR_READBACK : INA_ERR_ADC_CONFIG);
    d->current_lsb = i_max / 524288.0f;                                /* 2^19 */
    float cal = 13107.2e6f * d->current_lsb * r_shunt * 4.0f;           /* x4 for ADCRANGE = 1 */
    if (cal < 1.0f || cal > 32767.0f) return fail(d, INA_ERR_SHUNT_CAL);
    d->shunt_cal = (uint16_t)(cal + 0.5f);
    if (!wr_verify(d, INA_R_SHUNT_CAL, d->shunt_cal, 0x7FFF, &bad)) return fail(d, bad ? INA_ERR_READBACK : INA_ERR_SHUNT_CAL);
    /* not READY yet: the over-voltage limit must also be programmed (ina228_dev_set_ov) */
    d->status = INA_ERR_OV_LIMIT;
    return INA_OK;
}

ina_status_t ina228_dev_set_ov(ina228_dev_t *d, float volts) {
    if (!d->bus || d->current_lsb <= 0) return fail(d, INA_ERR_NOT_INIT);
    if (d->status != INA_OK && d->status != INA_ERR_OV_LIMIT) return d->status;   /* init failed earlier */
    if (!(volts > 0.0f && volts < 102.0f)) return fail(d, INA_ERR_OV_LIMIT);
    uint16_t v = (uint16_t)(volts / 3.125e-3f) & 0x7FFF;
    bool bad;
    if (!wr_verify(d, INA_R_BOVL, v, 0x7FFF, &bad)) return fail(d, bad ? INA_ERR_READBACK : INA_ERR_OV_LIMIT);
    d->ready = true; d->status = INA_OK;
    return INA_OK;
}

bool ina228_dev_read(ina228_dev_t *d, ina228_reading_t *r) {
    uint8_t b[3];
    r->ok = false; r->new_sample = false;
    if (!d->ready) return false;
    const ina228_bus_t *bus = d->bus;
    if (!bus->rd(bus->ctx, INA_R_DIAG_ALRT, b, 2)) return false;          /* reading clears CNVRF */
    r->new_sample = (b[1] & 0x02) != 0;
    if (!bus->rd(bus->ctx, INA_R_VBUS, b, 3)) return false;
    r->v_bus = s20(b) * 195.3125e-6f;
    if (!bus->rd(bus->ctx, INA_R_CURRENT, b, 3)) return false;
    r->i = s20(b) * d->current_lsb;
    if (!bus->rd(bus->ctx, INA_R_POWER, b, 3)) return false;
    uint32_t pw = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    r->p = pw * 3.2f * d->current_lsb;
    uint8_t t[2];
    if (bus->rd(bus->ctx, INA_R_DIETEMP, t, 2)) r->t_die = (int16_t)(t[0] << 8 | t[1]) * 7.8125e-3f;
    r->ok = true;
    return true;
}

const char *ina_status_str(ina_status_t s) {
    static const char *n[] = {"OK", "NO_DEVICE", "ID_MISMATCH", "RESET_FAILED", "CONFIG_WRITE_FAILED",
                              "ADC_CONFIG_WRITE_FAILED", "SHUNT_CAL_WRITE_FAILED", "READBACK_MISMATCH",
                              "OV_LIMIT_FAILED", "NOT_INITIALISED"};
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}
