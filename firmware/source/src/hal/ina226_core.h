/* INA226 driver core - CR-006-INA226 Phase A (Codex requirements, Claude implementation).
 * Pure C, no Pico SDK: the I2C bus is injected so every path is fault-injection tested on the host.
 * Datasheet: TI INA226 SBOS547 (CONFIG default 4127h, registers 00h-07h, MASK/ENABLE bits) and
 * SBOS547C Table 7-1 (FEh = 5449h, FFh = 2260h/2261h).
 *
 * State flags are separate and never imply each other (CR-006 identity/init contract item 6):
 *   configured            every write of init was read back correctly (identity strict AND first)
 *   calibration_verified  owner-supplied: shunt value measured and accepted (false in Phase A)
 *   protection_qualified  always false here: INA226 SOL is supplemental, R-03/V02-03 are OPEN
 * A configured sensor gives telemetry only. Nothing in this module can enable phone power. */
#ifndef CHIPLER_INA226_CORE_H
#define CHIPLER_INA226_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define I226_R_CONFIG   0x00
#define I226_R_SHUNT    0x01
#define I226_R_BUS      0x02
#define I226_R_POWER    0x03
#define I226_R_CURRENT  0x04
#define I226_R_CAL      0x05
#define I226_R_MASK     0x06
#define I226_R_ALERT    0x07
#define I226_R_MFR_ID   0xFE
#define I226_R_DIE_ID   0xFF

#define I226_MFR_TI          0x5449u
#define I226_CONFIG_DEFAULT  0x4127u   /* AVG=1, VBUSCT=VSHCT=1.1 ms, continuous shunt+bus */
#define I226_CONFIG_RST      0x8000u
#define I226_CONFIG_RW_MASK  0x0FFFu   /* AVG[11:9] VBUSCT[8:6] VSHCT[5:3] MODE[2:0]; [14:12] fixed 100b */
#define I226_CAL_RW_MASK     0x7FFFu   /* bit 15 unused */
#define I226_MASK_RW_MASK    0xFC03u   /* SOL SUL BOL BUL POL CNVR [15:10], APOL [1], LEN [0] */
#define I226_MASK_SOL        0x8000u
#define I226_MASK_AFF        0x0010u   /* alert function flag (read-only) */
#define I226_MASK_CVRF       0x0008u   /* conversion ready flag (read-only) */
#define I226_MASK_OVF        0x0004u   /* math overflow flag (read-only) */
#define I226_MASK_LEN        0x0001u
#define I226_MASK_SOL_LATCH  (I226_MASK_SOL | I226_MASK_LEN)   /* 0x8001, ALERT active-low */

#define I226_SHUNT_LSB_V     2.5e-6f
#define I226_BUS_LSB_V       1.25e-3f
#define I226_SHUNT_FS_V      0.08192f
#define I226_STALE_MS        100u      /* software validity window, NOT an electrical protection time */

typedef struct {
    bool (*wr)(void *ctx, const uint8_t *buf, size_t n);              /* true only if all bytes ACKed */
    bool (*rd)(void *ctx, uint8_t reg, uint8_t *buf, size_t n);       /* pointer + repeated START read */
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
    uint32_t (*now_ms)(void *ctx);                                 /* monotonic acquisition clock */
} ina226_bus_t;

typedef enum {
    I226_OK = 0,
    I226_ERR_NO_DEVICE,      /* first transfer not acknowledged */
    I226_ERR_TRANSPORT,      /* NAK/timeout/short transfer after the device answered once */
    I226_ERR_MFR_ID,
    I226_ERR_DIE_ID,
    I226_ERR_RESET,          /* RST did not clear, or CONFIG after reset != default */
    I226_ERR_READBACK,       /* write ACKed but read-back differs */
    I226_ERR_PARAM,          /* R / LSB / limit invalid or not representable */
    I226_ERR_NOT_INIT,
    I226_ERR_SENSOR_RESET,   /* CAL/CONFIG changed behind our back: re-init required */
    I226_ERR_OVERFLOW,       /* OVF set: sample not valid */
    I226_WAIT_CONVERSION,    /* complete transport but no new conversion */
    I226_ERR_STALE,          /* no fresh conversion within validity window */
} i226_status_t;

typedef struct {
    float    r_shunt_ohm;        /* PROVISIONAL until measured */
    float    current_lsb_a;      /* requested; actual LSB derived from the rounded CAL */
    uint16_t config;             /* CONFIG value (bit 15 must be 0) */
    float    sol_trip_a;         /* > 0; converted conservatively (never above the request) */
    bool     calibration_verified;
} i226_cfg_t;

typedef struct {
    float    v_bus;              /* V at VBUS */
    float    i;                  /* A, + = PSU -> load */
    float    p_signed;           /* W = v_bus * i (sign follows current) */
    int16_t  raw_shunt;
    uint16_t raw_bus;
    int16_t  raw_current;
    uint16_t raw_power;          /* diagnostic only, unsigned */
    uint16_t mask_flags;         /* MASK/ENABLE read at the start of this sample */
    uint32_t seq;                /* increments only for a complete valid sample */
    uint32_t t_start_ms, t_end_ms;
    bool     valid;
    bool     new_conversion;     /* CVRF was set */
    bool     uncalibrated;       /* !calibration_verified */
    bool     temperature_valid;  /* always false: INA226 has no die temperature */
} i226_sample_t;

typedef struct {
    const ina226_bus_t *bus;
    i226_status_t status;
    const char *fail_step;       /* NULL if none */
    uint8_t  fail_reg;
    uint16_t mfr_id, die_id;     /* only meaningful if *_read */
    bool     mfr_read, die_read;
    uint16_t config, cal, alert_limit, mask;
    float    r_shunt, current_lsb;
    bool     configured, calibration_verified, protection_qualified;
    bool     fault_latched;      /* software latch: set by ALERT pin event or AFF, cleared only by ack */
    uint16_t fault_flags;        /* MASK flags captured when the fault latched */
    uint32_t seq, last_new_ms;
    bool     have_new;
    uint32_t transport_errors, overflow_count;
} ina226_dev_t;

/* Pure helpers (also used by host tests). */
i226_status_t i226_compute_cal(float r_shunt, float lsb_req, uint16_t *cal, float *lsb_actual);
i226_status_t i226_sol_limit(float trip_a, float r_shunt, uint16_t *limit);
static inline float i226_shunt_v(int16_t raw) { return raw * I226_SHUNT_LSB_V; }
static inline float i226_bus_v(uint16_t raw) { return raw * I226_BUS_LSB_V; }

/* Order (CR-006): read FE/FF (strict AND) -> RST -> CONFIG==4127 -> MASK=0 -> CONFIG -> CAL ->
 * ALERT LIMIT -> MASK=SOL|LEN, each write read back. Output stays OFF throughout (not touched). */
i226_status_t ina226_init(ina226_dev_t *d, const ina226_bus_t *bus, const i226_cfg_t *cfg);
/* First construction uses init. Recovery MUST use reinit: preserves software faults,
 * sequence and diagnostic counters even when recovery fails. Core-0 serialized API. */
i226_status_t ina226_reinit(ina226_dev_t *d, const ina226_bus_t *bus, const i226_cfg_t *cfg);
/* One sample: MASK -> CAL/CONFIG/ALERT integrity -> SHUNT/BUS/CURRENT/POWER.
 * Publishes only a complete new conversion acquired within 100ms; otherwise valid=false,
 * sequence/data unchanged. Acquisition timestamps describe the latest attempted transfer.
 * now_ms is the acquisition start, from the same wrap-safe clock as bus->now_ms.
 * Continuous ADC registers are not an atomic simultaneous snapshot. */
bool ina226_read(ina226_dev_t *d, i226_sample_t *s, uint32_t now_ms);
/* True if no complete sample with a new conversion within I226_STALE_MS. */
bool ina226_stale(const ina226_dev_t *d, uint32_t now_ms);
/* Call when the ALERT pin is seen low (before anything reads MASK). Latches the fault. */
void ina226_alert_pin_event(ina226_dev_t *d);
/* Explicit acknowledgement. Clears ONLY the software latch; never re-enables anything. */
void ina226_fault_ack(ina226_dev_t *d);
const char *i226_status_str(i226_status_t s);
#endif
