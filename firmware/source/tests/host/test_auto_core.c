/* AUTO_LEARN_P1_RC1: matcher, generator, store and controller tests (host).
 * Oracles are written independently of the production code: command sets are built by hand here, store images are
 * built byte by byte, and expectations are literal values - the production functions are never their own oracle. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "auto/auto_core.h"
#include "auto/auto_ctl.h"
#include "auto/auto_templates.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { char kind; uint8_t cmd, n, b[34]; const char *t; } p12_frame_t;
#include "p12p_power_frames.inc"

static uint32_t R(uint8_t c) { return AUTO_OBS_PACK(c, AUTO_K_READ, 0, 1, 3); }
static uint32_t W(uint8_t c) { return AUTO_OBS_PACK(c, AUTO_K_WRITE, 0, 0, 4); }
static void mk(auto_template_t *t, uint8_t id, uint8_t addr, bool rt, const uint8_t *rd, int nr, const uint8_t *wr, int nw) {
    memset(t, 0, sizeof *t); t->id = id; t->name = "T"; t->addr7 = addr; t->runtime = rt;
    for (int i = 0; i < nr; i++) t->read[rd[i] >> 5] |= 1u << (rd[i] & 31);
    for (int i = 0; i < nw; i++) t->write[wr[i] >> 5] |= 1u << (wr[i] & 31);
}
static const uint8_t RA[] = {0x08, 0x09, 0x0A, 0x0D, 0x0F, 0x10}, WA[] = {0x65};
static const uint8_t RB[] = {0x08, 0x09, 0x0A, 0x0D, 0x20, 0x21};

/* fake NOR flash: program can only clear bits, erase sets 0xFF */
static uint8_t g_sec[AUTO_SECTOR];
static void flash_erase(void) { memset(g_sec, 0xFF, sizeof g_sec); }
static void flash_prog(const auto_job_t *j, unsigned cut) {   /* cut < 256: power lost after `cut` bytes */
    for (unsigned i = 0; i < AUTO_PAGE && i < cut; i++) g_sec[j->off + i] &= j->page[i];
}

static void test_obs_bits(void) {
    auto_obs_t o; auto_obs_reset(&o);
    auto_obs_add(&o, R(0x08), 100); auto_obs_add(&o, R(0x08), 110); auto_obs_add(&o, W(0x65), 120);
    auto_obs_add(&o, AUTO_OBS_PACK(0x09, AUTO_K_PTR, 0, 0, 1), 130);
    CHECK(o.n_read == 2 && o.n_write == 1 && o.n_ptr == 1);
    CHECK(o.read[0] == (1u << 8) && o.write[3] == (1u << 5));   /* hand-computed bit positions */
    CHECK(o.first_ms == 100 && o.last_ms == 130);
    CHECK(auto_bits_count(o.read) == 1);
    uint32_t p = AUTO_OBS_PACK(0xA8, AUTO_K_READ, 1, 0, 26);
    CHECK(AUTO_OBS_CMD(p) == 0xA8 && AUTO_OBS_KIND(p) == AUTO_K_READ && AUTO_OBS_SERVED(p) && !AUTO_OBS_REFUSED(p) && AUTO_OBS_LEN(p) == 26);
}

static void test_match_basic(void) {
    auto_template_t t[2]; mk(&t[0], 7, 0x0B, true, RA, 6, WA, 1);
    auto_obs_t o; auto_obs_reset(&o);
    auto_match_t m = auto_match(&o, t, 1, 0, 0);
    CHECK(m.kind == AM_WAITING);
    for (int i = 0; i < 4; i++) auto_obs_add(&o, R(RA[i]), 1000);
    m = auto_match(&o, t, 1, 0, 1500);           /* 500 ms < min window */
    CHECK(m.kind == AM_NEED_MORE);
    m = auto_match(&o, t, 1, 0, 2600);
    CHECK(m.kind == AM_COMPATIBLE && m.tmpl == 0 && m.covered == 4 && m.uncovered == 0);
    /* order and repetition do not matter: same set in another order */
    auto_obs_t o2; auto_obs_reset(&o2);
    for (int i = 3; i >= 0; i--) { auto_obs_add(&o2, R(RA[i]), 1000); auto_obs_add(&o2, R(RA[i]), 1001); }
    CHECK(auto_fingerprint(&o, 0x0B) == auto_fingerprint(&o2, 0x0B));
    CHECK(auto_fingerprint(&o, 0x0B) != auto_fingerprint(&o, 0x55));    /* address is part of the key */
    /* one extra write changes the signature */
    auto_obs_add(&o2, W(0x65), 1002);
    CHECK(auto_fingerprint(&o, 0x0B) != auto_fingerprint(&o2, 0x0B));
    /* analysis-only template never matches for runtime */
    t[0].runtime = false;
    m = auto_match(&o, t, 1, 0, 2600);
    CHECK(m.kind == AM_UNKNOWN && m.tmpl < 0);
}

static void test_match_unknown_partial_drop(void) {
    auto_template_t t[1]; mk(&t[0], 7, 0x0B, true, RA, 6, WA, 1);
    auto_obs_t o; auto_obs_reset(&o);
    const uint8_t unk[] = {0x30, 0x31, 0x32, 0x33};
    for (int i = 0; i < 4; i++) auto_obs_add(&o, R(unk[i]), 0);
    auto_obs_add(&o, R(0x08), 0);
    auto_match_t m = auto_match(&o, t, 1, 0, 3000);
    CHECK(m.kind == AM_UNKNOWN && m.first_uncovered == 0x30);
    /* 1 unknown of 6 distinct = 16 % <= 20 %: still compatible */
    auto_obs_reset(&o);
    for (int i = 0; i < 5; i++) auto_obs_add(&o, R(RA[i]), 0);
    auto_obs_add(&o, R(0x60), 0);
    m = auto_match(&o, t, 1, 0, 3000);
    CHECK(m.kind == AM_COMPATIBLE && m.uncovered == 1);
    /* 2 of 6 = 33 %: unknown */
    auto_obs_add(&o, R(0x61), 0);
    m = auto_match(&o, t, 1, 0, 3000);
    CHECK(m.kind == AM_UNKNOWN);
    /* only 2 commands for the whole window, all covered: PARTIAL after the max window, NEED_MORE before */
    auto_obs_reset(&o); auto_obs_add(&o, R(0x08), 0); auto_obs_add(&o, R(0x09), 0);
    CHECK(auto_match(&o, t, 1, 0, 5000).kind == AM_NEED_MORE);
    m = auto_match(&o, t, 1, 0, 25000);
    CHECK(m.kind == AM_PARTIAL && m.tmpl == 0);
    /* capture loss is never evidence */
    auto_obs_reset(&o); for (int i = 0; i < 6; i++) auto_obs_add(&o, R(RA[i]), 0);
    auto_obs_drop(&o, 3);
    CHECK(auto_match(&o, t, 1, 0, 3000).kind == AM_INCOMPLETE);
}

static void test_match_tie_and_shared_command(void) {
    /* two runtime templates share 0x08/0x09/0x0A/0x0D (same command numbers, different families) */
    auto_template_t t[2]; mk(&t[0], 1, 0x0B, true, RA, 6, WA, 1); mk(&t[1], 2, 0x0B, true, RB, 6, 0, 0);
    auto_obs_t o; auto_obs_reset(&o);
    for (int i = 0; i < 4; i++) auto_obs_add(&o, R(RA[i]), 0);   /* only the shared commands */
    auto_match_t m = auto_match(&o, t, 2, 0, 3000);
    CHECK(m.kind == AM_AMBIGUOUS && m.tmpl < 0);
    /* a distinguishing command decides it (deterministic: covered count) */
    auto_obs_add(&o, R(0x20), 0);
    m = auto_match(&o, t, 2, 0, 3000);
    CHECK(m.kind == AM_COMPATIBLE && m.tmpl == 1);
    auto_obs_reset(&o);
    for (int i = 0; i < 4; i++) auto_obs_add(&o, R(RA[i]), 0);
    auto_obs_add(&o, R(0x0F), 0);
    m = auto_match(&o, t, 2, 0, 3000);
    CHECK(m.kind == AM_COMPATIBLE && m.tmpl == 0);
}

static void test_real_capture(void) {
    auto_template_t t[AUTO_MAX_TEMPLATES];
    int n = auto_templates_build(t, AUTO_MAX_TEMPLATES);
    CHECK(n == 1 && t[0].id == AUTO_T_12P_POWER && t[0].addr7 == 0x0B && t[0].runtime);
    /* replay the original 12 Pro POWER capture (phone requests only) */
    auto_obs_t o; auto_obs_reset(&o);
    for (unsigned i = 0; i < P12_FRAME_COUNT; i++) {
        const p12_frame_t *f = &P12_FRAMES[i];
        auto_obs_add(&o, f->kind == 'W' ? W(f->cmd) : R(f->cmd), 1000 + i);
    }
    auto_match_t m = auto_match(&o, t, n, 0, 1000 + P12_FRAME_COUNT + 2000);
    printf("real capture: %s %s\n", auto_match_str(m.kind), m.reason);
    CHECK(m.kind == AM_COMPATIBLE && m.tmpl == 0 && m.uncovered == 0);
    /* mutation: the same requests at another address cannot use this template (address is part of the key) */
    auto_template_t moved = t[0]; moved.addr7 = 0x55;
    auto_profile_t p;
    CHECK(auto_generate(&m, &o, t, n, 37, 1, &p));
    CHECK(p.addr7 == 0x0B && p.tmpl_id == AUTO_T_12P_POWER && p.soc == 37 && p.gen == 1 && !(p.flags & AUTO_PF_PARTIAL));
    CHECK(auto_validate(&p, t, n));
    CHECK(!auto_validate(&p, &moved, 1));
    auto_profile_t bad = p; bad.soc = 0; CHECK(!auto_validate(&bad, t, n));
    bad = p; bad.tmpl_id = 99; CHECK(!auto_validate(&bad, t, n));
    bad = p; bad.fp = 0; CHECK(!auto_validate(&bad, t, n));
    /* the analysis-only path can never generate */
    auto_match_t mm = m; mm.kind = AM_UNKNOWN; CHECK(!auto_generate(&mm, &o, t, n, 50, 1, &p));
}

static void test_store(void) {
    flash_erase();
    auto_store_t s; auto_store_scan(g_sec, AUTO_SECTOR, &s);
    CHECK(s.n == 0 && s.auto_enabled && s.next_free == 0 && !s.tail_damaged && auto_store_may_autorun(&s));
    auto_profile_t a = {0x11111111u, 1, 0x0B, 40, 0, 1}, b = {0x22222222u, 1, 0x0B, 70, 0, 2};
    auto_job_t j;
    CHECK(auto_plan_profile(g_sec, &a, &j) == AUTO_PLAN_WRITE && j.off == 0 && j.seq == 1); flash_prog(&j, AUTO_PAGE);
    CHECK(auto_plan_profile(g_sec, &a, &j) == AUTO_PLAN_SAME);
    CHECK(auto_plan_profile(g_sec, &b, &j) == AUTO_PLAN_WRITE && j.seq == 2); flash_prog(&j, AUTO_PAGE);
    auto_store_scan(g_sec, AUTO_SECTOR, &s);
    CHECK(s.n == 2 && s.prof[0].soc == 40 && s.prof[1].soc == 70 && s.max_gen == 2 && s.next_free == 2);
    /* SOC update of a: newer record of the same fingerprint wins */
    a.soc = 41;
    CHECK(auto_plan_profile(g_sec, &a, &j) == AUTO_PLAN_WRITE); flash_prog(&j, AUTO_PAGE);
    auto_store_scan(g_sec, AUTO_SECTOR, &s);
    CHECK(s.n == 2 && s.prof[auto_store_find(&s, a.fp)].soc == 41);
    /* STOP control, then power lost in the middle of the next record */
    CHECK(auto_plan_control(g_sec, false, a.fp, &j) == AUTO_PLAN_WRITE); flash_prog(&j, AUTO_PAGE);
    auto_store_scan(g_sec, AUTO_SECTOR, &s);
    CHECK(!s.auto_enabled && !auto_store_may_autorun(&s));
    CHECK(auto_plan_control(g_sec, true, a.fp, &j) == AUTO_PLAN_WRITE);
    unsigned slot = (unsigned)s.next_free * AUTO_REC - j.off;
    flash_prog(&j, slot + 13);                       /* torn: 13 bytes of the AUTO ON record reached flash */
    auto_store_scan(g_sec, AUTO_SECTOR, &s);
    CHECK(s.damaged == 1 && s.tail_damaged && !s.auto_enabled && !auto_store_may_autorun(&s));
    CHECK(s.n == 2 && s.prof[auto_store_find(&s, a.fp)].soc == 41);   /* last good profiles kept */
    /* retry writes the next slot; the damaged one is old now and no longer blocks */
    CHECK(auto_plan_control(g_sec, true, a.fp, &j) == AUTO_PLAN_WRITE); flash_prog(&j, AUTO_PAGE);
    auto_store_scan(g_sec, AUTO_SECTOR, &s);
    CHECK(s.damaged == 1 && !s.tail_damaged && s.auto_enabled && auto_store_may_autorun(&s));
    /* bit flip in a stored profile = damaged, ignored */
    uint8_t save[AUTO_SECTOR]; memcpy(save, g_sec, sizeof save);
    g_sec[0 * AUTO_REC + 18] ^= 0x01;
    auto_store_scan(g_sec, AUTO_SECTOR, &s);
    CHECK(s.damaged == 2);
    memcpy(g_sec, save, sizeof save);
    /* unknown version is not accepted */
    g_sec[1 * AUTO_REC + 4] = 9;
    auto_store_scan(g_sec, AUTO_SECTOR, &s);
    CHECK(s.damaged >= 2 && auto_store_find(&s, b.fp) < 0);
    memcpy(g_sec, save, sizeof save);
    /* a foreign r8..r13 owner record is not damage */
    flash_erase();
    { uint8_t r[32]; memset(r, 0, sizeof r); r[0] = 'C'; r[1] = 'H'; r[2] = 'S'; r[3] = 'T'; r[4] = 1;
      /* crc32 computed here independently (bitwise, poly 0xEDB88320) */
      uint32_t c = 0xFFFFFFFFu; for (int i = 0; i < 28; i++) { c ^= r[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); } c = ~c;
      r[28] = (uint8_t)c; r[29] = (uint8_t)(c >> 8); r[30] = (uint8_t)(c >> 16); r[31] = (uint8_t)(c >> 24);
      memcpy(g_sec, r, 32); }
    auto_store_scan(g_sec, AUTO_SECTOR, &s);
    CHECK(s.foreign == 1 && s.damaged == 0 && s.next_free == 1 && s.auto_enabled);
    /* table full: 8 distinct profiles, the 9th is refused (old ones never overwritten) */
    flash_erase();
    for (uint32_t i = 0; i < AUTO_MAX_PROFILES; i++) { auto_profile_t p = {0x100u + i, 1, 0x0B, 50, 0, i + 1}; CHECK(auto_plan_profile(g_sec, &p, &j) == AUTO_PLAN_WRITE); flash_prog(&j, AUTO_PAGE); }
    { auto_profile_t p = {0x999u, 1, 0x0B, 50, 0, 9}; CHECK(auto_plan_profile(g_sec, &p, &j) == AUTO_PLAN_TABLE_FULL); }
    /* sector full: 128 records */
    flash_erase();
    for (int i = 0; i < 128; i++) { CHECK(auto_plan_control(g_sec, i & 1, (uint32_t)i, &j) == AUTO_PLAN_WRITE); flash_prog(&j, AUTO_PAGE); }
    CHECK(auto_plan_control(g_sec, true, 7777, &j) == AUTO_PLAN_FULL);
}

/* ---- controller scenarios with the real template and the fake flash ---- */
static auto_template_t T[AUTO_MAX_TEMPLATES]; static int NT;
static auto_result_t exec(auto_ctl_t *c, uint32_t now) {   /* the glue, in miniature */
    auto_job_t j; auto_plan_t pl = AUTO_PLAN_SAME;
    if (c->act == AA_SAVE_PROFILE) pl = auto_plan_profile(g_sec, &c->cur, &j);
    else if (c->act == AA_SAVE_CONTROL) pl = auto_plan_control(g_sec, c->act_enabled, c->cur.fp, &j);
    else if (c->act == AA_CLEAR_STORE) { flash_erase(); pl = AUTO_PLAN_SAME; }
    if (pl == AUTO_PLAN_WRITE) flash_prog(&j, AUTO_PAGE);
    auto_store_t s; auto_store_scan(g_sec, AUTO_SECTOR, &s);
    auto_result_t r = pl == AUTO_PLAN_FULL ? AR_FULL : pl == AUTO_PLAN_TABLE_FULL ? AR_TABLE_FULL : AR_OK;
    auto_ctl_done(c, r, &s, now);
    return r;
}
static void feed_phone(auto_ctl_t *c, const uint8_t *rd, int n, uint32_t t0) { for (int i = 0; i < n; i++) auto_ctl_observe(c, R(rd[i]), t0 + (uint32_t)i); }
static void run_until(auto_ctl_t *c, uint32_t *now, uint32_t until) {
    while (*now < until) { if (auto_ctl_tick(c, *now) != AA_NONE) exec(c, *now); *now += 100; }
}
static void boot(auto_ctl_t *c, uint32_t now) {
    auto_store_t s; auto_store_scan(g_sec, AUTO_SECTOR, &s);
    auto_ctl_init(c, T, NT, 50); auto_ctl_boot(c, &s, now);
}
static const uint8_t PH_A[] = {0x09, 0x88, 0x08, 0x0A, 0x0D, 0x0F, 0x10};     /* in the 12P template */
static const uint8_t PH_B[] = {0x09, 0x08, 0x0A, 0x0D, 0x11, 0x12};             /* different covered set */
static const uint8_t PH_X[] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x08};             /* mostly unknown */

static void test_ctl_scenarios(void) {
    NT = auto_templates_build(T, AUTO_MAX_TEMPLATES);
    auto_ctl_t c; uint32_t now = 0;
    flash_erase();
    /* first use: no store -> listen; nothing runs before evidence */
    boot(&c, now);
    CHECK(c.st == AS_WAIT_PHONE && auto_ctl_bus(&c) == AB_LISTEN);
    feed_phone(&c, PH_A, 7, 100);
    run_until(&c, &now, 1000);
    CHECK(c.st == AS_OBSERVE && auto_ctl_bus(&c) == AB_LISTEN);   /* min window not over */
    run_until(&c, &now, 2000);
    CHECK(c.st == AS_GENERATED && !c.reused && auto_ctl_bus(&c) == AB_RUN);
    uint32_t fpA = c.cur.fp;
    run_until(&c, &now, 7000);
    CHECK(c.st == AS_RUNNING);
    /* SOC change while running is saved for this profile */
    CHECK(auto_ctl_event(&c, AE_SOC, 63, now) == AA_SAVE_PROFILE); exec(&c, now);
    CHECK(c.st == AS_RUNNING);
    /* cold boot, same phone: REUSED, runs without any selection, SOC restored */
    now = 0; boot(&c, now);
    feed_phone(&c, PH_A, 7, 100);
    run_until(&c, &now, 2000);
    CHECK(c.st == AS_RUNNING && c.reused && c.cur.fp == fpA && c.soc == 63);
    /* A -> B: STOP, AUTO ON (explicit), new phone B; A stays stored */
    CHECK(auto_ctl_event(&c, AE_STOP, 0, now) == AA_SAVE_CONTROL); exec(&c, now);
    CHECK(c.st == AS_STOPPED && auto_ctl_bus(&c) == AB_OFF);
    /* STOP then cold boot: stays off */
    now = 0; boot(&c, now);
    CHECK(c.st == AS_AUTO_OFF && auto_ctl_bus(&c) == AB_OFF);
    feed_phone(&c, PH_A, 7, 100); run_until(&c, &now, 3000);
    CHECK(c.st == AS_AUTO_OFF && !c.obs.any);
    CHECK(auto_ctl_event(&c, AE_AUTO_ON, 0, now) == AA_SAVE_CONTROL); exec(&c, now);
    CHECK(c.st == AS_WAIT_PHONE);
    uint32_t t0 = now;
    feed_phone(&c, PH_B, 6, t0);
    run_until(&c, &now, t0 + 2000);
    CHECK(c.st == AS_GENERATED && !c.reused && c.cur.fp != fpA);
    uint32_t fpB = c.cur.fp;
    CHECK(c.store.n == 2 && c.cur.soc == 50);   /* B takes the SOC requested now (boot default 50: no profile ran since the boot) */
    /* B -> A without clearing: A is REUSED with A's own SOC, B's state does not leak */
    now = 0; boot(&c, now);
    feed_phone(&c, PH_A, 7, 100);
    run_until(&c, &now, 2000);
    CHECK(c.st == AS_RUNNING && c.reused && c.cur.fp == fpA && c.cur.fp != fpB && c.soc == 63);
    /* unknown phone: never runs, last success does not leak */
    now = 0; boot(&c, now);
    feed_phone(&c, PH_X, 6, 100);
    run_until(&c, &now, 3000);
    CHECK(c.st == AS_UNKNOWN && auto_ctl_bus(&c) == AB_LISTEN && !c.have_cur);
    /* relearn keeps the table and generates a new generation for the same fingerprint */
    now = 0; boot(&c, now);
    feed_phone(&c, PH_A, 7, 100); run_until(&c, &now, 2000);
    CHECK(c.st == AS_RUNNING && c.reused);
    uint32_t g0 = c.cur.gen;
    auto_ctl_event(&c, AE_RELEARN, 0, now);
    CHECK(c.st == AS_WAIT_PHONE && !c.have_cur);
    t0 = now; feed_phone(&c, PH_A, 7, t0); run_until(&c, &now, t0 + 2000);
    CHECK(c.st == AS_GENERATED && !c.reused && c.cur.gen > g0 && c.store.n == 2);
    /* clear needs replies off */
    CHECK(auto_ctl_event(&c, AE_CLEAR, 0, now) == AA_NONE);
    auto_ctl_event(&c, AE_STOP, 0, now); exec(&c, now);
    CHECK(auto_ctl_event(&c, AE_CLEAR, 0, now) == AA_CLEAR_STORE); exec(&c, now);
    CHECK(c.st == AS_AUTO_OFF && c.store.n == 0);
    /* capture loss: no generation from that window */
    now = 0; boot(&c, now);
    feed_phone(&c, PH_A, 7, 100); auto_ctl_dropped(&c, 2);
    run_until(&c, &now, 300);
    CHECK(c.st == AS_INCOMPLETE && !c.have_cur);
    /* torn newest record at boot: nothing runs */
    flash_erase();
    { auto_job_t j; CHECK(auto_plan_control(g_sec, false, 0, &j) == AUTO_PLAN_WRITE); flash_prog(&j, 9); }
    now = 0; boot(&c, now);
    CHECK(c.st == AS_UNCERTAIN && auto_ctl_bus(&c) == AB_OFF);
    /* save failure: DATA GENERATED never shown, no replies from the unsaved profile */
    flash_erase();
    for (uint32_t i = 0; i < AUTO_MAX_PROFILES; i++) { auto_job_t j; auto_profile_t p = {0x100u + i, 1, 0x0B, 50, 0, i + 1}; auto_plan_profile(g_sec, &p, &j); flash_prog(&j, AUTO_PAGE); }
    now = 0; boot(&c, now);
    feed_phone(&c, PH_A, 7, 100);
    run_until(&c, &now, 2000);
    CHECK(c.st == AS_STORE_FULL && auto_ctl_bus(&c) == AB_OFF && !c.have_cur);
}

static void test_stop_must_persist(void) {
    /* review fix 1: STOP with a full sector stays pending (no silent loss); after the glue's erase+STOP the cold boot is OFF */
    NT = auto_templates_build(T, AUTO_MAX_TEMPLATES);
    auto_ctl_t c; uint32_t now = 0;
    flash_erase();
    for (int i = 0; i < 128; i++) { auto_job_t j; auto_plan_control(g_sec, true, (uint32_t)i, &j); flash_prog(&j, AUTO_PAGE); }
    boot(&c, now);
    CHECK(c.st == AS_WAIT_PHONE);
    CHECK(auto_ctl_event(&c, AE_STOP, 0, now) == AA_SAVE_CONTROL);
    CHECK(exec(&c, now) == AR_FULL);
    CHECK(c.st == AS_STOPPED && c.stop_unsaved && c.act == AA_SAVE_CONTROL && auto_ctl_bus(&c) == AB_OFF);
    /* a cold boot NOW would still find auto enabled: this is exactly why the save stays pending */
    { auto_store_t s; auto_store_scan(g_sec, AUTO_SECTOR, &s); CHECK(s.auto_enabled); }
    /* glue escalation: erase + STOP alone */
    flash_erase();
    { auto_job_t j; CHECK(auto_plan_control(g_sec, false, 0, &j) == AUTO_PLAN_WRITE); flash_prog(&j, AUTO_PAGE); }
    { auto_store_t s; auto_store_scan(g_sec, AUTO_SECTOR, &s); auto_ctl_done(&c, AR_OK, &s, now); }
    CHECK(!c.stop_unsaved && c.act == AA_NONE && c.st == AS_STOPPED);
    boot(&c, now);
    CHECK(c.st == AS_AUTO_OFF && auto_ctl_bus(&c) == AB_OFF);
    /* flash error on STOP: pending, retried */
    flash_erase(); boot(&c, now);
    auto_ctl_event(&c, AE_STOP, 0, now);
    { auto_store_t s; auto_store_scan(g_sec, AUTO_SECTOR, &s); auto_ctl_done(&c, AR_FLASH, &s, now); }
    CHECK(c.stop_unsaved && c.act == AA_SAVE_CONTROL && c.stop_failures == 1);
}

static void test_names(void) {
    CHECK(!strcmp(auto_state_str(AS_GENERATED), "DATA GENERATED"));
    CHECK(!strcmp(auto_state_str(AS_RUNNING), "AUTO RUNNING"));
    CHECK(auto_state_bus(AS_SAVING) == AB_OFF && auto_state_bus(AS_WAIT_PHONE) == AB_LISTEN && auto_state_bus(AS_RUNNING) == AB_RUN);
}

int main(void) {
    test_obs_bits(); test_match_basic(); test_match_unknown_partial_drop(); test_match_tie_and_shared_command();
    test_real_capture(); test_store(); test_ctl_scenarios(); test_stop_must_persist(); test_names();
    printf("auto_core_tests: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
