/* OWNER PHONE-TEST: the live battery words written by main.c owner_battery_words() must land on the
 * recorded 12 Pro targets (P12P-C1 power, P12P-C2 charger) and be served with a valid PEC:
 *   0x0D/0x0F = SOC scaled on the profile's own 0x10, 0x09 = INA voltage in mV.
 * Same publish path as the firmware: live_publish_soc(SOC_PUBLISH_WORDS, {cmd,lo,hi}...). */
#include "../../src/batt/smbus_target.h"
#include "../../src/batt/live_prepare.h"
#include "../../src/batt/owner_words.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, fails, masked;
#define C(x) do{checks++;if(!(x)){fails++;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
static uint32_t guard(void *p){(void)p;unsigned o=masked;masked=1;return o;}
static void unguard(void *p,uint32_t v){(void)p;masked=v;}
static uint8_t ref_crc8(const uint8_t *p, unsigned n) {
    uint8_t c = 0;
    for (unsigned i = 0; i < n; i++) { c ^= p[i]; for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1)); }
    return c;
}
static bool rd_word(st_target_t *t, uint8_t cmd, uint16_t *v) {
    uint8_t d[2], pec, pre[5] = {0x16, cmd, 0x17};
    if (!st_on_rx(t, cmd) || !st_begin_read(t)) return false;
    if (!st_on_tx(t, &d[0]) || !st_on_tx(t, &d[1]) || !st_on_tx(t, &pec)) return false;
    st_on_nack(t); if (st_on_stop(t) != ST_OK) return false;
    pre[3] = d[0]; pre[4] = d[1];
    *v = (uint16_t)(d[0] | (d[1] << 8));
    return pec == ref_crc8(pre, 5);
}
/* F2: a soc_apply publish built on T06_FCC 2913 is rescaled on the target's own 0x10 before it is applied
 * (same calls as main.c publish_to_engine(); review 102 F-04: no hand copy of the firmware math any more) */
static bool publish_rescaled(st_target_t *t, const uint8_t *data, uint8_t len, uint16_t fcc) {
    uint8_t img[12];
    memcpy(img, data, len);
    if (!owner_rescale_words(img, len, fcc)) return false;
    return live_publish_soc(t, SOC_PUBLISH_WORDS, img, len);
}
/* same calls as main.c owner_battery_words() */
static bool publish_owner(st_target_t *t, int soc, uint16_t fcc, float v_bus) {
    uint8_t a[6], b[3];
    return owner_soc_image(a, soc, fcc) == 6 && owner_volt_image(b, v_bus) == 3 &&
           live_publish_soc(t, SOC_PUBLISH_WORDS, a, sizeof a) && live_publish_soc(t, SOC_PUBLISH_WORDS, b, sizeof b);
}
/* pure checks of the image builders: bounds, rounding, NaN */
static void images(void) {
    uint8_t a[6] = {0}, b[3] = {0};
    volatile float zero = 0.0f;
    C(owner_soc_image(a, 0, 2201) == 0 && owner_soc_image(a, 101, 2201) == 0 && owner_soc_image(a, 50, 0) == 0);
    C(owner_soc_image(a, 1, 2201) == 6 && a[0] == 0x0D && a[1] == 1 && a[3] == 0x0F && (a[4] | a[5] << 8) == 23);
    unsigned bad = 0;
    for (unsigned fcc = 100; fcc <= 4000; fcc += 7)
        for (unsigned s = 1; s <= 100; s++) if (100u * owner_remaining((uint16_t)fcc, s) / fcc != s) bad++;
    C(bad == 0);                                            /* F7 holds for every FCC >= 100 mAh */
    C(owner_volt_image(b, 2.79f) == 0 && owner_volt_image(b, 4.61f) == 0 && owner_volt_image(b, zero / zero) == 0);
    C(owner_volt_image(b, 3.9124f) == 3 && b[0] == 0x09 && (b[1] | b[2] << 8) == 3912);
    uint8_t w[9] = {0x09, 1, 0, 0x0F, 0xFF, 0xFF, 0x0D, 37, 0};
    C(owner_rescale_words(w, 9, 2201) && (w[4] | w[5] << 8) == owner_remaining(2201, 37) && w[1] == 1);
    uint8_t n[3] = {0x0F, 5, 0}; C(!owner_rescale_words(n, 3, 2201) && n[1] == 5);   /* no 0x0D: untouched */
}
/* review 105 R1: owner_initial_words() on a prepared (not yet answering) target: one atomic publish, failures */
static st_target_t *g_t; static unsigned g_pubs, g_last_len; static bool g_word_fail, g_pub_fail;
static bool iw_word(void *c, uint8_t cmd, uint16_t *v) { (void)c; return !g_word_fail && st_get_word(g_t, cmd, v); }
static bool iw_pub(void *c, uint8_t b, const uint8_t *d, uint8_t n) { (void)c; g_pubs++; g_last_len = n; return !g_pub_fail && live_publish_soc(g_t, b, d, n); }
static void initial_words(void) {
    const owner_bus_t bus = {iw_word, iw_pub, 0};
    st_target_t t; st_init(&t, 0x0B); C(st_set_guard(&t, guard, unguard, 0));
    C(live_prepare(&t, profile_get(profile_find("P12P-C1")), 50)); g_t = &t;
    uint16_t fcc = 0, v = 0, rec09 = 0; C(st_get_word(&t, 0x10, &fcc) && st_get_word(&t, 0x09, &rec09));
    g_pubs = 0; C(owner_initial_words(&bus, 37, true, 3.9124f) && g_pubs == 1 && g_last_len == 9);
    C(rd_word(&t, 0x0D, &v) && v == 37); C(rd_word(&t, 0x0F, &v) && v == owner_remaining(fcc, 37));
    C(rd_word(&t, 0x09, &v) && v == 3912);
    C(owner_initial_words(&bus, 55, false, 3.9f) && g_last_len == 6 && rd_word(&t, 0x09, &v) && v == 3912);   /* 0x09 untouched */
    C(owner_initial_words(&bus, 55, true, 4.9f) && g_last_len == 6);                                         /* out of range */
    g_pubs = 0;
    C(!owner_initial_words(&bus, 0, true, 3.9f) && !owner_initial_words(&bus, 101, true, 3.9f) && g_pubs == 0);
    g_word_fail = true; C(!owner_initial_words(&bus, 37, true, 3.9f) && g_pubs == 0); g_word_fail = false;
    g_pub_fail = true; C(!owner_initial_words(&bus, 37, true, 3.9f)); g_pub_fail = false;
    C(rd_word(&t, 0x0D, &v) && v == 55);                                                                      /* failed calls changed nothing */
    C(!owner_initial_words(0, 37, true, 3.9f));
    (void)rec09;
}
int main(void) {
    initial_words();
    images();
    static const char *ids[2] = {"P12P-C1", "P12P-C2"};
    static const int socs[3] = {25, 50, 100};
    for (int m = 0; m < 2; m++) {
        const batt_profile_t *p = profile_get(profile_find(ids[m]));
        C(p && p->status == PROF_PHONE_DATA_TEST);
        if (!p) continue;
        for (int s = 0; s < 3; s++) {
            st_target_t t; st_init(&t, 0x0B); C(st_set_guard(&t, guard, unguard, 0));
            C(live_prepare(&t, p, 50) && st_live_ready(&t));
            uint16_t fcc = 0, w = 0, rec08 = 0, after08 = 0;
            C(rd_word(&t, 0x10, &fcc) && fcc > 0);
            C(rd_word(&t, 0x08, &rec08));
            C(publish_owner(&t, socs[s], fcc, 3.912f));
            C(rd_word(&t, 0x0D, &w) && w == socs[s]);
            uint16_t rem = 0; C(rd_word(&t, 0x0F, &rem));
            C(rem == owner_remaining(fcc, (unsigned)socs[s]));
            C(100u * rem / fcc == (unsigned)socs[s]);           /* F7: integer SOC from 0x0F/0x10 equals 0x0D */
            /* F2: soc_apply-style publish with T06 0x0F (2913*soc/100) must land rescaled on this profile's 0x10 */
            {   uint16_t t06 = (uint16_t)((2913u * (unsigned)socs[s] + 50u) / 100u), r2 = 0, s2 = 0;
                uint8_t img[6] = {0x0D, (uint8_t)socs[s], 0, 0x0F, (uint8_t)t06, (uint8_t)(t06 >> 8)};
                C(publish_rescaled(&t, img, sizeof img, fcc));
                C(rd_word(&t, 0x0D, &s2) && s2 == socs[s]);
                C(rd_word(&t, 0x0F, &r2) && r2 == rem && r2 != t06);
            }
            C(rd_word(&t, 0x09, &w) && w == 3912);
            C(rd_word(&t, 0x10, &w) && w == fcc);            /* full charge stays as recorded */
            C(rd_word(&t, 0x08, &after08) && after08 == rec08); /* untouched words stay as recorded */
            printf("%s soc=%d: 0x10=%u 0x0F=%u 0x0D=%u 0x09=3912mV\n", ids[m], socs[s], fcc, rem, socs[s]);
        }
    }
    printf("owner_words: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
