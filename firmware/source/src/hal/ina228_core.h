/* INA228 driver core (V02-01). Pure C, no Pico SDK dependency: the I2C bus is injected so the
 * initialisation path can be fault-injection tested on the host.
 *
 * Readiness rule: the monitor is READY only if identification, reset, every configuration write
 * and the read-back of every written register succeeded, AND the bus over-voltage limit was
 * written and read back. Any failure leaves ready = false; the safety layer then treats the
 * monitor as absent (no MEASURED telemetry, output cannot be armed).
 * Register map / scaling: TI INA228 datasheet SBOSA20 - re-verify on the bench (S3). */
#ifndef CHIPLER_INA228_CORE_H
#define CHIPLER_INA228_CORE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define INA_R_CONFIG     0x00
#define INA_R_ADC_CONFIG 0x01
#define INA_R_SHUNT_CAL  0x02
#define INA_R_VSHUNT     0x04
#define INA_R_VBUS       0x05
#define INA_R_DIETEMP    0x06
#define INA_R_CURRENT    0x07
#define INA_R_POWER      0x08
#define INA_R_DIAG_ALRT  0x0B
#define INA_R_BOVL       0x0E
#define INA_R_MFG_ID     0x3E
#define INA_R_DEV_ID     0x3F

#define INA_CONFIG_RST      0x8000u
#define INA_CONFIG_VALUE    0x0010u   /* ADCRANGE = 1 (+-40.96 mV) */
#define INA_ADC_CONFIG_VALUE ((0xFu << 12) | (4u << 9) | (4u << 6) | (4u << 3) | 2u) /* cont. bus+shunt+temp, 540 us, 16x avg */

typedef struct {
    float v_bus;      /* V at VBUS pin */
    float i;          /* A, + = into phone (emulator supplies) */
    float p;          /* W */
    float t_die;      /* degC */
    bool  ok;
    bool  new_sample; /* DIAG_ALRT.CNVRF (bit 1): new conversion since the last read */
} ina228_reading_t;

typedef struct {
    /* write n bytes (register pointer + data) with STOP; true only if all bytes were ACKed */
    bool (*wr)(void *ctx, const uint8_t *buf, size_t n);
    /* write register pointer (repeated START) then read n bytes; true only on full success */
    bool (*rd)(void *ctx, uint8_t reg, uint8_t *buf, size_t n);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} ina228_bus_t;

typedef enum {
    INA_OK = 0, INA_ERR_NO_DEVICE, INA_ERR_ID, INA_ERR_RESET, INA_ERR_CONFIG, INA_ERR_ADC_CONFIG,
    INA_ERR_SHUNT_CAL, INA_ERR_READBACK, INA_ERR_OV_LIMIT, INA_ERR_NOT_INIT
} ina_status_t;

typedef struct {
    const ina228_bus_t *bus;
    bool ready;
    ina_status_t status;
    float current_lsb;
    uint16_t devid;
    uint16_t shunt_cal;
} ina228_dev_t;

ina_status_t ina228_dev_init(ina228_dev_t *d, const ina228_bus_t *bus, float r_shunt, float i_max);
/* Writes and reads back the bus over-voltage limit. Failure clears readiness (V02-01). */
ina_status_t ina228_dev_set_ov(ina228_dev_t *d, float volts);
bool ina228_dev_read(ina228_dev_t *d, ina228_reading_t *r);
const char *ina_status_str(ina_status_t s);
#endif
