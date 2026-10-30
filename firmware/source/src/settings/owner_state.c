#include "owner_state.h"
#include "settings_store.h"
#include <string.h>

/* record (little endian): 0 magic, 4 version, 5 soc, 6..7 zero, 8..23 profile id, 24 seq u32, 28 crc32 over 0..27 */
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

void owner_state_pack(const owner_state_t *s, uint8_t out[OWNER_STATE_REC]) {
    memset(out, 0, OWNER_STATE_REC);
    put32(out, OWNER_STATE_MAGIC);
    out[4] = (uint8_t)OWNER_STATE_VERSION;
    out[5] = (uint8_t)s->soc;
    size_t n = strlen(s->prof);
    memcpy(out + 8, s->prof, n < OWNER_STATE_ID_LEN ? n : OWNER_STATE_ID_LEN);
    put32(out + 24, s->seq);
    put32(out + 28, settings_crc32(out, 28));
}

bool owner_state_unpack(const uint8_t in[OWNER_STATE_REC], owner_state_t *s) {
    if (get32(in) != OWNER_STATE_MAGIC || in[4] != OWNER_STATE_VERSION || in[6] || in[7]) return false;
    if (get32(in + 28) != settings_crc32(in, 28)) return false;
    if (in[5] < 1 || in[5] > 100) return false;
    owner_state_t t;
    memset(&t, 0, sizeof t);
    memcpy(t.prof, in + 8, OWNER_STATE_ID_LEN);
    for (unsigned i = strlen(t.prof); i < OWNER_STATE_ID_LEN; i++) if (in[8 + i]) return false;   /* NUL padded only */
    t.soc = in[5]; t.seq = get32(in + 24);
    *s = t;
    return true;
}

static bool erased(const uint8_t *p) {
    for (unsigned i = 0; i < OWNER_STATE_REC; i++) if (p[i] != 0xFF) return false;
    return true;
}

bool owner_state_scan(const uint8_t *sector, unsigned len, owner_state_t *s, int *next_free) {
    unsigned n = len / OWNER_STATE_REC;
    int last_written = -1;
    bool found = false;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *r = sector + i * OWNER_STATE_REC;
        if (erased(r)) continue;
        last_written = (int)i;
        owner_state_t t;
        if (owner_state_unpack(r, &t) && (!found || (int32_t)(t.seq - s->seq) > 0)) { *s = t; found = true; }
    }
    *next_free = last_written + 1 < (int)n ? last_written + 1 : -1;
    return found;
}

unsigned owner_state_damaged(const uint8_t *sector, unsigned len) {
    unsigned n = len / OWNER_STATE_REC, bad = 0;
    owner_state_t t;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *r = sector + i * OWNER_STATE_REC;
        if (!erased(r) && !owner_state_unpack(r, &t)) bad++;
    }
    return bad;
}

owner_boot_t owner_state_boot(const uint8_t *sector, unsigned len, owner_state_t *s) {
    int nf;
    if (owner_state_damaged(sector, len)) return OWNER_BOOT_UNCERTAIN;
    if (!owner_state_scan(sector, len, s, &nf) || !s->prof[0]) return OWNER_BOOT_NONE;
    return OWNER_BOOT_OFFER;
}

bool owner_state_same(const owner_state_t *a, const owner_state_t *b) {
    return a->soc == b->soc && !strncmp(a->prof, b->prof, OWNER_STATE_ID_LEN);
}

owner_plan_t owner_state_plan(const uint8_t *sector, const owner_state_t *s, bool bus_running, owner_job_t *job) {
    owner_state_t last;
    int nf;
    bool have = owner_state_scan(sector, OWNER_STATE_SECTOR, &last, &nf);
    bool damaged = owner_state_damaged(sector, OWNER_STATE_SECTOR) != 0;
    /* same content but a damaged slot: rewrite (erase) with replies off, else boots would stay UNCERTAIN */
    if (have && owner_state_same(&last, s) && !(damaged && !bus_running)) return OWNER_PLAN_SAME;
    if (nf < 0 && bus_running) return OWNER_PLAN_WAIT_ERASE;
    owner_state_t rec = *s;
    rec.seq = have ? last.seq + 1 : 1;
    memset(job, 0xFF, sizeof *job);
    job->erase = nf < 0 || (damaged && !bus_running);
    job->seq = rec.seq;
    unsigned slot = job->erase ? 0 : (unsigned)nf;
    job->off = slot * OWNER_STATE_REC / OWNER_STATE_PAGE * OWNER_STATE_PAGE;
    owner_state_pack(&rec, job->page + (slot * OWNER_STATE_REC - job->off));
    return OWNER_PLAN_WRITE;
}

bool owner_state_region_ok(uint32_t image_end_offset, uint32_t flash_size) {
    return flash_size >= 2u * OWNER_STATE_SECTOR && image_end_offset <= flash_size - 2u * OWNER_STATE_SECTOR;
}

owner_when_t owner_state_when(bool pending, bool due, bool bus_running, uint32_t idle_us, uint32_t quiet_us) {
    if (!pending || !due) return OWNER_WHEN_IDLE;
    if (!bus_running) return OWNER_WHEN_NOW;
    return idle_us >= quiet_us ? OWNER_WHEN_PAUSE : OWNER_WHEN_LATER;
}

#ifdef PICO_ON_DEVICE
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"

_Static_assert(OWNER_STATE_PAGE == FLASH_PAGE_SIZE && OWNER_STATE_SECTOR == FLASH_SECTOR_SIZE, "flash geometry");
#define OWNER_STATE_OFFSET (PICO_FLASH_SIZE_BYTES - 2u * FLASH_SECTOR_SIZE)
extern char __flash_binary_end;
static const uint8_t *sector(void) { return (const uint8_t *)(XIP_BASE + OWNER_STATE_OFFSET); }
static bool region_ok(void) { return owner_state_region_ok((uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE), PICO_FLASH_SIZE_BYTES); }

bool owner_state_load(owner_state_t *s) {
    int nf;
    return region_ok() && owner_state_scan(sector(), FLASH_SECTOR_SIZE, s, &nf);
}
owner_boot_t owner_state_boot_load(owner_state_t *s) {
    return region_ok() ? owner_state_boot(sector(), FLASH_SECTOR_SIZE, s) : OWNER_BOOT_NONE;
}

static void do_write(void *param) {
    const owner_job_t *j = (const owner_job_t *)param;
    if (j->erase) flash_range_erase(OWNER_STATE_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(OWNER_STATE_OFFSET + j->off, j->page, FLASH_PAGE_SIZE);
}

owner_save_t owner_state_save(const owner_state_t *s, bool bus_running) {
    if (!region_ok()) return OWNER_SAVE_REGION;
    static owner_job_t job;
    switch (owner_state_plan(sector(), s, bus_running, &job)) {
    case OWNER_PLAN_SAME: return OWNER_SAVE_OK;
    case OWNER_PLAN_WAIT_ERASE: return OWNER_SAVE_WAIT_ERASE;
    case OWNER_PLAN_WRITE: break;
    }
    watchdog_update();
    int rc = flash_safe_execute(do_write, &job, 500);
    watchdog_update();
    if (rc != PICO_OK) return OWNER_SAVE_FLASH_ERROR;
    owner_state_t back;
    return owner_state_load(&back) && owner_state_same(&back, s) && back.seq == job.seq ? OWNER_SAVE_OK : OWNER_SAVE_FLASH_ERROR;
}
#endif
