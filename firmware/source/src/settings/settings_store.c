#include "settings_store.h"
#include <math.h>
#include <string.h>

uint32_t settings_crc32(const uint8_t *p, unsigned n) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1u));
    }
    return ~c;
}

void settings_defaults(ui_settings_t *s) { memset(s, 0, sizeof *s); }

/* layout (little endian): 0 magic, 4 version u16, 6 len u16, 8 rot180, 9 cal_valid, 10 cal_rot, 11 pad,
 * 12 w u16, 14 h u16, 16..39 six floats, 40..59 reserved zero, 60 crc32 over bytes 0..59 */
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void putf(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); put32(p, u); }
static float getf(const uint8_t *p) { uint32_t u = get32(p); float f; memcpy(&f, &u, 4); return f; }

void settings_pack(const ui_settings_t *s, uint8_t out[SETTINGS_BLOB_LEN]) {
    memset(out, 0, SETTINGS_BLOB_LEN);
    put32(out, SETTINGS_MAGIC);
    out[4] = (uint8_t)SETTINGS_VERSION; out[5] = 0;
    out[6] = SETTINGS_BLOB_LEN; out[7] = 0;
    out[8] = s->rot180 ? 1 : 0;
    out[9] = s->cal.valid ? 1 : 0;
    out[10] = s->cal.rot180 ? 1 : 0;
    out[12] = (uint8_t)s->cal.w; out[13] = (uint8_t)(s->cal.w >> 8);
    out[14] = (uint8_t)s->cal.h; out[15] = (uint8_t)(s->cal.h >> 8);
    putf(out + 16, s->cal.a); putf(out + 20, s->cal.b); putf(out + 24, s->cal.c);
    putf(out + 28, s->cal.d); putf(out + 32, s->cal.e); putf(out + 36, s->cal.f);
    put32(out + 60, settings_crc32(out, 60));
}

bool settings_unpack(const uint8_t in[SETTINGS_BLOB_LEN], ui_settings_t *s) {
    settings_defaults(s);
    if (get32(in) != SETTINGS_MAGIC) return false;
    if (in[4] != SETTINGS_VERSION || in[5] != 0 || in[6] != SETTINGS_BLOB_LEN || in[7] != 0) return false;
    if (get32(in + 60) != settings_crc32(in, 60)) return false;
    if (in[8] > 1 || in[9] > 1 || in[10] > 1) return false;
    ui_settings_t t;
    settings_defaults(&t);
    t.rot180 = in[8];
    t.cal.valid = in[9]; t.cal.rot180 = in[10];
    t.cal.w = (uint16_t)(in[12] | (in[13] << 8)); t.cal.h = (uint16_t)(in[14] | (in[15] << 8));
    t.cal.a = getf(in + 16); t.cal.b = getf(in + 20); t.cal.c = getf(in + 24);
    t.cal.d = getf(in + 28); t.cal.e = getf(in + 32); t.cal.f = getf(in + 36);
    if (t.cal.valid) {
        float v[6] = {t.cal.a, t.cal.b, t.cal.c, t.cal.d, t.cal.e, t.cal.f};
        for (int i = 0; i < 6; i++) if (!isfinite(v[i]) || fabsf(v[i]) > 4000.0f) return false;
        if (t.cal.w == 0 || t.cal.h == 0 || t.cal.w > 1024 || t.cal.h > 1024) return false;
    }
    *s = t;
    return true;
}

#ifdef PICO_ON_DEVICE
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/xip_cache.h"

#define SETTINGS_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

bool settings_load(ui_settings_t *s) {
    uint8_t blob[SETTINGS_BLOB_LEN];
    memcpy(blob, (const void *)(XIP_BASE + SETTINGS_OFFSET), sizeof blob);
    return settings_unpack(blob, s);
}

typedef struct { uint8_t page[FLASH_PAGE_SIZE]; } write_job_t;
static void do_write(void *param) {
    const write_job_t *j = (const write_job_t *)param;
    flash_range_erase(SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_OFFSET, j->page, FLASH_PAGE_SIZE);
}

bool settings_save(const ui_settings_t *s, bool bus_running) {
    if (bus_running) return false;      /* would stall the battery I2C IRQ */
    static write_job_t job;
    memset(&job, 0xFF, sizeof job);
    settings_pack(s, job.page);
    watchdog_update();
    int rc = flash_safe_execute(do_write, &job, 500);
    watchdog_update();
    if (rc != PICO_OK) return false;
    ui_settings_t back;
    return settings_load(&back) && back.rot180 == s->rot180 && back.cal.valid == s->cal.valid;
}
#endif
