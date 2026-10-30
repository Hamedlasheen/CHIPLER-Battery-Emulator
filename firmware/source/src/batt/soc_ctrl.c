#include "soc_ctrl.h"

const int k_soc_presets[SOC_N_PRESETS] = {25, 50, 100};

bool soc_valid(int pct) {
#if CHIPLER_OWNER_PHONE_TEST
    return pct >= 1 && pct <= 100;   /* OWNER r7: any integer SOC; the phone follows 0x0D/0x0F (bench 2026-10-27, 50 % shown) */
#else
    return pct == 25 || pct == 50 || pct == 100;
#endif
}

void soc_init(soc_ctrl_t *c) {
    c->requested = SOC_DEFAULT; c->applied = -1; c->last = SOC_RES_NONE; c->retry_pending = false;
}

static soc_result_t try_publish(soc_ctrl_t *c, const batt_profile_t *p, soc_publish_fn pub, void *ctx) {
    if (p && p->soc_words) {
        uint8_t cm[4], n = 0; uint16_t vl[4]; uint8_t img[12];
        if (!p->soc_words(c->requested, cm, vl, &n) || !n || n > 4) { c->retry_pending = false; return c->last = SOC_RES_NOT_SUPPORTED; }
        for (uint8_t k = 0; k < n; k++) { img[3*k] = cm[k]; img[3*k+1] = (uint8_t)vl[k]; img[3*k+2] = (uint8_t)(vl[k] >> 8); }
        if (!pub || !pub(ctx, SOC_PUBLISH_WORDS, img, (uint8_t)(3 * n))) { c->retry_pending = true; return c->last = SOC_RES_BUSY_RETRY; }
        c->applied = c->requested; c->retry_pending = false;
        return c->last = SOC_RES_APPLIED_PHONE_DATA;
    }
    if (!p || !p->soc_build) { c->retry_pending = false; return c->last = SOC_RES_NOT_SUPPORTED; }
    uint8_t img[32], base = 0, len = 0;
    if (!p->soc_build(c->requested, img, &base, &len) || len == 0 || len > sizeof img) {
        c->retry_pending = false; return c->last = SOC_RES_NOT_SUPPORTED;
    }
    if (!pub || !pub(ctx, base, img, len)) { c->retry_pending = true; return c->last = SOC_RES_BUSY_RETRY; }
    c->applied = c->requested; c->retry_pending = false;
    return c->last = (p->status == PROF_VERIFIED) ? SOC_RES_APPLIED_VERIFIED : SOC_RES_APPLIED_SYNTHETIC;
}

soc_result_t soc_apply(soc_ctrl_t *c, const batt_profile_t *p, int pct, soc_publish_fn pub, void *ctx) {
    if (!soc_valid(pct)) return SOC_RES_REJECTED;       /* state untouched */
    c->requested = pct;
    return try_publish(c, p, pub, ctx);
}

soc_result_t soc_service(soc_ctrl_t *c, const batt_profile_t *p, soc_publish_fn pub, void *ctx) {
    if (!c->retry_pending) return SOC_RES_NONE;
    return try_publish(c, p, pub, ctx);
}

soc_result_t soc_on_profile_change(soc_ctrl_t *c, const batt_profile_t *p, soc_publish_fn pub, void *ctx) {
    c->requested = SOC_DEFAULT; c->applied = -1;
    if (p && p->soc_words) { c->retry_pending = false; return c->last = SOC_RES_RECORDED_RETAINED; }   /* never overwrite the recorded 51 % with a silent default */
    return try_publish(c, p, pub, ctx);
}

const char *soc_result_str(soc_result_t r) {
    switch (r) {
    case SOC_RES_NONE: return "NONE";
    case SOC_RES_APPLIED_SYNTHETIC: return "APPLIED (BENCH SYNTHETIC)";
    case SOC_RES_APPLIED_VERIFIED: return "APPLIED (VERIFIED PROFILE)";
    case SOC_RES_APPLIED_PHONE_DATA: return "RESPONSE DATA UPDATED (PHONE NOT CONFIRMED)";
    case SOC_RES_RECORDED_RETAINED: return "RECORDED T06 VALUE SERVED (NO SOC APPLIED)";
    case SOC_RES_NOT_SUPPORTED: return "NOT SENT - PROFILE UNSUPPORTED";
    case SOC_RES_BUSY_RETRY: return "PENDING - RETRYING";
    case SOC_RES_REJECTED: return "REJECTED";
    }
    return "?";
}
