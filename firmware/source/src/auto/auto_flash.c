/* CHIPLER AUTO store, device half - see auto_flash.h. */
#include "auto_flash.h"
#ifdef PICO_ON_DEVICE
#include <string.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "../settings/owner_state.h"

_Static_assert(AUTO_PAGE == FLASH_PAGE_SIZE && AUTO_SECTOR == FLASH_SECTOR_SIZE, "flash geometry");
#define AUTO_OFFSET (PICO_FLASH_SIZE_BYTES - 2u * FLASH_SECTOR_SIZE)   /* same sector as owner_state (OWNER_STATE_OFFSET) */
extern char __flash_binary_end;
static const uint8_t *sector(void) { return (const uint8_t *)(XIP_BASE + AUTO_OFFSET); }
static bool region_ok(void) { return owner_state_region_ok((uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE), PICO_FLASH_SIZE_BYTES); }

bool auto_flash_scan(auto_store_t *s) {
    if (!region_ok()) { memset(s, 0, sizeof *s); s->next_free = -1; return false; }
    auto_store_scan(sector(), FLASH_SECTOR_SIZE, s);
    return true;
}
typedef struct { const auto_job_t *job; bool erase; } wjob_t;
static void do_write(void *param) {
    const wjob_t *w = (const wjob_t *)param;
    if (w->erase) flash_range_erase(AUTO_OFFSET, FLASH_SECTOR_SIZE);
    if (w->job) flash_range_program(AUTO_OFFSET + w->job->off, w->job->page, FLASH_PAGE_SIZE);
}
static auto_result_t run(const auto_job_t *j, bool erase) {
    wjob_t w = {j, erase};
    watchdog_update();
    int rc = flash_safe_execute(do_write, &w, 500);
    watchdog_update();
    if (rc != PICO_OK) return AR_FLASH;
    if (j && memcmp(sector() + j->off, j->page, FLASH_PAGE_SIZE) != 0) {
        /* bits the page did not set are 0xFF there and may hold older records: compare only the programmed record */
        for (unsigned i = 0; i < FLASH_PAGE_SIZE; i++) if (j->page[i] != 0xFF && sector()[j->off + i] != j->page[i]) return AR_FLASH;
    }
    return AR_OK;
}
static auto_result_t plan_result(auto_plan_t p) { return p == AUTO_PLAN_FULL ? AR_FULL : p == AUTO_PLAN_TABLE_FULL ? AR_TABLE_FULL : AR_OK; }
auto_result_t auto_flash_save_profile(const auto_profile_t *p) {
    if (!region_ok()) return AR_FLASH;
    static auto_job_t job;
    auto_plan_t pl = auto_plan_profile(sector(), p, &job);
    if (pl != AUTO_PLAN_WRITE) return plan_result(pl);
    auto_result_t r = run(&job, false);
    if (r != AR_OK) return r;
    auto_store_t s; auto_store_scan(sector(), FLASH_SECTOR_SIZE, &s);   /* read back through the same scanner the boot uses */
    int k = auto_store_find(&s, p->fp);
    return k >= 0 && s.prof[k].gen == p->gen && s.prof[k].soc == p->soc && s.seq == job.seq ? AR_OK : AR_FLASH;
}
auto_result_t auto_flash_save_control(bool en, uint32_t last_fp) {
    if (!region_ok()) return AR_FLASH;
    static auto_job_t job;
    auto_plan_t pl = auto_plan_control(sector(), en, last_fp, &job);
    if (pl != AUTO_PLAN_WRITE) return plan_result(pl);
    auto_result_t r = run(&job, false);
    if (r != AR_OK) return r;
    auto_store_t s; auto_store_scan(sector(), FLASH_SECTOR_SIZE, &s);
    return s.auto_enabled == en && !s.tail_damaged && s.seq == job.seq ? AR_OK : AR_FLASH;
}
auto_result_t auto_flash_clear(void) {
    if (!region_ok()) return AR_FLASH;
    auto_result_t r = run(0, true);
    if (r != AR_OK) return r;
    for (unsigned i = 0; i < FLASH_SECTOR_SIZE; i++) if (sector()[i] != 0xFF) return AR_FLASH;
    return AR_OK;
}
#endif
