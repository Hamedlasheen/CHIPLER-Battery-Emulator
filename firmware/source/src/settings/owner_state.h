/* OWNER PHONE-TEST BUILD (r8): last loaded profile + emulated SOC survive a reset.
 *
 * Own flash sector (the one BEFORE the UI settings sector), append-only log of 32-byte records,
 * 128 per sector. The newest valid record wins; a record torn by power loss fails its CRC and is skipped.
 * Appending programs one 256-byte page (about 1 ms with interrupts parked). Review 105 R2: flash is NEVER written
 * while the battery target can be addressed. A record is written either while replies are off, or inside a
 * battery_bus_pause(): the target is switched off (address NACKed, SCL never held), the page is programmed, the same
 * map is re-armed. A pause starts only after a quiet bus gap (owner_state_when), so it falls between phone bursts.
 * Erasing the sector is slow (tens of ms, up to ~400 ms): never inside a pause (pass bus_running = true there), only
 * with replies off; until then the save waits and the last saved record stays valid.
 *
 * Only DATA TEST profiles are worth restoring; an empty profile id means "nothing live" (STOP pressed).
 * Record version 1. A record of any other version is ignored (state = none, defaults); there is no migration yet.
 * The pack/unpack/scan/plan half is pure and host-tested with a fake NOR flash (tests/host/test_owner_state.c):
 * the device half only executes the planned erase/program and reads back.
 */
#ifndef CHIPLER_OWNER_STATE_H
#define CHIPLER_OWNER_STATE_H
#include <stdbool.h>
#include <stdint.h>

#define OWNER_STATE_MAGIC   0x54534843u   /* "CHST" little endian */
#define OWNER_STATE_VERSION 1u
#define OWNER_STATE_REC     32u
#define OWNER_STATE_ID_LEN  16u           /* profile id, NUL padded (ids are at most 10 chars) */
#define OWNER_STATE_PAGE    256u          /* flash program unit (FLASH_PAGE_SIZE) */
#define OWNER_STATE_SECTOR  4096u         /* flash erase unit (FLASH_SECTOR_SIZE) */

typedef struct {
    char prof[OWNER_STATE_ID_LEN + 1];    /* "" = no live profile */
    int soc;                              /* 1..100 */
    uint32_t seq;
} owner_state_t;

typedef enum {
    OWNER_PLAN_SAME = 0,                  /* newest record already equals the request: nothing to write */
    OWNER_PLAN_WRITE,                     /* job filled: program one page (after an erase if job.erase) */
    OWNER_PLAN_WAIT_ERASE                 /* sector full and the bus is running: erase refused, retry after STOP */
} owner_plan_t;

typedef struct {
    bool erase;                           /* erase the whole sector first */
    uint32_t off;                         /* page offset inside the sector */
    uint32_t seq;                         /* sequence number of the new record */
    uint8_t page[OWNER_STATE_PAGE];       /* 0xFF except the new record (0xFF programs nothing on NOR flash) */
} owner_job_t;

void owner_state_pack(const owner_state_t *s, uint8_t out[OWNER_STATE_REC]);
bool owner_state_unpack(const uint8_t in[OWNER_STATE_REC], owner_state_t *s);
/* Scan a sector image: newest valid record into *s (false if none); *next_free = first erased slot after the
 * last written one, or -1 when the sector is full. */
bool owner_state_scan(const uint8_t *sector, unsigned len, owner_state_t *s, int *next_free);
bool owner_state_same(const owner_state_t *a, const owner_state_t *b);
/* Slots that are neither erased nor a valid record: a write (or an erase) was interrupted. */
unsigned owner_state_damaged(const uint8_t *sector, unsigned len);
/* Review 110 R110-1: boot decision. OFFER only when the newest valid record names a profile AND no slot is damaged;
 * a torn or partial write anywhere means the last event may not have reached flash (e.g. a STOP), so the boot is
 * UNCERTAIN and nothing is offered or started (fail-closed). NONE = no record, or the newest record is "none" (STOP). */
typedef enum { OWNER_BOOT_NONE = 0, OWNER_BOOT_OFFER, OWNER_BOOT_UNCERTAIN } owner_boot_t;
owner_boot_t owner_state_boot(const uint8_t *sector, unsigned len, owner_state_t *s);
/* Decide how to store *s given the current sector image (len = OWNER_STATE_SECTOR). With replies off, a sector that
 * holds a damaged slot is erased first, so one interrupted write cannot keep later boots UNCERTAIN forever. */
owner_plan_t owner_state_plan(const uint8_t *sector, const owner_state_t *s, bool bus_running, owner_job_t *job);
/* The two reserved sectors (owner state, then UI settings) must lie above the program image. */
bool owner_state_region_ok(uint32_t image_end_offset, uint32_t flash_size);
/* review 105 R2: when may a pending record be written? NOW = replies are off; PAUSE = replies on and the bus has
 * been quiet for quiet_us: switch the target off for the write; LATER = replies on and the bus is busy. */
typedef enum { OWNER_WHEN_IDLE = 0, OWNER_WHEN_NOW, OWNER_WHEN_PAUSE, OWNER_WHEN_LATER } owner_when_t;
owner_when_t owner_state_when(bool pending, bool due, bool bus_running, uint32_t idle_us, uint32_t quiet_us);

#ifdef PICO_ON_DEVICE
bool owner_state_load(owner_state_t *s);
owner_boot_t owner_state_boot_load(owner_state_t *s);   /* owner_state_boot() on the real sector (NONE if region bad) */
/* Result of the last owner_state_save(). */
typedef enum { OWNER_SAVE_OK = 0, OWNER_SAVE_WAIT_ERASE, OWNER_SAVE_FLASH_ERROR, OWNER_SAVE_REGION } owner_save_t;
owner_save_t owner_state_save(const owner_state_t *s, bool bus_running);
#endif
#endif
