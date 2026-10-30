/* Persistent UI settings (display rotation + touch calibration), P1_r1.
 *
 * Stored in the LAST 4 KiB flash sector with magic + version + CRC32. A blob that fails any check is
 * ignored (defaults, touch UNCALIBRATED). Nothing safety-relevant is stored here: SOC, profile and
 * arming are never persisted here (they reset on every boot by design; the OWNER phone-test build alone keeps the
 * last DATA TEST profile + SOC in its own sector, see owner_state.h).
 *
 * Writing erases flash, which stalls the calling core and requires the other core to be parked.
 * That would delay the battery I2C IRQ, so settings_save() REFUSES while the caller says the bus is
 * running. The pack/unpack/CRC half is pure and host-tested (tests/host/test_settings.c).
 */
#ifndef CHIPLER_SETTINGS_STORE_H
#define CHIPLER_SETTINGS_STORE_H
#include <stdbool.h>
#include <stdint.h>
#include "../input/touch_core.h"

#define SETTINGS_MAGIC    0x42534843u   /* "CHSB" little endian */
#define SETTINGS_VERSION  1u
#define SETTINGS_BLOB_LEN 64u

typedef struct {
    uint8_t rot180;
    touch_cal_t cal;       /* cal.valid false = never calibrated */
} ui_settings_t;

uint32_t settings_crc32(const uint8_t *p, unsigned n);
void settings_defaults(ui_settings_t *s);
void settings_pack(const ui_settings_t *s, uint8_t out[SETTINGS_BLOB_LEN]);
/* False (and *s = defaults) on bad magic/version/length/CRC/range. */
bool settings_unpack(const uint8_t in[SETTINGS_BLOB_LEN], ui_settings_t *s);

#ifdef PICO_ON_DEVICE
bool settings_load(ui_settings_t *s);
/* Refuses (returns false) while bus_running; never called from an IRQ. */
bool settings_save(const ui_settings_t *s, bool bus_running);
#endif
#endif
