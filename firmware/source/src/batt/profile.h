/* Battery protocol profiles.
 *
 * RULE: no invented iPhone register data. Real-phone profiles carry NO register responses
 * until a logic-analyzer capture of that exact model exists and has been reviewed.
 * The only profile with response data is BENCH_TEST, whose bytes are a synthetic test
 * pattern used to validate the RP2350 I2C target against an isolated master (Gate 2). */
#ifndef CHIPLER_PROFILE_H
#define CHIPLER_PROFILE_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PROF_UNVERIFIED_LOCKED = 0,  /* no evidence; output cannot be armed */
    PROF_IN_DEVELOPMENT,         /* under capture/analysis; output remains locked */
    PROF_BENCH_TEST,             /* synthetic, for isolated bench master only */
    PROF_VERIFIED,               /* physically validated (Phone-pass) - none yet */
    PROF_PHONE_DATA_TEST         /* owner-authorized bus experiment; powers a phone only with CHIPLER_OWNER_PHONE_TEST (not VERIFIED) */
} prof_status_t;

typedef enum { ADDR_UNKNOWN = 0, ADDR_CANDIDATE, ADDR_VERIFIED } addr_status_t;

/* Review 106: where a profile's live responses come from, kept apart from the model identity (id/family/name).
 * BK_SHARED_12PRO = the owner-requested shared implementation: the working 12 Pro engine and its recorded POWER capture
 * (P12P-C1 data) serve this model. Data source is the 12 Pro; compatibility of THIS model is NOT established until it
 * is tried on that phone. Never implies VERIFIED. */
typedef enum { BK_NONE = 0, BK_SHARED_12PRO } prof_backend_t;

typedef struct {
    uint8_t reg;        /* first register */
    uint8_t len;        /* bytes */
    const uint8_t *data;
    bool writable;      /* bench scratch only */
} reg_block_t;

typedef struct {
    const char *id;          /* short id, e.g. "P12PM-r0" */
    const char *family;      /* "iPhone 12" */
    const char *name;        /* "iPhone 12 Pro Max" */
    uint8_t i2c_addr;        /* 7-bit; 0 = unknown */
    addr_status_t addr_status;
    prof_status_t status;
    const char *evidence;    /* reference to capture/doc, or "NONE" */
    const char *auth_note;
    const reg_block_t *regs; /* NULL for all real-phone profiles today */
    uint8_t n_regs;
    /* CR-003 SOC capability. Builds the protocol image for an emulated SOC (25/50/100) into
     * img[0..*len-1] served at register *base. NULL = profile cannot apply SOC (all real-phone
     * profiles today: no verified register map/capacity/checksum exists). Must be filled ONLY
     * from verified capture evidence for a phone profile. */
    bool (*soc_build)(int soc_pct, uint8_t *img, uint8_t *base, uint8_t *len);
    /* Phone DATA TEST SOC (062): fills up to 4 (command, WORD value) pairs from the recorded 12 Pro Max register relation
     * 0x0D = round(100 * 0x0F / 0x10) (5/5 sessions, 04_Protocol/CODEX_SOC_EVIDENCE_062.md). Changes ONLY those registers. */
    bool (*soc_words)(int soc_pct, uint8_t *cmds, uint16_t *vals, uint8_t *n);
    /* Review 106: model generation as explicit data (10 = X/XS/XR, 11..17 = iPhone 11..17 incl. Air/16e);
     * 0 = not a phone model (bench, offline capture, internal reference). Family exclusions test THIS field. */
    uint8_t gen;
    prof_backend_t backend;
} batt_profile_t;
#define SOC_PUBLISH_WORDS 0xFE   /* publish 'base' marker: img = {cmd, lo, hi} triplets applied atomically (live_publish_soc) */

int profile_count(void);
const batt_profile_t *profile_get(int idx);
int profile_find(const char *id);
/* The ONLY way a phone profile can power a phone is status == PROF_VERIFIED (physically
 * qualified). There is deliberately no override parameter or build flag (FW-REVIEW-001 #2). */
/* true for every iPhone-12-family DATA TEST profile: one shared backend (12 Pro Max T06 samples), owner decision 060 */
bool profile_uses_12pm_backend(const batt_profile_t *p);
/* Which recorded 12 Pro capture serves this profile: 0 none, 1 POWER (no charger, P12P-C1), 2 CHARGER (P12P-C2).
 * OWNER PHONE-TEST build: the four 12-family DATA TEST entries (12 mini/12/12 Pro/12 Pro Max) also use the POWER
 * capture, because the T06 sample table lacks the boot commands (0x18/0xAF/0x4C...) and the phone never boots on it. */
int profile_12p_capture_state(const batt_profile_t *p);
/* Owner-requested phone-test build switch (default 0). 1 = DATA TEST profiles may arm the
 * output and power a real phone for owner bench experiments. NOT VERIFIED, NOT phone acceptance. */
#ifndef CHIPLER_OWNER_PHONE_TEST
#define CHIPLER_OWNER_PHONE_TEST 0
#endif

bool profile_armable(const batt_profile_t *p);
/* Review 106 (owner scope decision, not a protocol finding): families 15, 16 and 17 stay off the shared engine. */
#define PROFILE_EXCLUDED_GEN_MIN 15
bool profile_family_excluded(const batt_profile_t *p);
const char *profile_backend_str(const batt_profile_t *p);   /* "SHARED_12PRO", "OWN_CAPTURE", "T06_SAMPLES", "BENCH", "NONE" */
/* Internal, never-live reference selected by STOP / serial 'x' / boot: replies stay off (review 106). */
int profile_replies_off_index(void);
/* Bus policy input (main.c apply_bus_policy): does this profile want the battery target running?
 * output_live = safety state ARMED or ON. Pure: host-tested for every profile. */
bool profile_bus_wanted(const batt_profile_t *p, bool output_live);
/* Owner restore: index of a saved profile id that may be restored (a DATA TEST model), or -1 ("" = saved none). */
int profile_restore_index(const char *id);
int profile_count_armable(void);
const char *profile_status_str(prof_status_t s);
#endif
