/* CHIPLER AUTO core - see auto_core.h. Pure C: no SDK, no heap, no flash access. */
#include "auto_core.h"
#include <stdio.h>
#include <string.h>

static uint32_t crc32(const uint8_t *p, unsigned n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---------------- observation ---------------- */
void auto_obs_reset(auto_obs_t *o) { memset(o, 0, sizeof *o); }
void auto_tmpl_set(uint32_t b[8], uint8_t cmd) { b[cmd >> 5] |= 1u << (cmd & 31u); }
bool auto_tmpl_has(const uint32_t b[8], uint8_t cmd) { return (b[cmd >> 5] >> (cmd & 31u)) & 1u; }
int auto_bits_count(const uint32_t b[8]) {
    int n = 0;
    for (int i = 0; i < 8; i++) for (uint32_t v = b[i]; v; v &= v - 1) n++;
    return n;
}
void auto_obs_add(auto_obs_t *o, uint32_t packed, uint32_t now_ms) {
    uint8_t cmd = AUTO_OBS_CMD(packed), kind = AUTO_OBS_KIND(packed);
    if (!o->any) { o->any = true; o->first_ms = now_ms; }
    o->last_ms = now_ms;
    if (kind == AUTO_K_READ) { o->n_read++; auto_tmpl_set(o->read, cmd); }
    else if (kind == AUTO_K_WRITE) { o->n_write++; auto_tmpl_set(o->write, cmd); }
    else o->n_ptr++;   /* a pointer-only write names a command but asks nothing: not part of the signature */
}
void auto_obs_drop(auto_obs_t *o, uint32_t n) { o->drops += n; }
uint32_t auto_fingerprint(const auto_obs_t *o, uint8_t addr7) {
    uint8_t buf[1 + 64];
    buf[0] = addr7;
    for (int i = 0; i < 8; i++) { put32(buf + 1 + 4 * i, o->read[i]); put32(buf + 33 + 4 * i, o->write[i]); }
    uint32_t c = crc32(buf, sizeof buf);
    return c ? c : 1u;
}

/* ---------------- matcher ---------------- */
const auto_match_cfg_t AUTO_MATCH_DEFAULT = {1500u, 20000u, 4u, 20u};
const char *auto_match_str(auto_match_kind_t k) {
    switch (k) {
    case AM_WAITING: return "WAITING"; case AM_NEED_MORE: return "NEED_MORE"; case AM_COMPATIBLE: return "COMPATIBLE";
    case AM_PARTIAL: return "PARTIAL"; case AM_UNKNOWN: return "UNKNOWN"; case AM_AMBIGUOUS: return "AMBIGUOUS";
    case AM_INCOMPLETE: return "INCOMPLETE";
    }
    return "?";
}
typedef struct { int covered, uncovered, first_unc; } cover_t;
static cover_t cover(const auto_obs_t *o, const auto_template_t *t) {
    cover_t c = {0, 0, -1};
    for (int cmd = 0; cmd < 256; cmd++) {
        if (auto_tmpl_has(o->read, (uint8_t)cmd)) {
            if (auto_tmpl_has(t->read, (uint8_t)cmd)) c.covered++;
            else { c.uncovered++; if (c.first_unc < 0) c.first_unc = cmd; }
        }
        if (auto_tmpl_has(o->write, (uint8_t)cmd) && !auto_tmpl_has(t->write, (uint8_t)cmd)) {
            c.uncovered++; if (c.first_unc < 0) c.first_unc = cmd;
        }
    }
    return c;
}
auto_match_t auto_match(const auto_obs_t *o, const auto_template_t *t, int n, const auto_match_cfg_t *cfg, uint32_t now_ms) {
    auto_match_t m;
    memset(&m, 0, sizeof m);
    m.tmpl = -1;
    if (!cfg) cfg = &AUTO_MATCH_DEFAULT;
    int distinct = auto_bits_count(o->read) + auto_bits_count(o->write);
    m.distinct = (uint8_t)(distinct > 255 ? 255 : distinct);
    if (o->drops) {
        m.kind = AM_INCOMPLETE;
        snprintf(m.reason, sizeof m.reason, "capture lost %lu observations", (unsigned long)o->drops);
        return m;
    }
    if (!o->any || distinct == 0) { m.kind = AM_WAITING; snprintf(m.reason, sizeof m.reason, "no request yet"); return m; }
    uint32_t age = (int32_t)(now_ms - o->first_ms) > 0 ? now_ms - o->first_ms : 0u;   /* never wraps if now precedes the drain stamp */
    bool window_over = age >= cfg->max_window_ms;
    /* best runtime template: most covered, then fewest uncovered; a tie on both is AMBIGUOUS */
    int best = -1, tie = -1; cover_t bc = {0, 0, -1};
    for (int i = 0; i < n; i++) {
        if (!t[i].runtime) continue;   /* analysis dictionary never competes */
        cover_t c = cover(o, &t[i]);
        if (best < 0 || c.covered > bc.covered || (c.covered == bc.covered && c.uncovered < bc.uncovered)) { best = i; bc = c; tie = -1; }
        else if (c.covered == bc.covered && c.uncovered == bc.uncovered) tie = i;
    }
    if (best < 0) { m.kind = AM_UNKNOWN; snprintf(m.reason, sizeof m.reason, "no runtime template on board"); return m; }
    m.covered = (uint8_t)bc.covered; m.uncovered = (uint8_t)bc.uncovered;
    m.first_uncovered = (uint8_t)(bc.first_unc < 0 ? 0 : bc.first_unc);
    bool unc_ok = bc.uncovered * 100 <= cfg->max_uncovered_pct * distinct;
    if (tie >= 0 && bc.covered > 0) {
        m.kind = AM_AMBIGUOUS;
        snprintf(m.reason, sizeof m.reason, "templates %u and %u tie (%d covered)", t[best].id, t[tie].id, bc.covered);
        return m;
    }
    if (age < cfg->min_window_ms) { m.kind = AM_NEED_MORE; snprintf(m.reason, sizeof m.reason, "window %lu ms", (unsigned long)age); return m; }
    if (bc.covered >= cfg->min_covered && unc_ok) {
        m.kind = AM_COMPATIBLE; m.tmpl = best;
        snprintf(m.reason, sizeof m.reason, "T%u covers %d/%d (uncovered %d)", t[best].id, bc.covered, distinct, bc.uncovered);
        return m;
    }
    if (!unc_ok && distinct >= cfg->min_covered) {
        m.kind = AM_UNKNOWN;
        snprintf(m.reason, sizeof m.reason, "%d of %d requests unknown (first 0x%02X)", bc.uncovered, distinct, m.first_uncovered);
        return m;
    }
    if (window_over) {
        if (bc.uncovered == 0 && bc.covered > 0) {
            m.kind = AM_PARTIAL; m.tmpl = best;
            snprintf(m.reason, sizeof m.reason, "only %d requests, all covered by T%u (low confidence)", bc.covered, t[best].id);
        } else {
            m.kind = AM_UNKNOWN;
            snprintf(m.reason, sizeof m.reason, "window over: %d covered, %d unknown", bc.covered, bc.uncovered);
        }
        return m;
    }
    m.kind = AM_NEED_MORE;
    snprintf(m.reason, sizeof m.reason, "%d covered of %d needed", bc.covered, cfg->min_covered);
    return m;
}

/* ---------------- profile ---------------- */
const auto_template_t *auto_tmpl_by_id(const auto_template_t *t, int n, uint8_t id) {
    for (int i = 0; i < n; i++) if (t[i].id == id) return &t[i];
    return 0;
}
bool auto_generate(const auto_match_t *m, const auto_obs_t *o, const auto_template_t *t, int n, int soc, uint32_t gen,
                   auto_profile_t *out) {
    if (!m || !o || !out || (m->kind != AM_COMPATIBLE && m->kind != AM_PARTIAL) || m->tmpl < 0 || m->tmpl >= n) return false;
    if (!t[m->tmpl].runtime || soc < 1 || soc > 100) return false;
    memset(out, 0, sizeof *out);
    out->fp = auto_fingerprint(o, t[m->tmpl].addr7);
    out->tmpl_id = t[m->tmpl].id; out->addr7 = t[m->tmpl].addr7; out->soc = (uint8_t)soc;
    out->flags = m->kind == AM_PARTIAL ? AUTO_PF_PARTIAL : 0;
    out->gen = gen;
    return true;
}
bool auto_validate(const auto_profile_t *p, const auto_template_t *t, int n) {
    if (!p || !p->fp || p->soc < 1 || p->soc > 100 || (p->flags & ~AUTO_PF_PARTIAL)) return false;
    const auto_template_t *tp = auto_tmpl_by_id(t, n, p->tmpl_id);
    return tp && tp->runtime && tp->addr7 == p->addr7 && p->addr7 > 0 && p->addr7 < 0x78;
}
uint32_t auto_profile_hash(const auto_profile_t *p) {
    uint8_t b[12];
    put32(b, p->fp); b[4] = p->tmpl_id; b[5] = p->addr7; b[6] = p->soc; b[7] = p->flags; put32(b + 8, p->gen);
    return crc32(b, sizeof b);
}

/* ---------------- store ---------------- */
static void rec_head(uint8_t *out, uint8_t kind, uint32_t seq) {
    memset(out, 0, AUTO_REC);
    put32(out, AUTO_MAGIC); out[4] = AUTO_VERSION; out[5] = kind; put32(out + 8, seq);
}
void auto_rec_pack_profile(const auto_profile_t *p, uint32_t seq, uint8_t out[AUTO_REC]) {
    rec_head(out, AUTO_KIND_PROFILE, seq);
    put32(out + 12, p->fp); out[16] = p->tmpl_id; out[17] = p->addr7; out[18] = p->soc; out[19] = p->flags;
    put32(out + 20, p->gen); put32(out + 24, auto_profile_hash(p));
    put32(out + 28, crc32(out, 28));
}
void auto_rec_pack_control(bool en, uint32_t last_fp, uint32_t seq, uint8_t out[AUTO_REC]) {
    rec_head(out, AUTO_KIND_CONTROL, seq);
    out[12] = en ? 1 : 0; put32(out + 16, last_fp);
    put32(out + 28, crc32(out, 28));
}
static bool erased(const uint8_t *p) { for (unsigned i = 0; i < AUTO_REC; i++) if (p[i] != 0xFF) return false; return true; }
static bool foreign_chst(const uint8_t *p) {   /* r8..r13 owner state record: magic CHST + its own crc32 at 28 */
    return get32(p) == 0x54534843u && get32(p + 28) == crc32(p, 28);
}
void auto_store_scan(const uint8_t *sector, unsigned len, auto_store_t *s) {
    memset(s, 0, sizeof *s);
    s->auto_enabled = true;
    s->next_free = -1;
    uint32_t best_ctrl_seq = 0; bool have_ctrl = false;
    int last_used = -1;
    unsigned slots = len / AUTO_REC;
    for (unsigned i = 0; i < slots; i++) {
        const uint8_t *r = sector + i * AUTO_REC;
        if (erased(r)) continue;
        last_used = (int)i;
        s->tail_damaged = false;
        if (foreign_chst(r)) { s->foreign++; continue; }
        if (get32(r) != AUTO_MAGIC || r[4] != AUTO_VERSION || get32(r + 28) != crc32(r, 28)) { s->damaged++; s->tail_damaged = true; continue; }
        uint32_t seq = get32(r + 8);
        if (seq > s->seq) s->seq = seq;
        if (r[5] == AUTO_KIND_PROFILE) {
            auto_profile_t p = {get32(r + 12), r[16], r[17], r[18], r[19], get32(r + 20)};
            if (get32(r + 24) != auto_profile_hash(&p)) { s->damaged++; s->tail_damaged = true; continue; }
            if (p.gen > s->max_gen) s->max_gen = p.gen;
            int k = auto_store_find(s, p.fp);
            if (k >= 0) s->prof[k] = p;                       /* newer record of the same fingerprint (e.g. SOC change) */
            else if (s->n < AUTO_MAX_PROFILES) s->prof[s->n++] = p;
        } else if (r[5] == AUTO_KIND_CONTROL) {
            if (!have_ctrl || seq >= best_ctrl_seq) { have_ctrl = true; best_ctrl_seq = seq; s->auto_enabled = r[12] == 1; s->last_fp = get32(r + 16); }
        } else { s->damaged++; s->tail_damaged = true; }
    }
    if (last_used + 1 < (int)slots) s->next_free = last_used + 1;
}
int auto_store_find(const auto_store_t *s, uint32_t fp) {
    for (int i = 0; i < s->n; i++) if (s->prof[i].fp == fp) return i;
    return -1;
}
bool auto_store_may_autorun(const auto_store_t *s) { return s->auto_enabled && !s->tail_damaged; }
static auto_plan_t plan_rec(const uint8_t *sector, const uint8_t rec[AUTO_REC], const auto_store_t *s, auto_job_t *job) {
    if (s->next_free < 0) return AUTO_PLAN_FULL;
    uint32_t slot_off = (uint32_t)s->next_free * AUTO_REC;
    job->off = slot_off & ~(uint32_t)(AUTO_PAGE - 1);
    memset(job->page, 0xFF, AUTO_PAGE);
    memcpy(job->page + (slot_off - job->off), rec, AUTO_REC);
    (void)sector;
    return AUTO_PLAN_WRITE;
}
auto_plan_t auto_plan_profile(const uint8_t *sector, const auto_profile_t *p, auto_job_t *job) {
    auto_store_t s;
    auto_store_scan(sector, AUTO_SECTOR, &s);
    int k = auto_store_find(&s, p->fp);
    if (k >= 0 && !memcmp(&s.prof[k], p, sizeof *p)) return AUTO_PLAN_SAME;
    if (k < 0 && s.n >= AUTO_MAX_PROFILES) return AUTO_PLAN_TABLE_FULL;
    uint8_t rec[AUTO_REC];
    job->seq = s.seq + 1;
    auto_rec_pack_profile(p, job->seq, rec);
    return plan_rec(sector, rec, &s, job);
}
auto_plan_t auto_plan_control(const uint8_t *sector, bool en, uint32_t last_fp, auto_job_t *job) {
    auto_store_t s;
    auto_store_scan(sector, AUTO_SECTOR, &s);
    if (s.auto_enabled == en && s.last_fp == last_fp && !s.tail_damaged && s.seq) return AUTO_PLAN_SAME;
    uint8_t rec[AUTO_REC];
    job->seq = s.seq + 1;
    auto_rec_pack_control(en, last_fp, job->seq, rec);
    return plan_rec(sector, rec, &s, job);
}

/* ---------------- state names ---------------- */
const char *auto_state_str(auto_state_t s) {
    static const char *n[] = {"BOOT", "WAITING FOR PHONE", "MATCHING", "GENERATING", "VALIDATING", "SAVING", "DATA GENERATED",
                              "AUTO RUNNING", "STOPPED", "AUTO OFF", "UNKNOWN REQUEST", "AMBIGUOUS", "CAPTURE INCOMPLETE",
                              "SAVE FAILED", "STORE FULL", "SAVE INCOMPLETE"};
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}
auto_bus_t auto_state_bus(auto_state_t s) {
    switch (s) {
    case AS_WAIT_PHONE: case AS_OBSERVE: case AS_UNKNOWN: case AS_AMBIGUOUS: case AS_INCOMPLETE: return AB_LISTEN;
    case AS_GENERATED: case AS_RUNNING: return AB_RUN;
    default: return AB_OFF;   /* BOOT, GENERATING/VALIDATING/SAVING (target paused off while flash is written), STOPPED, OFF, failures */
    }
}
