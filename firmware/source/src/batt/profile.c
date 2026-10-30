#include "profile.h"
#include <string.h>

/* Synthetic bench pattern (NOT iPhone data): reg r returns (r ^ 0xA5), plus a writable
 * 16-byte scratch area at 0x80 for write-then-read tests. */
static const uint8_t bench_ro[16] = {
    0xA5, 0xA4, 0xA7, 0xA6, 0xA1, 0xA0, 0xA3, 0xA2, 0xAD, 0xAC, 0xAF, 0xAE, 0xA9, 0xA8, 0xAB, 0xAA };
static uint8_t bench_scratch[16];
/* SYNTHETIC SOC image for the isolated bench master ONLY - not an iPhone register map.
 * reg 0x20 = SOC %, 0x21..0x22 = synthetic "units" = SOC*40 (LE), 0x23 = 8-bit sum of 0x20..0x22.
 * Lets the bench test prove that multi-byte reads never mix old and new values. */
static bool bench_soc_build(int soc, uint8_t *img, uint8_t *base, uint8_t *len) {
    if (soc != 25 && soc != 50 && soc != 100) return false;
    uint16_t units = (uint16_t)(soc * 40);
    img[0] = (uint8_t)soc; img[1] = (uint8_t)units; img[2] = (uint8_t)(units >> 8);
    img[3] = (uint8_t)(img[0] + img[1] + img[2]);
    *base = 0x20; *len = 4;
    return true;
}

/* 12-family DATA TEST SOC (062). Evidence: in all five recorded sessions 0x0D == round(100 * 0x0F / 0x10) (0x0F/0x10 are the
 * recorded capacity-like words, units not settled). The T06 full-charge word 0x10 = 0x0B61 (2913) is kept; only 0x0D and 0x0F move.
 * 0x11/0x12/0x66[4..7]/0xA8/0x47 also vary with charge in the recordings but their relation to SOC is UNRESOLVED and they stay as recorded. */
#define T06_FCC 2913u
static bool phone12_soc_words(int soc, uint8_t *cmds, uint16_t *vals, uint8_t *n) {
#if CHIPLER_OWNER_PHONE_TEST
    if (soc < 1 || soc > 100) return false;   /* OWNER r7: any integer SOC; 0x0F is rescaled on the live 0x10 in main.c */
#else
    if (soc != 25 && soc != 50 && soc != 100) return false;
#endif
    cmds[0] = 0x0D; vals[0] = (uint16_t)soc;
    cmds[1] = 0x0F; vals[1] = (uint16_t)((T06_FCC * (unsigned)soc + 50u) / 100u);
    *n = 2; return true;
}

static const reg_block_t bench_regs[] = {
    { 0x00, 16, bench_ro, false },
    { 0x80, 16, bench_scratch, true },
};

#define NOEV "NONE - requires logic-analyzer capture"
#define NOAUTH "UNKNOWN - Apple pack authentication not characterised"
#define P(id, fam, nm, g) { id, fam, nm, 0x00, ADDR_UNKNOWN, PROF_UNVERIFIED_LOCKED, NOEV, NOAUTH, 0, 0, 0, 0, g, BK_NONE }

#if CHIPLER_OWNER_PHONE_TEST
#define B2_BACKEND BK_SHARED_12PRO   /* owner: the 12 family already served the POWER capture (r6/r7) */
/* Review 106 (owner request): every phone model outside families 15/16/17 runs on the working 12 Pro engine.
 * Identity (id/family/name) stays the model's own; data = 12 Pro POWER capture; address 0x0B is a CANDIDATE for this model
 * (reused, not observed on it); compatibility unproven until tried on that phone. */
#define SHARED12(id, fam, nm, g) { id, fam, nm, 0x0B, ADDR_CANDIDATE, PROF_PHONE_DATA_TEST, \
      "SHARED_12PRO: owner request 106; served from the 12 Pro POWER capture (sha256 66c7e61d); no capture of this model", \
      "Shared 12 Pro responses; authentication UNKNOWN; this model's compatibility NOT tested", 0, 0, 0, phone12_soc_words, g, BK_SHARED_12PRO }
#else
#define B2_BACKEND BK_NONE           /* default build: T06 table, policy unchanged */
#define SHARED12(id, fam, nm, g) P(id, fam, nm, g)   /* default build: still locked, policy unchanged */
#endif

#define P12_SHARED(id, nm) { id, "Phone data test", nm, 0x0B, ADDR_VERIFIED, PROF_PHONE_DATA_TEST, \
      "SHARED with 12 Pro Max T06 samples by owner decision 060; no separate capture of this model", \
      "Experimental responses only; auth UNKNOWN; shared code is not proof of identical authentication", 0, 0, 0, phone12_soc_words, 12, B2_BACKEND }

static const batt_profile_t s_profiles[] = {
    SHARED12("PX-r0",    "iPhone X",  "iPhone X",          10),
    SHARED12("PXS-r0",   "iPhone XS", "iPhone XS",         10),
    SHARED12("PXSM-r0",  "iPhone XS", "iPhone XS Max",     10),
    SHARED12("PXR-r0",   "iPhone XR", "iPhone XR",         10),
    SHARED12("P11-r0",   "iPhone 11", "iPhone 11",         11),
    SHARED12("P11P-r0",  "iPhone 11", "iPhone 11 Pro",     11),
    SHARED12("P11PM-r0", "iPhone 11", "iPhone 11 Pro Max", 11),
    /* iPhone 12 family (owner order 060): mini / 12 / 12 Pro SHARE the 12 Pro Max T06 backend and responses. This is an owner-directed
     * sharing decision, NOT evidence that these models were captured or that their authentication matches. Never VERIFIED. */
    P12_SHARED("P12m-B2", "12 mini DATA TEST"),
    P12_SHARED("P12-B2",  "12 DATA TEST"),
    P12_SHARED("P12P-B2", "12 Pro DATA TEST"),
    { "P12PM-B2", "Phone data test", "12 Pro Max DATA TEST", 0x0B, ADDR_VERIFIED, PROF_PHONE_DATA_TEST,
      "T06 samples; battery removed; independent regulated battery-level PSU; translated 1V8 data",
      "Experimental responses only; no authentication/phone acceptance claim", 0, 0, 0, phone12_soc_words, 12, B2_BACKEND },
#if CHIPLER_12P_REFERENCE
    /* 12 Pro RECORDED-REFERENCE profile from the owner's own 2026-10-23 captures. It installs the
     * POWER (no-charger) recorded rows only; CHARGER rows are shipped as data and are not wired
     * into this profile. Payloads are recorded reference data, NOT live telemetry. No authentication
     * exchange has been identified, so authentication stays UNVERIFIED and no phone acceptance is
     * claimed. The four 12-family DATA TEST profiles keep their existing shared backend (R4). */
    { "P12P-C1", "Phone data test", "12 Pro CAP REF", 0x0B, ADDR_VERIFIED, PROF_PHONE_DATA_TEST,
      "POWER 12M SR 2026-10-23 sha256 66c7e61d; recorded reference payloads (no-charger state)",
      "RECORDED reference; no authentication exchange identified; no phone-acceptance claim",
      0, 0, 0, phone12_soc_words, 12, BK_NONE },
    { "P12P-C2", "Phone data test", "12 Pro CHG REF", 0x0B, ADDR_VERIFIED, PROF_PHONE_DATA_TEST,
      "CHARGER 12M SR 2026-10-23 sha256 3e9eda37; recorded reference payloads (charger-connected state)",
      "RECORDED reference; no authentication exchange identified; no phone-acceptance claim",
      0, 0, 0, phone12_soc_words, 12, BK_NONE },   /* charger reference: unchanged by review 106 */
#endif
    /* Original 12 Pro Max captures confirm traffic at0x0B; semantics/auth incomplete.
     * Keep no live response map, no SOC builder, no qualification.
     * Review 106: also the internal REPLIES-OFF reference (profile_replies_off_index): gen 0, never on the shared engine. */
    { "P12PM-r0", "iPhone 12", "12PM old ref", 0x0B, ADDR_VERIFIED, PROF_IN_DEVELOPMENT,
      "T04/T05/T06 raw SR + CODEX_SMBUS_FIXTURES; offline replay only", NOAUTH, 0, 0, 0, 0, 0, BK_NONE },
    SHARED12("P13m-r0",  "iPhone 13", "iPhone 13 mini",    13),
    SHARED12("P13-r0",   "iPhone 13", "iPhone 13",         13),
    SHARED12("P13P-r0",  "iPhone 13", "iPhone 13 Pro",     13),
    SHARED12("P13PM-r0", "iPhone 13", "iPhone 13 Pro Max", 13),
    SHARED12("P14-r0",   "iPhone 14", "iPhone 14",         14),
    SHARED12("P14Pl-r0", "iPhone 14", "iPhone 14 Plus",    14),
    SHARED12("P14P-r0",  "iPhone 14", "iPhone 14 Pro",     14),
    SHARED12("P14PM-r0", "iPhone 14", "iPhone 14 Pro Max", 14),
    /* families 15/16/17: excluded by the owner (review 106); locked in every build */
    P("P15-r0",   "iPhone 15", "iPhone 15",         15),
    P("P15Pl-r0", "iPhone 15", "iPhone 15 Plus",    15),
    P("P15P-r0",  "iPhone 15", "iPhone 15 Pro",     15),
    P("P15PM-r0", "iPhone 15", "iPhone 15 Pro Max", 15),
    P("P16-r0",   "iPhone 16", "iPhone 16",         16),
    P("P16Pl-r0", "iPhone 16", "iPhone 16 Plus",    16),
    P("P16P-r0",  "iPhone 16", "iPhone 16 Pro",     16),
    P("P16PM-r0", "iPhone 16", "iPhone 16 Pro Max", 16),
    P("P16e-r0",  "iPhone 16", "iPhone 16e",        16),
    P("P17-r0",   "iPhone 17", "iPhone 17",         17),
    P("PAir-r0",  "iPhone 17", "iPhone Air",        17),
    P("P17P-r0",  "iPhone 17", "iPhone 17 Pro",     17),
    P("P17PM-r0", "iPhone 17", "iPhone 17 Pro Max", 17),
    { "BENCH-r0", "Bench", "Bench test master", 0x55, ADDR_CANDIDATE, PROF_BENCH_TEST,
      "Synthetic pattern for Gate 2 isolated-master test", "n/a", bench_regs, 2, bench_soc_build, 0, 0, BK_NONE },
    { "SMB-BENCH", "Bench", "SMBus SOC bench", 0x0B, ADDR_CANDIDATE, PROF_BENCH_TEST,
      "Explicit synthetic SMBus block 0x20; no phone semantics", "n/a", 0, 0, bench_soc_build, 0, 0, BK_NONE },
    { "CAP12PM-B1", "Bench capture", "12PM capture BENCH", 0x0B, ADDR_VERIFIED, PROF_BENCH_TEST,
      "beta1/Protocol/CODEX_CAPTURED_COMMAND_CATALOG.json; isolated master only",
      "Static recorded samples; authentication UNKNOWN; SOC NOT SENT", 0, 0, 0, 0, 0, BK_NONE },
};

int profile_count(void) { return (int)(sizeof s_profiles / sizeof s_profiles[0]); }
const batt_profile_t *profile_get(int i) { return (i >= 0 && i < profile_count()) ? &s_profiles[i] : 0; }
int profile_find(const char *id) {
    for (int i = 0; i < profile_count(); i++) if (!strcmp(s_profiles[i].id, id)) return i;
    return -1;
}
bool profile_uses_12pm_backend(const batt_profile_t *p) { return p && p->status == PROF_PHONE_DATA_TEST && p->i2c_addr == 0x0B; }
int profile_12p_capture_state(const batt_profile_t *p) {
    if (!p) return 0;
    if (!strcmp(p->id, "P12P-C1")) return 1;
    if (!strcmp(p->id, "P12P-C2")) return 2;
    /* review 106: the shared backend is profile DATA, not a list of ids (owner build only; the default build has none) */
    if (p->backend == BK_SHARED_12PRO && !profile_family_excluded(p)) return 1;
    return 0;
}
bool profile_family_excluded(const batt_profile_t *p) { return p && p->gen >= PROFILE_EXCLUDED_GEN_MIN; }
const char *profile_backend_str(const batt_profile_t *p) {
    if (!p) return "NONE";
    if (p->backend == BK_SHARED_12PRO) return "SHARED_12PRO";
    if (profile_12p_capture_state(p)) return "OWN_CAPTURE";
    if (profile_uses_12pm_backend(p)) return "T06_SAMPLES";   /* default build: 12 family DATA TEST on the T06 table */
    if (p->status == PROF_BENCH_TEST) return "BENCH";
    return "NONE";
}
int profile_replies_off_index(void) { return profile_find("P12PM-r0"); }
bool profile_bus_wanted(const batt_profile_t *p, bool output_live) {
    if (p && (p->status == PROF_BENCH_TEST || p->status == PROF_PHONE_DATA_TEST)) return true;   /* bench master / data experiment */
    return output_live;
}
int profile_restore_index(const char *id) {
    if (!id || !id[0]) return -1;   /* saved "none" (STOP) */
    int i = profile_find(id);
    const batt_profile_t *p = profile_get(i);
    return p && p->status == PROF_PHONE_DATA_TEST && !profile_family_excluded(p) ? i : -1;
}
bool profile_armable(const batt_profile_t *p) {
    /* LOCKED, IN_DEVELOPMENT and BENCH_TEST never power a phone, in any build */
    if (!p) return false;
    if (p->status == PROF_VERIFIED) return true;
#if CHIPLER_OWNER_PHONE_TEST
    /* OWNER PHONE-TEST BUILD: the owner explicitly authorised powering a real phone from a
     * DATA TEST profile. Still NOT VERIFIED and carries no authentication/acceptance claim.
     * Review 110 R110-2: never for families 15/16/17, whatever their status says (defence in depth). */
    if (p->status == PROF_PHONE_DATA_TEST && !profile_family_excluded(p)) return true;
#endif
    return false;
}
int profile_count_armable(void) {
    int n = 0;
    for (int i = 0; i < profile_count(); i++) if (profile_armable(profile_get(i))) n++;
    return n;
}
const char *profile_status_str(prof_status_t s) {
    switch (s) {
    case PROF_UNVERIFIED_LOCKED: return "UNVERIFIED \x80 LOCKED";
    case PROF_IN_DEVELOPMENT:    return "IN DEVELOPMENT \x80 LOCKED";
    case PROF_BENCH_TEST:        return "BENCH TEST ONLY";
    case PROF_PHONE_DATA_TEST:
#if CHIPLER_OWNER_PHONE_TEST
        return "PHONE TEST - OWNER UNLOCK";
#else
        return "PHONE DATA TEST ONLY";
#endif
    case PROF_VERIFIED:          return "VERIFIED";
    }
    return "?";
}
