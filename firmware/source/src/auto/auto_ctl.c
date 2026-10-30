/* CHIPLER AUTO controller - see auto_ctl.h. Pure C, host-tested (tests/host/test_auto_core.c). */
#include "auto_ctl.h"
#include <stdio.h>
#include <string.h>

static void go(auto_ctl_t *c, auto_state_t s, uint32_t now) {
    if (c->st != s) { c->prev = c->st; c->st = s; c->since_ms = now; }
}
static void new_window(auto_ctl_t *c, uint32_t now) {
    auto_obs_reset(&c->obs);
    c->windows++;
    go(c, AS_WAIT_PHONE, now);
}
void auto_ctl_init(auto_ctl_t *c, const auto_template_t *t, int nt, int soc) {
    memset(c, 0, sizeof *c);
    c->t = t; c->nt = nt; c->soc = soc >= 1 && soc <= 100 ? soc : 50;
    c->cfg = AUTO_MATCH_DEFAULT;
    c->st = c->prev = AS_BOOT;
}
void auto_ctl_boot(auto_ctl_t *c, const auto_store_t *s, uint32_t now) {
    c->store = *s;
    c->have_cur = false; c->act = AA_NONE;
    if (s->tail_damaged) { snprintf(c->note, sizeof c->note, "newest saved record torn: replies stay off"); go(c, AS_UNCERTAIN, now); return; }
    if (!s->auto_enabled) { snprintf(c->note, sizeof c->note, "AUTO disabled by STOP: press AUTO ON"); go(c, AS_AUTO_OFF, now); return; }
    snprintf(c->note, sizeof c->note, "%d saved profile(s); listening", s->n);
    new_window(c, now);
}
static bool observing(const auto_ctl_t *c) { return auto_state_bus(c->st) == AB_LISTEN; }
void auto_ctl_observe(auto_ctl_t *c, uint32_t packed, uint32_t now) { if (observing(c)) auto_obs_add(&c->obs, packed, now); }
void auto_ctl_dropped(auto_ctl_t *c, uint32_t n) { if (n && observing(c)) auto_obs_drop(&c->obs, n); }
static auto_action_t want(auto_ctl_t *c, auto_action_t a) { c->act = a; return a; }

auto_action_t auto_ctl_tick(auto_ctl_t *c, uint32_t now) {
    if (c->act != AA_NONE) return c->act;   /* an action is still pending: nothing else moves */
    switch (c->st) {
    case AS_WAIT_PHONE: case AS_OBSERVE: case AS_UNKNOWN: case AS_AMBIGUOUS: case AS_INCOMPLETE: {
        auto_match_t m = auto_match(&c->obs, c->t, c->nt, &c->cfg, now);
        c->m = m;
        switch (m.kind) {
        case AM_WAITING: if (c->st != AS_INCOMPLETE) go(c, AS_WAIT_PHONE, now); return AA_NONE;   /* the loss stays visible until new requests arrive */
        case AM_NEED_MORE: go(c, AS_OBSERVE, now); return AA_NONE;
        case AM_INCOMPLETE:   /* a window with losses is not evidence: report it and start a clean one */
            snprintf(c->note, sizeof c->note, "%s", m.reason);
            go(c, AS_INCOMPLETE, now);
            { uint32_t w = c->windows; auto_obs_reset(&c->obs); c->windows = w + 1; }
            return AA_NONE;
        case AM_UNKNOWN: case AM_AMBIGUOUS:
            snprintf(c->note, sizeof c->note, "%s", m.reason);
            go(c, m.kind == AM_UNKNOWN ? AS_UNKNOWN : AS_AMBIGUOUS, now);
            /* keep listening; a fresh window after the long window so a phone reboot can be re-evaluated */
            if ((int32_t)(now - c->obs.first_ms) >= (int32_t)(c->cfg.max_window_ms * 2u)) { uint32_t w = c->windows; auto_obs_reset(&c->obs); c->windows = w + 1; }
            return AA_NONE;
        case AM_COMPATIBLE: case AM_PARTIAL: break;
        }
        uint32_t fp = auto_fingerprint(&c->obs, c->t[m.tmpl].addr7);
        int k = auto_store_find(&c->store, fp);
        if (k >= 0 && !c->force_new && auto_validate(&c->store.prof[k], c->t, c->nt)) {
            c->cur = c->store.prof[k]; c->have_cur = true; c->reused = true;
            c->soc = c->cur.soc;
            snprintf(c->note, sizeof c->note, "REUSED gen %lu fp %08lX: %s", (unsigned long)c->cur.gen, (unsigned long)fp, m.reason);
            go(c, AS_RUNNING, now);
            return AA_NONE;
        }
        go(c, AS_GENERATING, now);
        if (!auto_generate(&m, &c->obs, c->t, c->nt, c->soc, c->store.max_gen + 1u, &c->cur)) {
            snprintf(c->note, sizeof c->note, "generation refused (%s)", m.reason);
            go(c, AS_SAVE_FAILED, now);
            return AA_NONE;
        }
        go(c, AS_VALIDATING, now);
        if (!auto_validate(&c->cur, c->t, c->nt)) {
            snprintf(c->note, sizeof c->note, "validation failed");
            go(c, AS_SAVE_FAILED, now);
            return AA_NONE;
        }
        c->reused = false; c->force_new = false;
        snprintf(c->note, sizeof c->note, "NEW gen %lu fp %08lX: %s", (unsigned long)c->cur.gen, (unsigned long)c->cur.fp, m.reason);
        go(c, AS_SAVING, now);
        return want(c, AA_SAVE_PROFILE);
    }
    case AS_GENERATED:
        if (now - c->since_ms >= AUTO_GENERATED_SHOW_MS) go(c, AS_RUNNING, now);
        return AA_NONE;
    default:
        return AA_NONE;
    }
}

auto_action_t auto_ctl_event(auto_ctl_t *c, auto_event_t e, int arg, uint32_t now) {
    switch (e) {
    case AE_STOP:   /* replies off at once (state bus OFF); the persistent flag follows */
        c->have_cur = false; c->act = AA_NONE; c->stop_unsaved = true;
        snprintf(c->note, sizeof c->note, "STOP by user: AUTO disabled until AUTO ON");
        go(c, AS_STOPPED, now);
        c->act_enabled = false;
        return want(c, AA_SAVE_CONTROL);
    case AE_AUTO_ON:
        if (c->st != AS_STOPPED && c->st != AS_AUTO_OFF && c->st != AS_UNCERTAIN && c->st != AS_SAVE_FAILED && c->st != AS_STORE_FULL) return AA_NONE;
        c->act_enabled = true;
        return want(c, AA_SAVE_CONTROL);
    case AE_RELEARN:   /* keep every saved profile; ignore the fingerprint table for this window and generate again */
        if (c->st == AS_STOPPED || c->st == AS_AUTO_OFF || c->st == AS_UNCERTAIN) return AA_NONE;
        c->have_cur = false; c->force_new = true; c->act = AA_NONE;
        snprintf(c->note, sizeof c->note, "RELEARN: new window, saved profiles kept");
        new_window(c, now);
        return AA_NONE;
    case AE_CLEAR:   /* only with replies off: STOP first */
        if (c->st != AS_STOPPED && c->st != AS_AUTO_OFF && c->st != AS_UNCERTAIN && c->st != AS_STORE_FULL && c->st != AS_SAVE_FAILED) return AA_NONE;
        return want(c, AA_CLEAR_STORE);
    case AE_SOC:
        if (arg < 1 || arg > 100) return AA_NONE;
        c->soc = arg;
        if (c->have_cur && (c->st == AS_RUNNING || c->st == AS_GENERATED) && c->cur.soc != (uint8_t)arg) {
            c->cur.soc = (uint8_t)arg;
            return want(c, AA_SAVE_PROFILE);   /* glue saves it in a quiet bus gap (target paused), replies continue */
        }
        return AA_NONE;
    default: return AA_NONE;
    }
}

void auto_ctl_done(auto_ctl_t *c, auto_result_t r, const auto_store_t *s, uint32_t now) {
    auto_action_t a = c->act;
    c->act = AA_NONE;
    if (s) c->store = *s;
    if (a == AA_SAVE_PROFILE) {
        bool soc_update = c->st == AS_RUNNING || c->st == AS_GENERATED;
        if (r == AR_OK && auto_store_find(&c->store, c->cur.fp) >= 0) {
            if (!soc_update) { c->have_cur = true; go(c, AS_GENERATED, now); }   /* committed and read back: now it may answer */
            return;
        }
        if (soc_update) { snprintf(c->note, sizeof c->note, "SOC not saved (%d): runs, restart keeps old SOC", (int)r); return; }
        c->have_cur = false;
        snprintf(c->note, sizeof c->note, r == AR_FULL ? "store sector full: STOP + CLEAR" : r == AR_TABLE_FULL ? "8 profiles stored: STOP + CLEAR" : "flash write/verify failed");
        go(c, r == AR_FULL || r == AR_TABLE_FULL ? AS_STORE_FULL : AS_SAVE_FAILED, now);
    } else if (a == AA_SAVE_CONTROL) {
        if (!c->act_enabled) {   /* STOP */
            if (r == AR_OK && !c->store.auto_enabled) { c->stop_unsaved = false; return; }
            /* review fix 1: a STOP that did not reach flash would let a cold boot restart the replies. Keep the save
             * pending (the glue retries; on a full sector it erases and writes the STOP alone: STOP beats saved data). */
            c->stop_unsaved = true; c->stop_failures++; c->act = AA_SAVE_CONTROL;
            snprintf(c->note, sizeof c->note, "STOP NOT SAVED YET (%d), retrying: do not cut power", (int)r);
            return;
        }
        if (r == AR_OK && c->store.auto_enabled) { snprintf(c->note, sizeof c->note, "AUTO ON: listening"); new_window(c, now); }
        else { snprintf(c->note, sizeof c->note, "AUTO ON not saved (%d)", (int)r); go(c, AS_SAVE_FAILED, now); }
    } else if (a == AA_CLEAR_STORE) {
        c->have_cur = false;
        if (r == AR_OK) { snprintf(c->note, sizeof c->note, "saved profiles cleared"); go(c, AS_AUTO_OFF, now); }
        else { snprintf(c->note, sizeof c->note, "clear failed"); go(c, AS_SAVE_FAILED, now); }
    }
}
auto_bus_t auto_ctl_bus(const auto_ctl_t *c) {
    auto_bus_t b = auto_state_bus(c->st);
    if (b == AB_RUN && !c->have_cur) return AB_LISTEN;
    return b;
}
const auto_template_t *auto_ctl_template(const auto_ctl_t *c) {
    if (c->have_cur) return auto_tmpl_by_id(c->t, c->nt, c->cur.tmpl_id);
    for (int i = 0; i < c->nt; i++) if (c->t[i].runtime) return &c->t[i];
    return 0;
}
