/* CHIPLER AUTO controller: the state machine of handoff sec. 6, PURE (time, observations, store image and
 * save results come in as arguments; flash writes and bus switching go out as actions the device glue executes).
 *
 * First use:  BOOT -> WAITING FOR PHONE (listen) -> MATCHING -> GENERATING -> VALIDATING -> SAVING -> DATA GENERATED -> AUTO RUNNING
 * Later boot: BOOT -> (store: AUTO enabled, newest slot intact) -> listen -> same fingerprint saved: AUTO RUNNING (REUSED)
 *                                                                      -> new fingerprint, compatible: generate + save (NEW)
 *                                                                      -> unknown / ambiguous / capture loss: no replies
 * STOP:       replies off at once, CONTROL(auto=0) saved; a cold boot stays AUTO OFF until AUTO ON is pressed.
 * Listen = the target ACKs its address and records requests; EVERY read is answered 0xFF (released-bus value, the
 * documented "unsupported" behaviour). No template data is sent before a profile is selected or generated. */
#ifndef CHIPLER_AUTO_CTL_H
#define CHIPLER_AUTO_CTL_H
#include "auto_core.h"

typedef enum { AE_NONE = 0, AE_STOP, AE_AUTO_ON, AE_RELEARN, AE_CLEAR, AE_SOC } auto_event_t;
typedef enum { AA_NONE = 0, AA_SAVE_PROFILE, AA_SAVE_CONTROL, AA_CLEAR_STORE } auto_action_t;
typedef enum { AR_OK = 0, AR_FULL, AR_TABLE_FULL, AR_FLASH } auto_result_t;
#define AUTO_GENERATED_SHOW_MS 4000u

typedef struct {
    auto_state_t st, prev;
    uint32_t since_ms;
    auto_obs_t obs;
    auto_match_t m;
    auto_store_t store;
    auto_profile_t cur;          /* profile running / being saved */
    bool have_cur, reused, force_new;
    int soc;                     /* requested SOC (owner choice); a new profile takes it */
    const auto_template_t *t; int nt;
    auto_match_cfg_t cfg;
    /* pending action for the glue */
    auto_action_t act;
    bool act_enabled;            /* AA_SAVE_CONTROL value */
    char note[112];               /* last decision, for telemetry and the screen */
    uint32_t windows;            /* observation windows started (telemetry) */
    bool stop_unsaved;           /* review fix 1: STOP is not on flash yet; the save stays pending until it is */
    uint32_t stop_failures;
} auto_ctl_t;

void auto_ctl_init(auto_ctl_t *c, const auto_template_t *t, int nt, int soc);
/* boot with the scanned store image (glue scans flash) */
void auto_ctl_boot(auto_ctl_t *c, const auto_store_t *s, uint32_t now_ms);
/* feed one packed observation / dropped count (from the IRQ ring) */
void auto_ctl_observe(auto_ctl_t *c, uint32_t packed, uint32_t now_ms);
void auto_ctl_dropped(auto_ctl_t *c, uint32_t n);
/* periodic decision; returns the action the glue must execute now (also kept in c->act until acknowledged) */
auto_action_t auto_ctl_tick(auto_ctl_t *c, uint32_t now_ms);
auto_action_t auto_ctl_event(auto_ctl_t *c, auto_event_t e, int arg, uint32_t now_ms);
/* glue reports the result of the action; s = store rescanned after the write */
void auto_ctl_done(auto_ctl_t *c, auto_result_t r, const auto_store_t *s, uint32_t now_ms);
auto_bus_t auto_ctl_bus(const auto_ctl_t *c);
/* which template the bus must load in LISTEN/RUN (listening uses the runtime template's address; reads refused) */
const auto_template_t *auto_ctl_template(const auto_ctl_t *c);
#endif
