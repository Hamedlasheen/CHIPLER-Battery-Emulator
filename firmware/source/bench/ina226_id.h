/* PM-ID-001 bench: pure INA226 identity verdict (host-testable).
 * r2 (Codex CHANGES REQUESTED): strict AND of CONFIG, FEh and FFh; transport failure separated.
 * Values: TI SBOS547C Table 7-1 - CONFIG reset default 4127h, Manufacturer ID (FEh) 5449h,
 * Die ID (FFh) 2260h or 2261h. */
#ifndef CHIPLER_INA226_ID_H
#define CHIPLER_INA226_ID_H
#include <stdbool.h>
#include <stdint.h>
#define INA226_CFG_DEFAULT 0x4127u
#define INA226_MFR_TI      0x5449u
typedef enum {
    ID_PASS = 0,        /* CONFIG == 4127h AND FEh == 5449h AND FFh in {2260h, 2261h} */
    ID_TRANSPORT,       /* NAK/timeout/short transfer on any step: identity not evaluated */
    ID_CONFIG_MISMATCH, /* cause unresolved: reset not applied, read error, or not INA226 */
    ID_MFR_MISMATCH,
    ID_DIE_MISMATCH,
} ina226_id_verdict_t;
static inline ina226_id_verdict_t ina226_id_verdict(bool transport_ok, uint16_t cfg, uint16_t fe, uint16_t ff) {
    if (!transport_ok) return ID_TRANSPORT;
    if (cfg != INA226_CFG_DEFAULT) return ID_CONFIG_MISMATCH;
    if (fe != INA226_MFR_TI) return ID_MFR_MISMATCH;
    if (ff != 0x2260u && ff != 0x2261u) return ID_DIE_MISMATCH;
    return ID_PASS;
}
/* Raw register scaling (datasheet): shunt LSB 2.5 uV (signed), bus LSB 1.25 mV. */
static inline float ina226_shunt_uv(uint16_t raw) { return (int16_t)raw * 2.5f; }
static inline float ina226_bus_mv(uint16_t raw) { return raw * 1.25f; }
#endif
