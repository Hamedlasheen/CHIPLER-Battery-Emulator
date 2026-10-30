/* CHIPLER AUTO (AUTO_LEARN_P1_RC1): phone-independent request matching, profile generation and
 * power-loss-safe storage. PURE logic (no SDK, no flash, no clock): host-tested in tests/host/test_auto_core.c.
 *
 * Words used here (handoff 2026-10-29):
 *   observation  = what the phone REQUESTED at the target address (command + direction + length). Source is the
 *                  SMBus target callback at the emulated address, NOT a wire sniffer: other addresses are invisible.
 *   template     = a response map that exists on the board with evidence behind it (captured, PEC-valid rows).
 *                  Only templates with runtime = true may ever answer; analysis-only entries never answer.
 *   profile      = {template, address, SOC, request fingerprint, generation}. Generated from a template, never from
 *                  the phone name. No model name, UDID or trial label enters the matcher.
 *   DATA GENERATED = the profile passed validation AND its record was committed and read back. Not "phone accepted".
 */
#ifndef CHIPLER_AUTO_CORE_H
#define CHIPLER_AUTO_CORE_H
#include <stdbool.h>
#include <stdint.h>

/* ---------------- observation ---------------- */
#define AUTO_K_PTR   0u   /* command-only transaction (pointer write, no data) */
#define AUTO_K_READ  1u
#define AUTO_K_WRITE 2u
/* one packed observation as produced by the IRQ path (smbus_target): cmd | kind<<8 | served<<10 | refused<<11 | len<<16 */
#define AUTO_OBS_PACK(cmd, kind, served, refused, len) \
    ((uint32_t)(cmd) | ((uint32_t)(kind) << 8) | ((uint32_t)((served) ? 1u : 0u) << 10) | ((uint32_t)((refused) ? 1u : 0u) << 11) | ((uint32_t)(len) << 16))
#define AUTO_OBS_CMD(o)     ((uint8_t)((o) & 0xFFu))
#define AUTO_OBS_KIND(o)    ((uint8_t)(((o) >> 8) & 3u))
#define AUTO_OBS_SERVED(o)  ((((o) >> 10) & 1u) != 0)
#define AUTO_OBS_REFUSED(o) ((((o) >> 11) & 1u) != 0)
#define AUTO_OBS_LEN(o)     ((uint8_t)(((o) >> 16) & 0xFFu))

typedef struct {
    uint32_t read[8], write[8];          /* command bitmaps (256 commands) */
    uint32_t n_read, n_write, n_ptr;     /* transactions */
    uint32_t first_ms, last_ms;          /* drain time of the first / last observation (not wire time) */
    uint32_t drops;                      /* observations lost before the main loop saw them (IRQ ring full) */
    bool any;
} auto_obs_t;
void auto_obs_reset(auto_obs_t *o);
void auto_obs_add(auto_obs_t *o, uint32_t packed, uint32_t now_ms);
void auto_obs_drop(auto_obs_t *o, uint32_t n);
int auto_bits_count(const uint32_t b[8]);
uint32_t auto_fingerprint(const auto_obs_t *o, uint8_t addr7);   /* CRC32 of addr + read/write sets; 0 never returned */

/* ---------------- templates ---------------- */
typedef struct {
    uint8_t id;                          /* stable id stored in profiles (never reused for another map) */
    const char *name;
    uint8_t addr7;
    bool runtime;                        /* false = analysis dictionary only: never answers, never matched for runtime */
    uint32_t read[8], write[8];          /* commands this template can answer / accept */
} auto_template_t;
void auto_tmpl_set(uint32_t b[8], uint8_t cmd);
bool auto_tmpl_has(const uint32_t b[8], uint8_t cmd);

/* ---------------- matcher ---------------- */
typedef enum {
    AM_WAITING = 0,          /* no request seen yet */
    AM_NEED_MORE,            /* requests seen, not enough to decide (window not over) */
    AM_COMPATIBLE,           /* one runtime template covers the requests with the required evidence */
    AM_PARTIAL,              /* window over, few distinct requests, ALL covered by exactly one template (low confidence) */
    AM_UNKNOWN,              /* requests the board has no template for (too many uncovered) */
    AM_AMBIGUOUS,            /* two runtime templates tie: nothing is chosen */
    AM_INCOMPLETE            /* observations were dropped: this window is not evidence */
} auto_match_kind_t;
typedef struct {
    uint32_t min_window_ms;   /* do not decide before this much time after the first request */
    uint32_t max_window_ms;   /* after this, decide PARTIAL/UNKNOWN instead of waiting forever */
    uint8_t min_covered;      /* distinct covered READ commands needed for COMPATIBLE */
    uint8_t max_uncovered_pct;/* uncovered share of distinct commands tolerated for COMPATIBLE */
} auto_match_cfg_t;
extern const auto_match_cfg_t AUTO_MATCH_DEFAULT;
typedef struct {
    auto_match_kind_t kind;
    int tmpl;                 /* index into the template array, -1 if none */
    uint8_t distinct, covered, uncovered, first_uncovered;
    char reason[64];          /* deterministic, human readable */
} auto_match_t;
auto_match_t auto_match(const auto_obs_t *o, const auto_template_t *t, int n, const auto_match_cfg_t *cfg, uint32_t now_ms);
const char *auto_match_str(auto_match_kind_t k);

/* ---------------- profile ---------------- */
#define AUTO_PF_PARTIAL 0x01u     /* generated from a PARTIAL (low-confidence) match */
typedef struct {
    uint32_t fp;              /* request fingerprint */
    uint8_t tmpl_id, addr7, soc, flags;
    uint32_t gen;             /* generation counter (monotonic in the store) */
} auto_profile_t;
bool auto_generate(const auto_match_t *m, const auto_obs_t *o, const auto_template_t *t, int n, int soc, uint32_t gen,
                   auto_profile_t *out);
const auto_template_t *auto_tmpl_by_id(const auto_template_t *t, int n, uint8_t id);
bool auto_validate(const auto_profile_t *p, const auto_template_t *t, int n);
uint32_t auto_profile_hash(const auto_profile_t *p);

/* ---------------- store: append-only log of 32-byte records in ONE flash sector ----------------
 * record: 0 magic 'CHAU', 4 version, 5 kind, 6..7 zero, 8 seq u32, 12..27 body, 28 crc32(0..27)
 *   PROFILE body: fp u32, tmpl u8, addr u8, soc u8, flags u8, gen u32, hash u32
 *   CONTROL body: auto_enabled u8 (1 = AUTO may run), 3 zero, last_fp u32, 8 zero
 * A torn/partial slot fails its CRC: it is skipped and counted as damaged. Records of the r8..r13 owner state
 * ('CHST') in the same sector are recognised as foreign and ignored (not damaged). */
#define AUTO_MAGIC     0x55414843u   /* "CHAU" little endian */
#define AUTO_VERSION   1u
#define AUTO_REC       32u
#define AUTO_PAGE      256u
#define AUTO_SECTOR    4096u
#define AUTO_MAX_PROFILES 8
#define AUTO_KIND_PROFILE 1u
#define AUTO_KIND_CONTROL 2u
typedef struct {
    auto_profile_t prof[AUTO_MAX_PROFILES];
    int n;
    bool auto_enabled;        /* newest CONTROL; true when no CONTROL was ever written (first use) */
    uint32_t last_fp;         /* fingerprint of the last profile that ran (diagnostic) */
    uint32_t seq, max_gen;
    int next_free;            /* first erased slot after the last written one, -1 = sector full */
    unsigned damaged;         /* slots neither erased nor valid */
    bool tail_damaged;        /* the LAST written slot is damaged: the newest event (maybe a STOP) is lost */
    unsigned foreign;         /* valid records of another format (owner state CHST) */
} auto_store_t;
void auto_rec_pack_profile(const auto_profile_t *p, uint32_t seq, uint8_t out[AUTO_REC]);
void auto_rec_pack_control(bool auto_enabled, uint32_t last_fp, uint32_t seq, uint8_t out[AUTO_REC]);
void auto_store_scan(const uint8_t *sector, unsigned len, auto_store_t *s);
int auto_store_find(const auto_store_t *s, uint32_t fp);
/* boot policy: AUTO may start replies only if enabled AND the newest written slot is intact (a torn write may have been a STOP).
 * An older damaged slot followed by a valid record does not block: the later record is the newest event. */
bool auto_store_may_autorun(const auto_store_t *s);
typedef enum { AUTO_PLAN_SAME = 0, AUTO_PLAN_WRITE, AUTO_PLAN_FULL, AUTO_PLAN_TABLE_FULL } auto_plan_t;
typedef struct { uint32_t off, seq; uint8_t page[AUTO_PAGE]; } auto_job_t;
/* plan one record. FULL = no erased slot left (Clear needed); TABLE_FULL = 8 distinct profiles already stored. */
auto_plan_t auto_plan_profile(const uint8_t *sector, const auto_profile_t *p, auto_job_t *job);
auto_plan_t auto_plan_control(const uint8_t *sector, bool auto_enabled, uint32_t last_fp, auto_job_t *job);

/* ---------------- state machine ---------------- */
typedef enum {
    AS_BOOT = 0, AS_WAIT_PHONE, AS_OBSERVE, AS_GENERATING, AS_VALIDATING, AS_SAVING, AS_GENERATED, AS_RUNNING,
    AS_STOPPED, AS_AUTO_OFF, AS_UNKNOWN, AS_AMBIGUOUS, AS_INCOMPLETE, AS_SAVE_FAILED, AS_STORE_FULL, AS_UNCERTAIN
} auto_state_t;
const char *auto_state_str(auto_state_t s);
/* what the board does on the battery bus in a state */
typedef enum { AB_OFF = 0, AB_LISTEN, AB_RUN } auto_bus_t;
auto_bus_t auto_state_bus(auto_state_t s);
#endif
