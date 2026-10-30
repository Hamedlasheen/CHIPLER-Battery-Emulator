/* P1_r1 persistent settings blob: pack/unpack, CRC, corruption handling. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../src/settings/settings_store.h"

static unsigned fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
    /* CRC32 known answer ("123456789" -> CBF43926) */
    CHECK(settings_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);

    ui_settings_t s; settings_defaults(&s);
    s.rot180 = 1; s.cal.valid = true; s.cal.rot180 = 1; s.cal.w = 320; s.cal.h = 240;
    s.cal.a = -0.0123f; s.cal.b = 0.0861f; s.cal.c = -14.5f; s.cal.d = 0.0627f; s.cal.e = -0.0049f; s.cal.f = 8.25f;
    uint8_t blob[SETTINGS_BLOB_LEN]; settings_pack(&s, blob);
    ui_settings_t r;
    CHECK(settings_unpack(blob, &r));
    CHECK(r.rot180 == 1 && r.cal.valid && r.cal.rot180 == 1 && r.cal.w == 320 && r.cal.h == 240);
    CHECK(r.cal.a == s.cal.a && r.cal.b == s.cal.b && r.cal.c == s.cal.c && r.cal.d == s.cal.d && r.cal.e == s.cal.e && r.cal.f == s.cal.f);

    /* every single-bit flip in the protected region is detected, and leaves defaults (UNCALIBRATED) */
    unsigned detected = 0, total = 0;
    for (unsigned byte = 0; byte < SETTINGS_BLOB_LEN; byte++)
        for (unsigned bit = 0; bit < 8; bit++) {
            uint8_t b2[SETTINGS_BLOB_LEN]; memcpy(b2, blob, sizeof b2); b2[byte] ^= (uint8_t)(1u << bit);
            ui_settings_t x; total++;
            if (!settings_unpack(b2, &x)) { detected++; CHECK(!x.cal.valid && !x.rot180); }
        }
    printf("  bit-flip detection: %u / %u\n", detected, total);
    CHECK(detected == total);

    /* erased flash (all 0xFF) and zeroed flash are not accepted */
    { uint8_t ff[SETTINGS_BLOB_LEN]; memset(ff, 0xFF, sizeof ff); ui_settings_t x; CHECK(!settings_unpack(ff, &x) && !x.cal.valid);
      uint8_t z[SETTINGS_BLOB_LEN]; memset(z, 0, sizeof z); CHECK(!settings_unpack(z, &x)); }

    /* a blob with a VALID crc but out-of-range content (NaN / huge coefficients / bad size) is rejected */
    { ui_settings_t bad = s; bad.cal.a = NAN; uint8_t b2[SETTINGS_BLOB_LEN]; settings_pack(&bad, b2); ui_settings_t x; CHECK(!settings_unpack(b2, &x));
      bad = s; bad.cal.c = 1e9f; settings_pack(&bad, b2); CHECK(!settings_unpack(b2, &x));
      bad = s; bad.cal.w = 0; settings_pack(&bad, b2); CHECK(!settings_unpack(b2, &x)); }

    /* settings without calibration are still valid (rotation only) */
    { ui_settings_t only; settings_defaults(&only); only.rot180 = 1; uint8_t b2[SETTINGS_BLOB_LEN]; settings_pack(&only, b2);
      ui_settings_t x; CHECK(settings_unpack(b2, &x) && x.rot180 == 1 && !x.cal.valid); }

    printf("settings: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
