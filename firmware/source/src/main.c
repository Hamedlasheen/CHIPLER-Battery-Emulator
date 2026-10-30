/*
 * CHIPLER Smart Battery Emulator - firmware v0.1.0-dev
 * Target: RP2350A (Waveshare RP2350-USB-A assumed - PROVISIONAL pin map, see board_config.h)
 *
 * P1_ILI9341_TOUCH (revision P1_r1): ILI9341 2.8" 320x240 landscape + resistive touch (controller U2
 * UNCONFIRMED). Imported from beta1 (declared upstream 5368a2fb) and extended; see CLAUDE_CHANGELOG.
 *
 * Core 0: optional key, navigation, safety state machine, power monitor, battery-bus I2C target
 *         (IRQ at highest priority), serial log, flash settings. Executes UI commands.
 * Core 1: UI rendering into a 320x240 RGB565 framebuffer (153600 B, single buffer) + SPI DMA to the LCD,
 *         touch polling (shared SPI0 through spi_bus), calibration procedure. Never touches the I2C
 *         target or protocol state: it only posts commands (ui_cmd_queue) that core 0 validates.
 * The UI can never block the I2C target: it runs on the other core and only reads a copy
 * of the model.
 *
 * NOTHING HERE IS HARDWARE-VERIFIED YET. Build success (gate S1) != hardware pass.
 */
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/watchdog.h"
#include "hardware/regs/sysinfo.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"
#include "board_config.h"
#include "log/log.h"
#include "hal/lcd.h"
#include "hal/power_monitor.h"
#include "batt/capture_replay.h"
#include "hal/board_io.h"
#include "hal/battery_bus.h"
#include "gfx/gfx.h"
#include "ui/ui_ls.h"
#include "ui/ui_nav.h"
#include "ui/ui_cmd_queue.h"
#include "ui/splash_gate.h"
#include "hal/spi_bus.h"
#include "diag/core1_diag.h"
#include <stdio.h>
#include <stdatomic.h>
#include "input/touch_core.h"
#include "input/touch_hw.h"
#include "settings/settings_store.h"
#if CHIPLER_OWNER_PHONE_TEST
#include "settings/owner_state.h"
#include "batt/owner_words.h"
#endif
#include "input/buttons.h"
#include "safety/safety.h"
#include "batt/profile.h"
#include "batt/soc_ctrl.h"
#include "power/telem_fresh.h"
#include "charge/charge_diag.h"
#include "ctrl/ctrl_cycle.h"
#if CHIPLER_AUTO_LEARN
#include "auto/auto_core.h"
#include "auto/auto_ctl.h"
#include "auto/auto_templates.h"
#include "auto/auto_flash.h"
#if !CHIPLER_OWNER_PHONE_TEST || !CHIPLER_12P_REFERENCE
#error "AUTO_LEARN needs the owner phone-test build with the 12 Pro reference (the only runtime template)"
#endif
static auto_ctl_t s_auto;   /* AUTO controller (glue below apply_bus_policy); declared early for bus_initial_words */
#endif

#define FW_VERSION CHIPLER_REVISION      /* one revision string everywhere (r9/r10 printed fw=P1_r1 in status) */
#ifndef CHIPLER_SIM_TELEMETRY
#define CHIPLER_SIM_TELEMETRY 0     /* 1 = synthetic V/A/W, always labelled SIMULATED, never armable */
#endif

static uint16_t s_fb[GFX_W * GFX_H];   /* RAM budget: 153600 B (single buffer, see docs/CLAUDE_RAM_BUDGET) */
static ui_model_t s_model;           /* written by core 0 */
static critical_section_t s_lock;
static uicq_t s_uiq;                 /* core 1 -> core 0 touch commands */
static touch_cal_t s_cal_to_ui;      /* core 0 -> core 1 (loaded / accepted calibration), under s_lock */
static uint32_t s_cal_to_ui_seq;
static touch_cal_t s_cal_from_ui;    /* core 1 -> core 0 (new calibration result), under s_lock */

#define CAL_POINTS 5
#define CAL_TIMEOUT_MS 45000u
#define FRAME_MIN_MS 150u            /* >= 150 ms between full-frame pushes; touch is polled in between */
#define POLL_MS 10u

/* ---------------- core 1: UI + touch ---------------- */
static splash_gate_t s_splash_gate;       /* core 1 -> core 0: the SCR_STARTUP frame was pushed to the panel successfully (atomic_bool, release/acquire) */
static void core1_main(void) {
    static ui_model_t local, drawn;
    static ls_touch_info_t ti;
    static touch_core_t tc;
    multicore_lockout_victim_init();     /* lets core 0 write flash settings while the bus is stopped */
    c1d_stage(C1S_LCD_INIT);
    lcd_init();
    c1d_stage(C1S_GFX_INIT);
    gfx_init(s_fb);
    c1d_stage(C1S_TOUCH_INIT);
    touch_hw_init();
    c1d_stage(C1S_BOOT_DELAY);
    lcd_fill(0x0000);                    /* beta 1: no colour-bar test at boot (colour/inversion already verified on the bench); the splash screen is the first thing drawn */
    touch_core_init(&tc, LCD_WIDTH, LCD_HEIGHT);
    ti.hw_enabled = TOUCH_BACKEND != 0;
    uint32_t last_poll = 0, last_draw = 0, last_copy = 0, cal_seq_seen = 0, cal_deadline = 0;
    uint8_t cur_rot = 0xFF;
    uint32_t last_full = 0; screen_t last_screen = SCR_STARTUP;
    screen_t shown_screen = SCR_STARTUP;   /* DS-TOUCH-12: a held contact must not release into a new screen */
    bool have_local = false, cal_active = false;
    uint32_t presses_seen = 0, last_offer = 0;
    static tch_pt_t raw_pts[CAL_POINTS], tgt[CAL_POINTS];
    touch_cal_targets(CAL_POINTS, LCD_WIDTH, LCD_HEIGHT, tgt);
    static ls_layout_t lay;
    while (true) {
        uint32_t loop_t0 = time_us_32();
        c1d_stage(C1S_LOOP_TOP);
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (!have_local || (uint32_t)(now - last_copy) >= 40) {
            c1d_stage(C1S_LOCK_WAIT);
            critical_section_enter_blocking(&s_lock);
            c1d_stage(C1S_MODEL_COPY);
            local = s_model;
            touch_cal_t cal = s_cal_to_ui; uint32_t seq = s_cal_to_ui_seq;
            critical_section_exit(&s_lock);
            last_copy = now; have_local = true;
            if (local.rot180 != cur_rot) {
                cur_rot = local.rot180;
                lcd_set_rotation(cur_rot != 0);
                lcd_invalidate();
                touch_core_set_rotation(&tc, cur_rot);
            }
            if (seq != cal_seq_seen) { cal_seq_seen = seq; touch_core_set_cal(&tc, &cal); }
            ui_ls_layout(&local, &lay);
            if (local.screen != shown_screen) { shown_screen = local.screen; touch_core_cancel(&tc); }
        }
        /* ---- touch poll ---- */
        if ((uint32_t)(now - last_poll) >= POLL_MS) {
            last_poll = now;
            touch_raw_t raw;
            c1d_stage(C1S_TOUCH_POLL);
            uint32_t poll_t0 = time_us_32();
            bool ok = touch_hw_read(&raw);
            c1d_note_poll(time_us_32() - poll_t0, ok);
            c1d_stage(C1S_UI_LOGIC);
            tch_event_t ev = touch_core_feed(&tc, now, ok ? &raw : 0);
            ti.cal_valid = tc.cal.valid; ti.ever_pen = tc.ever_pen; ti.down = tc.down;
            ti.irq = TOUCH_BACKEND ? touch_hw_irq_active() : false;
            ti.rx = tc.last_rx; ti.ry = tc.last_ry; ti.rz = tc.last_rz; ti.sx = tc.last_sx; ti.sy = tc.last_sy;
            ti.polls = tc.polls; ti.presses = tc.presses; ti.taps = tc.taps; ti.rejected = tc.rejected_raw;
            ti.spi_violations = spi_bus_violations();
            if (local.screen == SCR_CAL) {
                if (!cal_active) {
                    cal_active = true; ti.cal_step = 0; ti.cal_total = CAL_POINTS; ti.cal_fail = false;
                    cal_deadline = now + CAL_TIMEOUT_MS;
                }
                ti.cal_tx = tgt[ti.cal_step].x; ti.cal_ty = tgt[ti.cal_step].y;
                ti.cal_left_ms = (int32_t)(cal_deadline - now) > 0 ? cal_deadline - now : 0;
                ls_region_t chit;
                if (ev.kind == TCH_EV_TAP && tc.cal.valid && ui_ls_hit(&lay, ev.x, ev.y, &chit) && chit.cmd == LS_CMD_BACK) {
                    uicq_push(&s_uiq, (uic_t){UIC_BACK, SCR_CAL, 0});   /* cancel with the previous calibration */
                    cal_active = false;
                } else if (ev.kind == TCH_EV_TAP) {
                    raw_pts[ti.cal_step] = (tch_pt_t){(float)tc.press_rx, (float)tc.press_ry};
                    cal_deadline = now + CAL_TIMEOUT_MS;
                    if (++ti.cal_step >= CAL_POINTS) {
                        touch_cal_t c; float worst;
                        if (touch_cal_solve(raw_pts, tgt, CAL_POINTS, LCD_WIDTH, LCD_HEIGHT, cur_rot, &c, &worst)) {
                            touch_core_set_cal(&tc, &c);
                            critical_section_enter_blocking(&s_lock);
                            s_cal_from_ui = c;
                            critical_section_exit(&s_lock);
                            uicq_push(&s_uiq, (uic_t){UIC_CAL_DONE, SCR_CAL, 0});
                            cal_active = false;
                        } else { ti.cal_fail = true; ti.cal_step = 0; }
                    }
                } else if ((int32_t)(now - cal_deadline) >= 0) {
                    uicq_push(&s_uiq, (uic_t){UIC_BACK, SCR_CAL, 0});
                    cal_active = false;
                }
            } else {
                cal_active = false;
                /* FAILSAFE: a stationary hold of 1.2 s anywhere = STOP REPLIES. It needs no calibration and no
                 * hit-test, so the safe action stays reachable if the calibration is missing or has drifted. */
                if (ev.kind == TCH_EV_LONG && local.screen != SCR_STARTUP)
                    uicq_request_stop(&s_uiq);       /* priority flag: never dropped by a full queue or a stale-screen filter */
                if (ev.kind == TCH_EV_TAP) {
                    ls_region_t hit;
                    if (ui_ls_hit(&lay, ev.x, ev.y, &hit) && tc.cal.valid) {
                        uic_kind_t k = hit.cmd == LS_CMD_SELECT ? UIC_SELECT : hit.cmd == LS_CMD_BACK ? UIC_BACK :
                                       hit.cmd == LS_CMD_STOP ? UIC_STOP : hit.cmd == LS_CMD_UP ? UIC_PAGE_UP :
                                       hit.cmd == LS_CMD_DOWN ? UIC_PAGE_DOWN : UIC_NONE;
                        if (k == UIC_STOP) uicq_request_stop(&s_uiq);
                        else if (k != UIC_NONE) uicq_push(&s_uiq, (uic_t){k, (uint8_t)local.screen, hit.idx});
                    }
                }
                /* Touch seen while UNCALIBRATED: (re)offer calibration on any new contact, at most every 3 s and never over
                 * a fault/confirm/startup screen. This removes the first-calibration dead end (DS-UI-11 C-05): a timed-out
                 * or rejected calibration cannot leave the user on a screen where every tap is gated. Raw guesses never navigate. */
                if (!tc.cal.valid && tc.presses != presses_seen && local.screen != SCR_STARTUP && local.screen != SCR_FAULT &&
                    local.screen != SCR_CONFIRM && local.screen != SCR_CAL && (uint32_t)(now - last_offer) >= 3000u) {
                    presses_seen = tc.presses; last_offer = now;
                    uicq_push(&s_uiq, (uic_t){UIC_AUTOCAL, (uint8_t)local.screen, 0});
                } else if (tc.cal.valid) presses_seen = tc.presses;
            }
        }
        /* ---- draw ---- */
        if ((uint32_t)(now - last_draw) >= FRAME_MIN_MS) {
            ui_model_t cmp = local; cmp.now_ms = 0;
            bool changed = memcmp(&cmp, &drawn, sizeof cmp) != 0 || local.screen == SCR_CAL || local.screen == SCR_TOUCHDIAG;
            if (changed || (uint32_t)(now - last_draw) >= 500) {
                last_draw = now;
                drawn = cmp;
                c1d_stage(C1S_RENDER);
                uint32_t rt0 = time_us_32();
                ui_ls_render(&local, &ti);
                c1d_note_render(time_us_32() - rt0);
                bool full = (uint32_t)(now - last_full) >= 1000u || local.screen != last_screen;
                if (full) last_full = now;
                last_screen = local.screen;
                ti.rows_pushed = lcd_push_frame_dirty(s_fb, full);
                splash_gate_on_push(&s_splash_gate, local.screen == SCR_STARTUP, ti.rows_pushed, lcd_last_push_ok());
            }
        }
        c1d_stage(C1S_SLEEP);
        sleep_us(500);
        c1d_note_loop(time_us_32() - loop_t0);
    }
}

/* ---------------- core 0: control ---------------- */
static safety_t s_safety;
static ui_nav_t s_nav;
static buttons_t s_btn;
static int s_prof;
static char s_toast[32]; static uint32_t s_toast_until;
static uint8_t s_spark[SPARK_N];
static uint32_t s_last_txn_ms, s_sec_txns, s_prev_overflows;
static soc_ctrl_t s_soc;
static telem_fresh_t s_fresh;
static float s_lg_v, s_lg_a, s_lg_w;     /* last good measured sample (STALE view only) */
static chg_checks_t s_chk;
static bool s_chg_ok;
static ui_settings_t s_set;          /* persistent UI settings (rotation, touch calibration) */
static bool noted_settings_save(const ui_settings_t *s, bool bus_running);   /* times the flash write for the core-1 diagnostics */
static bool s_set_saved;

/* CHARGE TEST readiness. Sink and recipe do not exist in this firmware -> never ready. */
static void update_charge(telem_quality_t q) {
    const batt_profile_t *p = profile_get(s_prof);
    chg_eval_checks(&s_chk, profile_armable(p), CHG_SINK_FITTED != 0 && false, q,
                    CHG_INPUT_SENSOR_FITTED != 0, false, false);
    s_chg_ok = chg_start_allowed(&s_chk);
}

static bool publish_to_engine(void *ctx, uint8_t base, const uint8_t *data, uint8_t len) {
    (void)ctx;
#if CHIPLER_OWNER_PHONE_TEST
    /* F2: soc_apply scales 0x0F on the T06 full charge (2913). Rescale it on the live target's own 0x10 inside the same
     * atomic publish, so 0x0D and 0x0F never disagree, not even for one control tick. */
    uint16_t fcc;
    if (base == SOC_PUBLISH_WORDS && data && len <= 12 && len % 3 == 0 && battery_bus_word(0x10, &fcc) && fcc) {
        uint8_t img[12];
        memcpy(img, data, len);
        (void)owner_rescale_words(img, len, fcc);
        return battery_bus_publish(base, img, len);
    }
#endif
    return battery_bus_publish(base,data,len);
}

static void log_soc(soc_result_t r) {
    LOG("SOC", "requested=%d%% applied=%d result=%s (phone effect NOT VERIFIED; APPLIED_PHONE_DATA = response registers only)", s_soc.requested,
        s_soc.applied, soc_result_str(r));
}

static void toast(const char *t, uint32_t now) {
    strncpy(s_toast, t, sizeof s_toast - 1); s_toast[sizeof s_toast - 1] = 0;
    s_toast_until = now + 2500;
}

#if CHIPLER_OWNER_PHONE_TEST
/* F5 backoff: the first recovery after a calm period is immediate (next tick), repeats double 10 ms -> 100 ms.
 * There is deliberately no give-up limit: a latched port is exactly the "phone stops at the logo" failure. */
#define OWNER_RECOVER_MIN_MS  10u
#define OWNER_RECOVER_MAX_MS  100u    /* r4: the phone reboots if the gauge is silent too long; keep gaps short */
#define OWNER_RECOVER_CALM_MS 2000u   /* bus running fault-free this long -> back to immediate recovery */
static uint32_t s_bus_recoveries, s_recover_at, s_recover_last, s_recover_backoff = OWNER_RECOVER_MIN_MS;
static void owner_recover_reset(void) { s_bus_recoveries = 0; s_recover_at = 0; s_recover_backoff = OWNER_RECOVER_MIN_MS; }
/* review 105 R1: last INA226 bus voltage of a fresh measured sample, kept for the words published before activation */
static float s_ow_v;
static bool s_ow_v_ok;
static bool ob_word(void *ctx, uint8_t cmd, uint16_t *v) { (void)ctx; return battery_bus_word(cmd, v); }
static bool ob_publish(void *ctx, uint8_t base, const uint8_t *d, uint8_t len) { (void)ctx; return battery_bus_publish(base, d, len); }
static const owner_bus_t s_owner_bus = {ob_word, ob_publish, 0};
#endif

/* Runs between prepare and activate (battery_bus_start_seq): the target cannot be addressed yet. */
static bool bus_initial_words(void *ctx) {
    const batt_profile_t *p = ctx;
#if CHIPLER_AUTO_LEARN
    battery_bus_set_listen(auto_ctl_bus(&s_auto) != AB_RUN);   /* before the first ACK: LISTEN never serves template bytes */
#endif
#if CHIPLER_OWNER_PHONE_TEST
    /* review 105 R1: a DATA TEST target starts with the owner SOC (restored, chosen, or after a recovery restart) and the
     * measured voltage already in its map; the recorded 2 % / 3455 mV are never the first answer. */
    if (p->status == PROF_PHONE_DATA_TEST) return owner_initial_words(&s_owner_bus, s_soc.requested, s_ow_v_ok, s_ow_v);
#endif
    return !p->soc_words || s_soc.applied < 0 || soc_apply(&s_soc, p, s_soc.requested, publish_to_engine, battery_bus_engine()) == SOC_RES_APPLIED_PHONE_DATA;
}

#if CHIPLER_AUTO_LEARN
/* ================= AUTO_LEARN_P1_RC1 glue =================
 * The controller (auto/auto_ctl.c) decides; this glue only feeds it the requests the SMBus target recorded, switches the
 * target between OFF / LISTEN (reads answered 0xFF) / RUN (template map), executes flash writes with the target off
 * (or paused in a quiet gap for a SOC update), and prints telemetry. There is no phone-model selection anywhere. */
static auto_ctl_t s_auto;
static auto_template_t s_tmpl[AUTO_MAX_TEMPLATES];
static int s_ntmpl, s_auto_prof = -1;        /* profile entry that loads the 12P POWER map (P12P-C1) */
static uint32_t s_auto_seq, s_auto_boot_id, s_auto_obs_drops_seen, s_auto_last_state_ms;
static auto_state_t s_auto_shown = AS_BOOT;
static bool s_auto_soc_dirty; static uint32_t s_auto_soc_due, s_auto_retry_at;
static uint8_t crc8_line(const char *p) {
    uint8_t c = 0;
    while (*p) { c ^= (uint8_t)*p++; for (int k = 0; k < 8; k++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1); }
    return c;
}
/* telemetry frame: "@A1 <type> n=<seq> b=<boot> k=v ...*CC" (CC = CRC-8 poly 0x07 over the text between '@' and '*') */
static void auto_emit(const char *type, const char *body) {
    char f[136];
    snprintf(f, sizeof f, "A1 %s n=%lu b=%08lX %s", type, (unsigned long)++s_auto_seq, (unsigned long)s_auto_boot_id, body);
    LOG("AUTO", "@%s*%02X", f, crc8_line(f));
}
static void auto_emit_status(void) {
    char b[120];
    snprintf(b, sizeof b, "st=%u bus=%u rd=%lu wr=%lu dist=%u cov=%u unc=%u odrop=%lu ldrop=%lu prof=%u soc=%d", (unsigned)s_auto.st,
        (unsigned)auto_ctl_bus(&s_auto), (unsigned long)s_auto.obs.n_read, (unsigned long)s_auto.obs.n_write, s_auto.m.distinct,
        s_auto.m.covered, s_auto.m.uncovered, (unsigned long)battery_bus_obs_dropped(), (unsigned long)log_dropped(),
        (unsigned)s_auto.store.n, s_soc.requested);
    auto_emit("S", b);
    if (s_auto.have_cur) {
        snprintf(b, sizeof b, "fp=%08lX gen=%lu hash=%08lX tmpl=%u addr=%02X soc=%u reused=%d partial=%d", (unsigned long)s_auto.cur.fp,
            (unsigned long)s_auto.cur.gen, (unsigned long)auto_profile_hash(&s_auto.cur), s_auto.cur.tmpl_id, s_auto.cur.addr7,
            s_auto.cur.soc, s_auto.reused, (s_auto.cur.flags & AUTO_PF_PARTIAL) != 0);
        auto_emit("P", b);
    }
}
static void auto_boot(void) {
    s_ntmpl = auto_templates_build(s_tmpl, AUTO_MAX_TEMPLATES);
    s_auto_prof = profile_find("P12P-C1");
    auto_store_t st;
    bool region = auto_flash_scan(&st);
    s_auto_boot_id = (st.seq << 16) ^ (time_us_32() & 0xFFFFu) ^ 0xA5A50000u;
    auto_ctl_init(&s_auto, s_tmpl, s_ntmpl, s_soc.requested);
    if (!region || s_auto_prof < 0 || s_ntmpl < 1) {
        auto_store_t none; memset(&none, 0, sizeof none); none.auto_enabled = false; none.next_free = -1;
        auto_ctl_boot(&s_auto, &none, 0);
        snprintf(s_auto.note, sizeof s_auto.note, "%s", !region ? "flash region overlaps program: AUTO OFF" : "no runtime template: AUTO OFF");
    } else auto_ctl_boot(&s_auto, &st, 0);
    LOG("AUTO", "boot: %d template(s), store n=%d enabled=%d damaged=%u tail_damaged=%d foreign=%u seq=%lu -> %s (%s)", s_ntmpl, st.n,
        st.auto_enabled, st.damaged, st.tail_damaged, st.foreign, (unsigned long)st.seq, auto_state_str(s_auto.st), s_auto.note);
}
typedef struct { auto_result_t r; bool ran; } auto_pause_job_t;
static void auto_pause_save(void *ctx) { auto_pause_job_t *j = ctx; j->r = auto_flash_save_profile(&s_auto.cur); j->ran = true; }
static void auto_service(uint32_t now) {
    /* 1. requests recorded by the target since the last tick (bounded per tick) */
    uint32_t o;
    for (int k = 0; k < 64 && battery_bus_pop_obs(&o); k++) {
        auto_ctl_observe(&s_auto, o, now);
        if (k < 6 && auto_state_bus(s_auto.st) == AB_LISTEN) {   /* transaction detail, bounded; a full log ring drops lines (counted) */
            char b[72];
            snprintf(b, sizeof b, "cmd=%02X k=%u srv=%d ref=%d len=%u src=TARGET_CALLBACK", AUTO_OBS_CMD(o), AUTO_OBS_KIND(o),
                AUTO_OBS_SERVED(o), AUTO_OBS_REFUSED(o), AUTO_OBS_LEN(o));
            auto_emit("T", b);
        }
    }
    uint32_t d = battery_bus_obs_dropped();
    if (d != s_auto_obs_drops_seen) { if (d > s_auto_obs_drops_seen) auto_ctl_dropped(&s_auto, d - s_auto_obs_drops_seen); s_auto_obs_drops_seen = d; }
    /* 2. decide */
    auto_action_t a = auto_ctl_tick(&s_auto, now);
    /* 3. execute a pending action */
    if (a == AA_SAVE_PROFILE && (s_auto.st == AS_RUNNING || s_auto.st == AS_GENERATED)) {
        /* SOC update of the running profile: debounced 1.5 s, written in a quiet gap with the target paused (~1 ms) */
        if (!s_auto_soc_dirty) { s_auto_soc_dirty = true; s_auto_soc_due = now + 1500u; }
        if ((int32_t)(now - s_auto_soc_due) >= 0 && (!battery_bus_running() || battery_bus_idle_us(time_us_32()) >= 20000u)) {
            auto_pause_job_t j = {AR_FLASH, false};
            bool rearmed = true;
            if (battery_bus_running()) rearmed = battery_bus_pause(auto_pause_save, &j); else auto_pause_save(&j);
            if (j.ran) {
                auto_store_t st; auto_flash_scan(&st);
                auto_ctl_done(&s_auto, j.r, &st, now);
                s_auto_soc_dirty = false;
                LOG("AUTO", "SOC %u%% saved for fp %08lX: %s%s", s_auto.cur.soc, (unsigned long)s_auto.cur.fp, j.r == AR_OK ? "OK" : "FAILED",
                    rearmed ? "" : " (target did not re-arm; bus policy restarts it)");
            }
        }
    } else if (a != AA_NONE && !battery_bus_running() && (int32_t)(now - s_auto_retry_at) >= 0) {   /* new profile / control / clear: target off */
        auto_result_t r = a == AA_SAVE_PROFILE ? auto_flash_save_profile(&s_auto.cur)
                        : a == AA_SAVE_CONTROL ? auto_flash_save_control(s_auto.act_enabled, s_auto.cur.fp)
                        : auto_flash_clear();
        if (a == AA_SAVE_CONTROL && !s_auto.act_enabled && r == AR_FULL) {
            /* review fix 1: the STOP must reach flash. A full sector is erased and the STOP is written alone; the saved
             * profiles are lost (logged) - a cold boot must never restart replies the user stopped. */
            LOG("AUTO", "sector full while saving STOP: erasing the store and writing STOP alone (saved profiles are lost)");
            r = auto_flash_clear();
            if (r == AR_OK) r = auto_flash_save_control(false, 0);
        }
        s_auto_retry_at = r == AR_OK ? now : now + 2000u;   /* a failed write is retried every 2 s, never in a tight loop */
        auto_store_t st; auto_flash_scan(&st);
        LOG("AUTO", "%s -> %s (store n=%d seq=%lu)", a == AA_SAVE_PROFILE ? "save profile" : a == AA_SAVE_CONTROL ? "save control" : "clear store",
            r == AR_OK ? "COMMITTED" : r == AR_FULL ? "SECTOR FULL" : r == AR_TABLE_FULL ? "TABLE FULL" : "FLASH ERROR", st.n, (unsigned long)st.seq);
        auto_ctl_done(&s_auto, r, &st, now);
        if (r == AR_OK && a == AA_SAVE_PROFILE && s_auto.st == AS_GENERATED) {
            char b[96];
            snprintf(b, sizeof b, "saved=1 fp=%08lX gen=%lu hash=%08lX", (unsigned long)s_auto.cur.fp, (unsigned long)s_auto.cur.gen,
                (unsigned long)auto_profile_hash(&s_auto.cur));
            auto_emit("G", b);
            LOG("AUTO", "DATA GENERATED (validated + committed + read back). Phone result is separate. Board power may now be cut and restored.");
        }
    }
    /* 4. the requested SOC follows a reused profile */
    if (s_auto.have_cur && (s_auto.st == AS_RUNNING || s_auto.st == AS_GENERATED) && s_soc.requested != s_auto.cur.soc && !s_auto_soc_dirty)
        s_soc.requested = s_auto.cur.soc;
    /* 5. telemetry: on every state change and once a second */
    if (s_auto.st != s_auto_shown) {
        char b[128];
        snprintf(b, sizeof b, "from=%u to=%u why=%.70s", (unsigned)s_auto_shown, (unsigned)s_auto.st, s_auto.note);
        auto_emit("E", b);
        LOG("AUTO", "%s -> %s: %s", auto_state_str(s_auto_shown), auto_state_str(s_auto.st), s_auto.note);
        s_auto_shown = s_auto.st; auto_emit_status(); s_auto_last_state_ms = now;
    } else if ((uint32_t)(now - s_auto_last_state_ms) >= 1000u) { s_auto_last_state_ms = now; auto_emit_status(); }
}
#endif

static void apply_bus_policy(uint32_t now) {
    const batt_profile_t *p = profile_get(s_prof);
    if(battery_bus_fault()){
#if CHIPLER_OWNER_PHONE_TEST
        /* OWNER PHONE-TEST BUILD: a phone power-cycle (power button / reboot) cuts the bus mid-transaction and
         * latches the port. Re-arm it so the following boot finds the gauge.
         * Default builds keep the latch: explicit profile reload required, no auto retry. */
        if (p && p->status == PROF_PHONE_DATA_TEST) {
            if ((int32_t)(now - s_recover_at) < 0) return;   /* backoff slot not reached: keep the latch */
            battery_bus_diag_t d;
            bool have = battery_bus_diagnostic(&d);
            uint32_t hv[8] = {0}; uint8_t site = 0;
            bool hh = battery_bus_history(hv, &site);
            s_bus_recoveries++;
            /* a powered-down phone can re-fault on every slot: log the first 30, then every 20th */
            if (s_bus_recoveries <= 30 || s_bus_recoveries % 20 == 0) {
                LOG("BUS", "OWNER auto-recover #%lu: fault=%d site=%u cmd=0x%02X abort=0x%08lX -> restarting target (next slot %lu ms)",
                (unsigned long)s_bus_recoveries, have ? (int)d.fault : -1, site, have ? d.command : 0,
                have ? (unsigned long)d.abort_source : 0ul, (unsigned long)s_recover_backoff);
                /* IRQ history, oldest -> newest (failing batch last): [15:0] IC_INTR_STAT, [20:16] rx, [21] reading, [22] active, [31:24] tx_off */
                if (hh) LOG("BUS", "OWNER irq-hist %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                    (unsigned long)hv[0], (unsigned long)hv[1], (unsigned long)hv[2], (unsigned long)hv[3],
                    (unsigned long)hv[4], (unsigned long)hv[5], (unsigned long)hv[6], (unsigned long)hv[7]);
            }
            battery_bus_reset_fault();
            s_recover_last = now; s_recover_at = now + s_recover_backoff;
            s_recover_backoff = s_recover_backoff >= OWNER_RECOVER_MAX_MS / 2 ? OWNER_RECOVER_MAX_MS : s_recover_backoff * 2;
        } else
#endif
        return; /* explicit profile reload required, no auto retry */
    }
#if CHIPLER_OWNER_PHONE_TEST
    if (s_recover_backoff != OWNER_RECOVER_MIN_MS && battery_bus_running() && (uint32_t)(now - s_recover_last) >= OWNER_RECOVER_CALM_MS)
        s_recover_backoff = OWNER_RECOVER_MIN_MS;
#endif
    /* bench master / data-only experiment / armed output; the replies-off reference wants nothing (review 106) */
#if CHIPLER_AUTO_LEARN
    /* AUTO decides: OFF / LISTEN / RUN. LISTEN and RUN load the same template entry; only RUN answers reads. */
    auto_bus_t ab = auto_ctl_bus(&s_auto);
    bool want = ab != AB_OFF;
    if (want && s_prof != s_auto_prof) { battery_bus_stop(); s_prof = s_auto_prof; owner_recover_reset(); p = profile_get(s_prof); }
    if (!want && s_prof != profile_replies_off_index()) s_prof = profile_replies_off_index();
    if (want && battery_bus_running() && battery_bus_listening() != (ab == AB_LISTEN)) {
        battery_bus_set_listen(ab == AB_LISTEN);
        LOG("AUTO", "target %s", ab == AB_LISTEN ? "LISTEN (reads answered 0xFF)" : "RUN (template map answers)");
    }
#else
    bool want = profile_bus_wanted(p, s_safety.state == OUT_ARMED || s_safety.state == OUT_ON);
#endif
    if (want && !battery_bus_running()) {
        bb_start_t r = battery_bus_start_seq(p, s_soc.requested, bus_initial_words, (void *)p);
        if (r == BB_START_OK) {
            LOG("BUS", "target enabled at 0x%02X (%s)", p->i2c_addr,
                p->addr_status == ADDR_VERIFIED ? "verified" : "CANDIDATE");
            /* Initial responses were published while isolated, before the first possible ACK. */
#if CHIPLER_OWNER_PHONE_TEST
            /* bookkeeping only (UI "applied", SOC log): the same 0x0D/0x0F are already served */
            if (p->status == PROF_PHONE_DATA_TEST && s_soc.applied != s_soc.requested)
                log_soc(soc_apply(&s_soc, p, s_soc.requested, publish_to_engine, battery_bus_engine()));
#endif
        } else {   /* F10: name the failing step; at most once per second (this runs every control tick) */
            static uint32_t last_fail_log;
            if (!last_fail_log || (uint32_t)(now - last_fail_log) >= 1000u) {
                last_fail_log = now ? now : 1;
                LOG("BUS", "target NOT started: %s", r == BB_START_PREPARE ? (p->addr_status == ADDR_UNKNOWN ? "profile address unknown" : "prepare failed")
                                                    : r == BB_START_WORDS ? "SOC words not applied" : "activate refused");
            }
        }
    } else if (!want && battery_bus_running()) {
        battery_bus_stop(); LOG("BUS", "target disabled");
    }
}

#if CHIPLER_OWNER_PHONE_TEST
/* OWNER PHONE-TEST BUILD: the recorded 12 Pro captures were taken at 1-3 % (0x0D=2, 0x09=3455 mV), so iOS boots and then
 * shuts down for low battery. Every 100 ms, while a DATA TEST target is live:
 *   0x0D/0x0F = owner SOC (s_soc.requested), 0x0F scaled on the profile's OWN full-charge word 0x10 (0x0D == 100*0x0F/0x10);
 *   0x09      = INA226 bus voltage in mV (only with a fresh measured sample in 2.80..4.60 V).
 * These are NOT recorded values. Other words (0x0A current, 0x11/0x12 time-to-empty, 0x66 pages) stay as recorded.
 * F6: the INA226 is not calibrated (SHUNT_OHMS nominal); 0x09 is still served, and the flag is reported, not hidden.
 * F8: every state change of the two writers is logged once ("OWNER words ..."), so a silent fallback to the recorded
 * 2 % / 3455 mV is visible without typing 's'. */
/* r8: last loaded DATA TEST profile + SOC survive a reset (settings/owner_state.h). A profile load or STOP is written
 * at once (the target is off at that moment); a SOC change 1.5 s after it settles (so -10/-1 tapping is one write).
 * A failed write is retried every 5 s. After a restore the bus starts by the normal policy (DATA TEST profile => want)
 * and the restored SOC is in the map before the first ACK (review 105 R1, bus_initial_words).
 * Review 105 R2 (decision D7): flash is never written while the target can be addressed. With replies on, the write
 * runs inside battery_bus_pause() (target off for the page program, ~1 ms, then re-armed), only after a quiet gap. */
#define OWNER_STATE_QUIET_US 20000u   /* pause only after 20 ms without a bus event: the phone polls in bursts (~15 ms, 1/s) */
static owner_state_t s_ost;
static bool s_ost_dirty;
static uint32_t s_ost_at;
static int s_ost_last_err = -1;
static void owner_state_service(uint32_t now);
static void owner_state_mark(uint32_t now, bool at_once) {
    const batt_profile_t *p = profile_get(s_prof);
    memset(&s_ost, 0, sizeof s_ost);
    if (p && p->status == PROF_PHONE_DATA_TEST) snprintf(s_ost.prof, sizeof s_ost.prof, "%s", p->id);
    s_ost.soc = s_soc.requested;
    s_ost_dirty = true; s_ost_at = at_once ? now : now + 1500u;
    if (at_once) owner_state_service(now);
}
/* Review 110 R110-1: after a reset the saved profile is OFFERED (LOAD PROFILE? confirm when the splash ends), replies start
 * only on APPLY, and the saved SOC is applied then. A damaged slot (torn write, e.g. an interrupted STOP) offers nothing.
 * OWNER_AUTOSTART_AFTER_RESTORE 1 restores r8..r11 behaviour (start without a tap) - an owner decision, still fail-closed. */
#ifndef OWNER_AUTOSTART_AFTER_RESTORE
#define OWNER_AUTOSTART_AFTER_RESTORE 0
#endif
static int s_offer_prof = -1, s_offer_soc;
static bool s_offer_open;   /* review 111: the offer confirm is on screen; closing it without APPLY withdraws the offer */
static void owner_state_restore(void) {
    owner_state_t st;
    owner_boot_t b = owner_state_boot_load(&st);
    if (b == OWNER_BOOT_UNCERTAIN) { LOG("OWNER", "saved state: last write INCOMPLETE (torn/partial record); nothing offered, responses stay OFF"); return; }
    if (b == OWNER_BOOT_NONE) { LOG("OWNER", "saved state: none/STOP (boot profile %s, SOC %d%%); responses stay OFF", profile_get(s_prof)->id, s_soc.requested); return; }
    int i = profile_restore_index(st.prof);   /* review 106: "" (STOP), unknown, excluded or non-DATA-TEST -> -1 */
    if (i < 0) {
        LOG("OWNER", "saved state: profile '%s' not restored (none/unknown/not DATA TEST); responses stay OFF", st.prof);
        return;
    }
#if OWNER_AUTOSTART_AFTER_RESTORE
    s_prof = i;
    log_soc(soc_on_profile_change(&s_soc, profile_get(s_prof), publish_to_engine, battery_bus_engine()));
    s_soc.requested = st.soc;
    LOG("OWNER", "saved state RESTORED: profile %s SOC %d%% (seq %lu); phone responses start automatically", st.prof, st.soc,
        (unsigned long)st.seq);
#else
    s_offer_prof = i; s_offer_soc = st.soc;
    LOG("OWNER", "saved state OFFERED: profile %s SOC %d%% (seq %lu); responses stay OFF until LOAD PROFILE is confirmed", st.prof, st.soc,
        (unsigned long)st.seq);
#endif
}
typedef struct { owner_save_t r; bool ran; uint32_t write_us; } owner_pause_job_t;
static void owner_pause_save(void *ctx) {   /* runs with the target switched off (battery_bus_pause) */
    owner_pause_job_t *j = ctx;
    uint32_t t0 = time_us_32();
    j->r = owner_state_save(&s_ost, true);   /* true: no sector erase inside a pause */
    j->write_us = time_us_32() - t0;
    j->ran = true;
}
static void owner_state_service(uint32_t now) {
    bool run = battery_bus_running();
    owner_when_t w = owner_state_when(s_ost_dirty, (int32_t)(now - s_ost_at) >= 0, run,
                                      run ? battery_bus_idle_us(time_us_32()) : UINT32_MAX, OWNER_STATE_QUIET_US);
    if (w == OWNER_WHEN_IDLE || w == OWNER_WHEN_LATER) return;   /* LATER: bus busy, next control tick */
    owner_save_t r;
    uint32_t t0 = time_us_32(), write_us;
    bool paused = w == OWNER_WHEN_PAUSE;
    if (paused) {
        owner_pause_job_t j = {OWNER_SAVE_FLASH_ERROR, false, 0};
        bool rearmed = battery_bus_pause(owner_pause_save, &j);
        if (!j.ran) return;                  /* nothing was paused (target not live): next tick decides again */
        r = j.r; write_us = j.write_us;
        if (!rearmed) LOG("OWNER", "replies did not re-arm after the save pause; the bus policy restarts the target");
    } else {
        r = owner_state_save(&s_ost, false);
        write_us = time_us_32() - t0;
    }
    uint32_t dt = time_us_32() - t0;
    if (r == OWNER_SAVE_OK) {
        s_ost_dirty = false; s_ost_last_err = -1;
        if (paused) {
            LOG("OWNER", "state saved: profile %s SOC %d%% (replies paused %lu us, flash %lu us)", s_ost.prof[0] ? s_ost.prof : "NONE",
                s_ost.soc, (unsigned long)dt, (unsigned long)write_us);
            char t[40]; snprintf(t, sizeof t, "SAVED (REPLIES PAUSED %lu US)", (unsigned long)dt); toast(t, now);
        } else
            LOG("OWNER", "state saved: profile %s SOC %d%% (replies off, %lu us)", s_ost.prof[0] ? s_ost.prof : "NONE", s_ost.soc,
                (unsigned long)write_us);
    } else {
        s_ost_at = now + 5000u;
        if ((int)r != s_ost_last_err) {   /* review 102 F-07: log a reason once, not every retry */
            s_ost_last_err = (int)r;
            LOG("OWNER", "state NOT saved yet: %s; retrying every 5 s", r == OWNER_SAVE_WAIT_ERASE ? "sector full, erase waits for replies off (STOP/profile load)" :
                r == OWNER_SAVE_REGION ? "flash region overlaps the program image (persistence disabled)" : "flash write/verify failed");
        }
    }
}
static const char *s_ow_soc = "bus_off", *s_ow_volt = "bus_off";
static bool s_ow_uncal;
static void owner_battery_words(uint32_t now, const safety_inputs_t *in, const pm_reading_t *rd) {
    static uint32_t last;
    if ((uint32_t)(now - last) < 100) return;
    last = now;
    const char *soc_st, *volt_st;
    const batt_profile_t *p = profile_get(s_prof);
    if (!p || p->status != PROF_PHONE_DATA_TEST || !battery_bus_running()) {
        soc_st = volt_st = "bus_off";
    } else {
        uint16_t fcc;
        uint8_t img[6];
        int soc = s_soc.requested;
        if (soc < 1 || soc > 100) soc_st = "bad_soc";
        else if (!battery_bus_word(0x10, &fcc) || !owner_soc_image(img, soc, fcc)) soc_st = "no_0x10";
        else soc_st = battery_bus_publish(SOC_PUBLISH_WORDS, img, 6) ? "live" : "publish_failed";
        if (in->q != TQ_MEASURED) volt_st = "no_sample";
        else if (!owner_volt_image(img, rd->v_bus)) volt_st = "out_of_range";
        else {
            volt_st = battery_bus_publish(SOC_PUBLISH_WORDS, img, 3) ? "live" : "publish_failed";
            s_ow_uncal = rd->uncalibrated;
        }
    }
    if (soc_st != s_ow_soc || volt_st != s_ow_volt) {
        s_ow_soc = soc_st; s_ow_volt = volt_st;
        LOG("OWNER", "words soc=%s volt=%s ina_uncal=%d (0x0D/0x0F/0x09 computed, NOT recorded)", soc_st, volt_st, s_ow_uncal);
    }
}
#endif

static void read_inputs(safety_inputs_t *in, pm_reading_t *r, uint32_t now) {
    memset(in, 0, sizeof *in);
    /* CR-003: GP29 was reassigned from VIN sense to the OK button. PSU-side overvoltage is now
     * NOT qualified yet. Output remains locked; downstream VBUS is not upstream protection. */
    in->v_in = 0; in->v_in_valid = false;
    in->alert_pin = pm_fault_latched();
    bool ok = pm_read(r);
    bool fresh = pm_present() ? tf_update(&s_fresh, now, ok, ok && r->new_sample) : false;
    if (ok && fresh) {
        in->q = TQ_MEASURED; in->v_out = r->v_bus; in->i_out = r->i;
    } else if (pm_present()) {
        in->q = TQ_ABSENT;               /* read failed or no new conversion within TF_MAX_AGE_MS */
    } else if (CHIPLER_SIM_TELEMETRY) {
        in->q = TQ_SIMULATED;
        r->v_bus = 3.90f + 0.05f * sinf(now / 3000.0f);
        r->i = 0.25f + 0.2f * (0.5f + 0.5f * sinf(now / 700.0f));
        r->p = r->v_bus * r->i;
        in->v_out = r->v_bus; in->i_out = r->i;
    } else {
        in->q = TQ_ABSENT;
    }
}

static void handle_action(ui_action_t a, int arg, const safety_inputs_t *in, uint32_t now) {
    switch (a) {
    case ACT_OUTPUT_OFF:
        safety_off(&s_safety);
#if CHIPLER_AUTO_LEARN
        battery_bus_stop(); s_prof = profile_replies_off_index();
        auto_ctl_event(&s_auto, AE_STOP, 0, now);
        toast("REPLIES STOPPED - AUTO OFF", now);
        LOG("SAFE", "STOP (user): replies off, AUTO disabled until AUTO ON");
        break;
#endif
        if (profile_get(s_prof)->status == PROF_PHONE_DATA_TEST) {   /* STOP also ends a phone data experiment */
            battery_bus_stop(); s_prof = profile_replies_off_index();
            log_soc(soc_on_profile_change(&s_soc, profile_get(s_prof), publish_to_engine, battery_bus_engine()));
            toast("PHONE DATA STOPPED", now);
#if CHIPLER_OWNER_PHONE_TEST
            owner_state_mark(now, true);   /* replies are off: written now */
#endif
        }
        LOG("SAFE", "output OFF (user)");
        break;
    case ACT_ARM_REQUEST: {
        arm_result_t r = safety_request_arm(&s_safety, now, in, profile_get(s_prof));
        LOG("SAFE", "arm request -> %s", arm_result_str(r));
        if (r == ARM_OK) nav_open_confirm(&s_nav, CONF_ARM, 0, 0, now); else toast(arm_result_str(r), now);
        break;
    }
    case ACT_ARM_CONFIRM:
        if (safety_confirm(&s_safety, now, profile_get(s_prof))) LOG("SAFE", "output ON (confirmed)");
        else toast("ARM TIMED OUT", now);
        break;
    case ACT_ARM_CANCEL: safety_cancel(&s_safety); LOG("SAFE", "arm cancelled"); break;
    case ACT_LOAD_PROFILE:
        if (s_safety.state == OUT_OFF) {
            battery_bus_stop();
            battery_bus_reset_fault();
#if CHIPLER_OWNER_PHONE_TEST
            owner_recover_reset();
#endif
            s_prof = arg;
            LOG("PROF", "loaded %s (%s) status=%s backend=%s", profile_get(arg)->id, profile_get(arg)->name,
                profile_status_str(profile_get(arg)->status), profile_backend_str(profile_get(arg)));
            log_soc(soc_on_profile_change(&s_soc, profile_get(s_prof), publish_to_engine, battery_bus_engine()));
#if CHIPLER_OWNER_PHONE_TEST
            if (arg == s_offer_prof) {   /* R110-1: the boot offer was confirmed: the saved SOC comes with it */
                s_soc.requested = s_offer_soc;
                LOG("OWNER", "boot offer confirmed: %s SOC %d%%", profile_get(arg)->id, s_offer_soc);
            }
            s_offer_prof = -1;
#endif
            if (profile_get(arg)->status == PROF_PHONE_DATA_TEST)
                LOG("PHONE", "BATTERY REMOVED: external regulated 4V supply + 1V8 translator required; recorded samples (%s); GP28 OFF does NOT cut the external PSU", profile_backend_str(profile_get(arg)));
            toast("PROFILE LOADED", now);
#if CHIPLER_OWNER_PHONE_TEST
            owner_state_mark(now, true);   /* replies are off until the bus policy re-arms: written now */
#endif
        }
        break;
    case ACT_ACK_FAULT: {
        safety_inputs_t acknowledged = *in;
        if (pm_ack_fault()) acknowledged.alert_pin = false;
        if (safety_ack(&s_safety, &acknowledged)) LOG("SAFE", "fault acknowledged, output stays OFF");
        else toast("FAULT STILL PRESENT", now);
        break;
    }
    case ACT_SOC_APPLY: {
        /* Emulated SOC only: never touches safety state, eFuse EN or PSU limits. */
        soc_result_t r = soc_apply(&s_soc, profile_get(s_prof), arg, publish_to_engine, battery_bus_engine());
        log_soc(r);
        toast(r == SOC_RES_APPLIED_PHONE_DATA ? "SOC DATA SET (PHONE NOT CONFIRMED)" : r == SOC_RES_APPLIED_SYNTHETIC ? "SOC APPLIED (BENCH)" :
              r == SOC_RES_APPLIED_VERIFIED ? "SOC APPLIED" :
              r == SOC_RES_BUSY_RETRY ? "SOC PENDING" :
              r == SOC_RES_REJECTED ? "INVALID SOC" : "NOT SENT: UNSUPPORTED", now);
#if CHIPLER_AUTO_LEARN
        if (r != SOC_RES_REJECTED) auto_ctl_event(&s_auto, AE_SOC, arg, now);
#elif CHIPLER_OWNER_PHONE_TEST
        if (r != SOC_RES_REJECTED) owner_state_mark(now, false);
#endif
        break;
    }
    case ACT_CLEAR_COUNTERS: battery_bus_clear_counters(); toast("COUNTERS CLEARED", now); break;
#if CHIPLER_AUTO_LEARN
    case ACT_AUTO_ON:
        toast(auto_ctl_event(&s_auto, AE_AUTO_ON, 0, now) == AA_SAVE_CONTROL ? "AUTO ON" : "AUTO ALREADY ON", now);
        break;
    case ACT_RELEARN:
        auto_ctl_event(&s_auto, AE_RELEARN, 0, now);
        toast(s_auto.st == AS_WAIT_PHONE ? "RELEARN: LISTENING" : "STOPPED: PRESS AUTO ON", now);
        break;
    case ACT_CLEAR_PROFILES:
        toast(auto_ctl_event(&s_auto, AE_CLEAR, 0, now) == AA_CLEAR_STORE ? "CLEARING SAVED PROFILES" : "STOP REPLIES FIRST", now);
        break;
#endif
    case ACT_ROTATE:
        s_set.rot180 = s_set.rot180 ? 0 : 1;
        s_set_saved = noted_settings_save(&s_set, battery_bus_running());
        LOG("UI", "display rotation %s saved=%d", s_set.rot180 ? "180" : "normal", s_set_saved);
        toast(s_set_saved ? "ROTATED + SAVED" : (battery_bus_running() ? "NOT SAVED: STOP REPLIES FIRST" : "ROTATED (NOT SAVED)"), now);   /* F-09 */
        break;
    case ACT_CHG_START:
        /* defence in depth: even if navigation passed, there is no sink to enable */
        LOG("CHG", "start refused: sink_fitted=%d checks_ok=%d (CP-09 not implemented)", CHG_SINK_FITTED, s_chg_ok);
        toast("START BLOCKED: NO SINK", now);
        break;
    case ACT_CHG_STOP: toast("NO TEST RUNNING", now); break;
    default: break;
    }
}

/* V02-02: at most BUS_LOG_BUDGET records per control cycle; LOG never blocks (ring, drop when full) */
#define BUS_LOG_BUDGET 8
static uint32_t s_drain_now;
static void bus_rec(void *ctx, const txn_rec_t *r) {
    (void)ctx;
    s_last_txn_ms = s_drain_now;
    LOG("I2C", "t=%lu reg=0x%02X %s nw=%u nr=%u data=%02X %02X %02X %02X%s%s",
        (unsigned long)r->t_us, r->reg, (r->flags & TXN_PTR) ? "PTR" : "   ", r->nw, r->nr,
        r->first[0], r->first[1], r->first[2], r->first[3],
        (r->flags & TXN_UNKNOWN) ? " UNKNOWN-REG" : "", (r->flags & TXN_WIGNORED) ? " W-IGNORED" : "");
}
static void drain_bus_log(uint32_t now) {
    s_drain_now = now;
    te_drain(battery_bus_engine(), BUS_LOG_BUDGET, bus_rec, 0);
    st_write_t w;
    /* Diagnostic log only (R3): protocol state for selector pages is applied in the wire path
     * at STOP time, so a queued record here cannot delay or lose it. */
    for(unsigned k=0;k<4&&battery_bus_pop_write(&w);k++)
        LOG("SMB", "opaque write cmd=%02X len=%u seq=%lu; queued for log only",w.cmd,w.len,(unsigned long)w.sequence);
}
static void hal_en_write(void *ctx, bool on) { (void)ctx; out_enable(on); }
static const ctrl_hal_t s_ctrl_hal = {hal_en_write, 0};

/* ---- core-1 diagnostics, read on core 0 only (P1_r10). Never takes a lock core 1 could own. ---- */
#define C1_STACK_CANARY 0xC1C1C1C1u
static uint32_t s_core1_stack[2048];
static atomic_uint s_flash_busy;
static uint32_t s_flash_saves, s_flash_last_us, s_flash_max_us;

static bool noted_settings_save(const ui_settings_t *s, bool bus_running) {
    uint32_t t0 = time_us_32();
    atomic_store(&s_flash_busy, 1);
    bool r = settings_save(s, bus_running);
    atomic_store(&s_flash_busy, 0);
    s_flash_last_us = time_us_32() - t0; if (s_flash_last_us > s_flash_max_us) s_flash_max_us = s_flash_last_us;
    s_flash_saves++;
    return r;
}

static uint32_t core1_stack_free_bytes(void) {
    size_t n = sizeof s_core1_stack / sizeof s_core1_stack[0], i = 0;
    while (i < n && s_core1_stack[i] == C1_STACK_CANARY) i++;
    return (uint32_t)(i * sizeof s_core1_stack[0]);
}

static void log_core1_diag(const char *tag) {
    c1d_snapshot_t d;
    if (!c1d_read(&d)) { LOG(tag, "core1 diag: nothing written yet"); return; }
    spi_bus_hw_t hw; spi_bus_hw_status(&hw);
    LOG(tag, "stage=%s age_ms=%lu serial=%lu loops=%lu polls=%lu(ok=%lu fail=%lu) renders=%lu pushes=%lu rows=%lu%s",
        c1d_stage_name(d.stage), (unsigned long)(d.stage_age_us / 1000u), (unsigned long)d.stage_serial, (unsigned long)d.loops,
        (unsigned long)d.polls, (unsigned long)d.touch_ok, (unsigned long)d.touch_fail, (unsigned long)d.renders,
        (unsigned long)d.pushes, (unsigned long)d.rows_total, d.torn ? " TORN-READ" : "");
    LOG(tag, "us last/max: loop=%lu/%lu render=%lu/%lu push=%lu/%lu touch=%lu/%lu | push_mode=%s budget_full_frame_us=%lu",
        (unsigned long)d.last_loop_us, (unsigned long)d.max_loop_us, (unsigned long)d.last_render_us, (unsigned long)d.max_render_us,
        (unsigned long)d.last_push_us, (unsigned long)d.max_push_us, (unsigned long)d.last_touch_us, (unsigned long)d.max_touch_us,
        lcd_push_mode_cpu() ? "CPU" : "DMA", (unsigned long)lcd_push_budget_us((uint32_t)LCD_WIDTH * LCD_HEIGHT));
    LOG(tag, "hw: dma_busy=%d dma_remaining=%lu spi_sr=0x%02lX | timeouts dma=%lu spi=%lu touch=%lu cpu=%lu write=%lu recoveries=%lu bus_failed=%lu",
        hw.dma_busy, (unsigned long)hw.dma_remaining, (unsigned long)hw.sr, (unsigned long)d.tmo_dma, (unsigned long)d.tmo_spi,
        (unsigned long)d.tmo_touch, (unsigned long)d.tmo_cpu, (unsigned long)d.tmo_write, (unsigned long)d.recoveries, (unsigned long)d.bus_failed);
    LOG(tag, "last_fault=%s in_stage=%s | core1_stack_free=%lu of %u bytes | flash_save busy=%lu saves=%lu last_us=%lu max_us=%lu | spi_refused=%lu",
        c1d_fault_name(d.fault_kind), c1d_stage_name(d.fault_stage), (unsigned long)core1_stack_free_bytes(), (unsigned)sizeof s_core1_stack,
        (unsigned long)atomic_load(&s_flash_busy), (unsigned long)s_flash_saves, (unsigned long)s_flash_last_us, (unsigned long)s_flash_max_us,
        (unsigned long)spi_bus_violations());
}

/* Stall monitor: core 1 changes stage many times per loop (every ~1-2 ms). No stage change for STALL_MS while no flash save is
 * running means core 1 is stuck in that stage (or died). Reported at most every 2 s; it only reads and logs. */
#define C1_STALL_MS 1500u
static void core1_stall_monitor(uint32_t now_ms) {
    static uint32_t last_check, last_serial, last_change_ms, last_report, reported;
    if ((uint32_t)(now_ms - last_check) < 250u) return;
    last_check = now_ms;
    c1d_snapshot_t d;
    if (!c1d_read(&d)) return;
    if (d.stage_serial != last_serial || atomic_load(&s_flash_busy)) { last_serial = d.stage_serial; last_change_ms = now_ms; reported = 0; return; }
    if ((uint32_t)(now_ms - last_change_ms) >= C1_STALL_MS && (!reported || (uint32_t)(now_ms - last_report) >= 2000u)) {
        last_report = now_ms; reported++;
        LOG("C1", "CORE1 STALL: no progress for %lu ms (report #%lu)", (unsigned long)(now_ms - last_change_ms), (unsigned long)reported);
        log_core1_diag("C1");
    }
}

static void serial_cmd(void) {
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) return;
    if (c == 's') {
        target_engine_t *e = battery_bus_engine();
        LOG("STAT", "display_set=%s (%s) log_dropped=%lu uiq_dropped=%lu spi_refused=%lu", LCD_SET_ID, DISPLAY_VARIANT,
            (unsigned long)log_dropped(), (unsigned long)s_uiq.dropped, (unsigned long)spi_bus_violations());
        LOG("STAT", "SMBus_fault=%d boardUSB=5V phonePSU=MANUAL_4V HW_protection=ABSENT auth=%s SOC_control=NOT_SUPPORTED",
            battery_bus_fault(),battery_bus_auth_state_str());
#if CHIPLER_OWNER_PHONE_TEST
        {   uint16_t w09 = 0, w0d = 0, w0f = 0, w10 = 0;
            bool ok = battery_bus_word(0x09, &w09) & battery_bus_word(0x0D, &w0d) & battery_bus_word(0x0F, &w0f) &
                      battery_bus_word(0x10, &w10);
            LOG("STAT", "OWNER bus_auto_recoveries=%lu backoff=%lums served%s 0x09=%umV 0x0D=%u%% 0x0F=%u 0x10=%u words soc=%s volt=%s ina_uncal=%d",
                (unsigned long)s_bus_recoveries, (unsigned long)s_recover_backoff, ok ? "" : "(partial)", w09, w0d, w0f, w10,
                s_ow_soc, s_ow_volt, s_ow_uncal);
        }
#endif
        battery_bus_auth_t au;
        if (battery_bus_auth_status(&au))
            LOG("AUTH", "%s servable=%lu refusedReads=%lu refusedWrites=%lu lastRefused=%02X selectorPages=%lu unknownPages=%lu lastSelTarget=%02X",
                au.auth_str, (unsigned long)au.servable, (unsigned long)au.refused_reads,
                (unsigned long)au.refused_writes, au.last_refused_cmd,
                (unsigned long)au.selector_pages, (unsigned long)au.unknown_pages, au.last_selector_target);
        battery_bus_diag_t diag;
        if (battery_bus_diagnostic(&diag))
            LOG("PHONE", "reads=%lu short=%lu opaqueW=%lu errors=%lu lastCmd=%02X faultReason=%d rawIRQ=%08lX abortSrc=%08lX",
                (unsigned long)diag.reads, (unsigned long)diag.short_reads, (unsigned long)diag.writes, (unsigned long)diag.errors,
                diag.command, diag.fault, (unsigned long)diag.irq, (unsigned long)diag.abort_source);
        log_core1_diag("C1");
        LOG("STAT", "fw=%s prof=%s out=%d fault=%s ina=%d bus=%d txns=%lu unk=%lu ign=%lu ovf=%lu",
            FW_VERSION, profile_get(s_prof)->id, s_safety.state, fault_name(s_safety.fault),
            pm_present(), battery_bus_running(), (unsigned long)e->txns,
            (unsigned long)e->unknown_reads, (unsigned long)e->ignored_writes, (unsigned long)e->overflows);
    } else if (c == 'g') {
        log_core1_diag("C1");
    } else if (c == 'm') {
        lcd_set_push_mode(!lcd_push_mode_cpu());
        LOG("C1", "LCD push mode -> %s (isolation switch; takes effect on the next frame)", lcd_push_mode_cpu() ? "CPU register writes" : "DMA");
#if !CHIPLER_AUTO_LEARN   /* AUTO: no model list anywhere, not even on serial */
    } else if (c == 'p') {
        for (int i = 0; i < profile_count(); i++) {
            const batt_profile_t *p = profile_get(i);
            LOG("PROF", "%2d %-9s %-20s addr=0x%02X %s %s", i, p->id, p->name, p->i2c_addr, profile_status_str(p->status), profile_backend_str(p));
        }
#endif
    } else if (c == 'r') {
        capture_replay_start(); LOG("REPLAY", "saved captures: offline verification, no phone traffic");
    } else if(c=='c') {
        pm_diagnostic_t d={0};
        if(pm_diagnostic(&d))LOG("CAL", "seq=%lu raw_shunt=%d raw_current=%d raw_bus=%u raw_power=%u Rnom=10000uOhm UNCAL; external reference required",
            (unsigned long)d.seq,d.raw_shunt,d.raw_current,d.raw_bus,d.raw_power);
        else LOG("CAL", "NO FRESH INA226 SAMPLE; calibration not applied");
    } else if (c == 'd' || c == 't' || c == 'b') {   /* serial fallback when touch does not work (HR2046 at U1; owner-reported bench OK, main stability open): d = TOUCH DIAG, t = calibration, b = dashboard */
        if (s_nav.screen != SCR_FAULT) {
            s_nav.screen = c == 'd' ? SCR_TOUCHDIAG : (c == 't' ? SCR_CAL : SCR_DASH);
            LOG("UI", "serial: screen -> %s", c == 'd' ? "TOUCH DIAG" : (c == 't' ? "CALIBRATION" : "DASHBOARD"));
        }
    } else if (c == 'x') {
        battery_bus_stop(); safety_off(&s_safety); s_prof = profile_replies_off_index();
        log_soc(soc_on_profile_change(&s_soc, profile_get(s_prof), publish_to_engine, battery_bus_engine()));
        LOG("PHONE", "data isolated; reverted to locked profile; no auto restart");
#if CHIPLER_AUTO_LEARN
        auto_ctl_event(&s_auto, AE_STOP, 0, to_ms_since_boot(get_absolute_time()));
#endif
#if CHIPLER_OWNER_PHONE_TEST && !CHIPLER_AUTO_LEARN
        owner_state_mark(to_ms_since_boot(get_absolute_time()), true);
#endif
    } else if (c == 'h') {
#if CHIPLER_AUTO_LEARN
        LOG("HELP", "s=status g=core1 diag m=toggle DMA/CPU LCD push r=offline replay c=raw INA226 data x=STOP d=touch diag t=calibration b=dashboard h=help (AUTO: control by touch/buttons; the PC monitor only listens)");
#else
        LOG("HELP", "s=status g=core1 diag m=toggle DMA/CPU LCD push p=profiles r=offline replay c=raw INA226 data x=STOP DATA d=touch diag t=calibration b=dashboard h=help (control by touch only; start a data test via confirmed profile selection)");
#endif
    }
}

/* Execute one command from the touch UI. Same state machine and same actions as the physical key. */
static void process_ui_cmd(const uic_t *c, const nav_ctx_t *nc, const safety_inputs_t *in, uint32_t now) {
    int arg = -1; const char *t = 0; ui_action_t a = ACT_NONE;
    if (c->kind == UIC_AUTOCAL) {
        if (s_nav.screen != SCR_FAULT && s_nav.screen != SCR_CONFIRM && s_nav.screen != SCR_CAL && s_nav.screen != SCR_STARTUP) {
            s_nav.screen = SCR_CAL; LOG("UI", "touch seen while uncalibrated: calibration offered");
        }
        return;
    }
    if (c->kind == UIC_STOP) {      /* STOP is executed whatever screen it was issued on (DS-UI-11 C-12 / DS-TOUCH-12 N2) */
        a = nav_handle(&s_nav, BTN_EV_OK_LONG, nc, &arg, &t);
        LOG("TOUCH", "STOP REPLIES (touch) -> %s", action_name(a));
        toast("REPLIES STOPPED - SWITCH PSU OFF BY HAND", now);
        handle_action(a, arg, in, now);
        return;
    }
    if (c->kind == UIC_CAL_DONE) {
        touch_cal_t cal;
        critical_section_enter_blocking(&s_lock);
        cal = s_cal_from_ui; s_cal_to_ui = cal; s_cal_to_ui_seq++;
        critical_section_exit(&s_lock);
        s_set.cal = cal;
        s_set_saved = noted_settings_save(&s_set, battery_bus_running());
        LOG("UI", "touch calibration accepted rot=%d saved=%d (a=%.4f b=%.4f c=%.1f d=%.4f e=%.4f f=%.1f)",
            cal.rot180, s_set_saved, (double)cal.a, (double)cal.b, (double)cal.c, (double)cal.d, (double)cal.e, (double)cal.f);
        if (s_nav.screen == SCR_CAL) s_nav.screen = SCR_SETTINGS;
        toast(s_set_saved ? "CALIBRATION SAVED" : (battery_bus_running() ? "NOT SAVED: STOP REPLIES FIRST" : "CAL OK (NOT SAVED)"), now);   /* F-09 */
        return;
    }
    if (c->screen != (uint8_t)s_nav.screen) { LOG("UI", "stale touch command dropped (kind=%d)", c->kind); return; }
    switch (c->kind) {
    case UIC_SELECT: a = nav_select(&s_nav, c->idx, nc, &arg, &t); break;
    case UIC_BACK:   a = nav_back(&s_nav, nc, &arg, &t); break;
    case UIC_STOP:   a = nav_handle(&s_nav, BTN_EV_OK_LONG, nc, &arg, &t); break;
    case UIC_PAGE_UP: case UIC_PAGE_DOWN:
        if (s_nav.screen == SCR_CHG || s_nav.screen == SCR_CHG_MENU)
            nav_handle(&s_nav, c->kind == UIC_PAGE_UP ? BTN_EV_UP : BTN_EV_DOWN, nc, &arg, &t);
        else if (s_nav.screen == SCR_MODELS)
            for (int i = 0; i < 4; i++) nav_handle(&s_nav, c->kind == UIC_PAGE_UP ? BTN_EV_UP : BTN_EV_DOWN, nc, &arg, &t);
        break;
    default: break;
    }
    LOG("TOUCH", "cmd=%d idx=%d -> %s", c->kind, c->idx, action_name(a));
    if (t) toast(t, now);
    handle_action(a, arg, in, now);
}

int main(void) {
    board_io_init();                       /* output OFF before anything else */
    battery_bus_init();                    /* battery bus isolated */
    stdio_init_all();
    log_init();                            /* V02-02: bounded non-blocking log ring */
    critical_section_init(&s_lock);
    if (watchdog_caused_reboot()) { /* logged after USB enumerates */ }

    memset(&s_model, 0, sizeof s_model);
    s_model.fw = FW_VERSION; s_model.disp = LCD_SET_ID " \x80 " DISPLAY_VARIANT; s_model.screen = SCR_STARTUP;
    s_model.lim_ov = LIMIT_OV_V; s_model.lim_oc = LIMIT_OC_A; s_model.last_reg = -1;
    s_prof = profile_replies_off_index();
    s_model.prof_idx = s_prof;
    uicq_init(&s_uiq);
    settings_defaults(&s_set);
    s_set_saved = settings_load(&s_set);
    s_cal_to_ui = s_set.cal; s_cal_to_ui_seq = 1;
    s_model.rot180 = s_set.rot180; s_model.touch_cal_valid = s_set.cal.valid; s_model.settings_saved = s_set_saved;
    /* The SDK default core-1 stack is 2 KiB (PICO_CORE1_STACK_SIZE 0x800): the renderer, layout table and model copies
     * need more, so core 1 gets its own 8 KiB stack (found by review before first power-up). */
    c1d_init();
    for (size_t i = 0; i < sizeof s_core1_stack / sizeof s_core1_stack[0]; i++) s_core1_stack[i] = C1_STACK_CANARY;   /* high-water mark for 'g' */
    splash_gate_init(&s_splash_gate);     /* before core 1 exists */
    multicore_launch_core1_with_stack(core1_main, s_core1_stack, sizeof s_core1_stack);

    safety_limits_t lim = {LIMIT_OV_V, LIMIT_UV_V, LIMIT_OC_A, 10000, 500};
    safety_init(&s_safety, &lim);
    nav_init(&s_nav, profile_count());
    s_nav.chg_ui = CHG_UI_ENABLED != 0;
    buttons_init(&s_btn, 0);
    soc_init(&s_soc);                     /* 50 % after every reset; RAM only, never persisted (owner build: see r8 restore) */
    tf_init(&s_fresh);
    log_soc(soc_on_profile_change(&s_soc, profile_get(s_prof), publish_to_engine, battery_bus_engine()));
#if CHIPLER_AUTO_LEARN
    auto_boot();
    s_model.prof_idx = s_prof;
#elif CHIPLER_OWNER_PHONE_TEST
    owner_state_restore();
    s_model.prof_idx = s_prof;
#endif

    /* startup self-test (~1.5 s) */
    bool ina = false;
    for (int i = 0; i <= 100; i += 5) {
        if (i == 30) ina = pm_init();
        critical_section_enter_blocking(&s_lock);
        s_model.boot_progress = (uint8_t)i;
        critical_section_exit(&s_lock);
        sleep_ms(75);
    }
    LOG("BOOT", "CHIPLER Battery Emulator %s build=%s owner=%d 12p_ref=%d  watchdog_reboot=%d", FW_VERSION, CHIPLER_BUILD_ID_STR,
        CHIPLER_OWNER_PHONE_TEST, CHIPLER_12P_REFERENCE, watchdog_caused_reboot());
    {   /* review 102 / 103: chip identity for the firmware-protection plan (A2 vs A3/A4 boot ROM fixes) */
        uint32_t chip_id = *(io_ro_32 *)(SYSINFO_BASE + SYSINFO_CHIP_ID_OFFSET);
        uint32_t pkg = *(io_ro_32 *)(SYSINFO_BASE + SYSINFO_PACKAGE_SEL_OFFSET);
        LOG("BOOT", "CHIP sysinfo chip_id=0x%08lX revision=%u package_sel=%lu rom[0x13]=%u (raw boot ROM version byte; A2 vs A3/A4 and OTP secure-boot state to be confirmed with picotool info -a)",
            (unsigned long)chip_id, (unsigned)rp2350_chip_version(), (unsigned long)pkg, (unsigned)*(const volatile uint8_t *)0x13u);
    }
    LOG("BOOT", "VARIANT %s rev %s | DISPLAY %s = %s | controller=%s %dx%d madctl=0x%02X invert=%d SPI=%lu Hz",
        CHIPLER_VARIANT_ID, CHIPLER_REVISION, LCD_SET_ID, DISPLAY_VARIANT,
        LCD_CONTROLLER == 3 ? "ILI9341" : (LCD_CONTROLLER == 1 ? "GC9A01" : "ST7789"), LCD_WIDTH, LCD_HEIGHT,
        LCD_MADCTL, LCD_INVERT, (unsigned long)lcd_spi_actual_hz());
    LOG("BOOT", "TOUCH backend=%d (XPT2046-family command set; module chip U1 is marked HR2046; owner reports touch tracking + calibration OK on bench r7 (no capture), main-firmware stability NOT proven) cal=%s settings_from_flash=%d",
        TOUCH_BACKEND, s_set.cal.valid ? "loaded" : "NONE", s_set_saved);
    LOG("BOOT", "battery bus: translator EN present=%d (P1: none; stop releases GP6/GP7 to hi-Z inputs, no electrical isolation claimed)",
        BAT_BUS_EN_PRESENT);
    LOG("BOOT", "%s", lcd_build_id);
    LOG("BOOT", "%s %s status=%s dev_id=0x%04X", pm_name(), pm_present() ? "READY" : "NOT READY - output cannot be armed",
        pm_status(), pm_device_id());
    LOG("BOOT", "sensor_init=%d calibration=PROVISIONAL protection=UNQUALIFIED; no upstream VIN sense; limits OV %.2f V UV %.2f V OC %.2f A, INA226 ALERT trip %.2f A",
        ina, (double)LIMIT_OV_V, (double)LIMIT_UV_V, (double)LIMIT_OC_A, (double)INA_SOL_TRIP_A);
#if CHIPLER_OWNER_PHONE_TEST
    LOG("BOOT", "OWNER PHONE-TEST BUILD: phone DATA TEST profiles are ARMABLE (NOT VERIFIED); output can power a phone");
    LOG("BOOT", "auth=AUTH=UNVERIFIED(no_authentication_exchange_identified); connect a real phone at your own risk");
#endif
    LOG("BOOT", "sim_telemetry=%d armable_profiles=%d%s", CHIPLER_SIM_TELEMETRY, profile_count_armable(),
#if CHIPLER_OWNER_PHONE_TEST
        " (OWNER UNLOCK - NOT VERIFIED)"
#else
        " (expected 0: no phone profile is VERIFIED)"
#endif
    );
    /* P1_r11: NO automatic offline replay at boot (r = explicit, serial only). Phone responses are OFF until a data-test profile
     * is selected AND confirmed in the MODELS screen; the bus engine is isolated and never started by boot. */
#if CHIPLER_AUTO_LEARN
    LOG("BOOT", "AUTO_LEARN build: no phone-model selection. AUTO listens (reads answered 0xFF), matches the requests to the on-board template, generates + saves a profile, then replies. STOP / 1.2 s hold / serial x stops it and keeps it off across power cycles until AUTO ON.");
#else
    LOG("BOOT", "phone responses OFF (bus isolated, profile %s); enable only via MODELS -> a 12-family DATA TEST entry (12 mini/12/12 Pro/12PM, shared 12PM backend) -> CONFIRM; STOP / 1.2 s hold / serial x ends it; offline replay NOT auto-started", profile_get(s_prof)->id);
#endif
    /* beta 1: the splash stays up until core 1 has actually drawn it, then ~2.5 s more; no hardware is enabled by showing it */
    watchdog_enable(500, true);

    uint32_t last_ctrl = 0, last_sec = 0;
    safety_inputs_t in; pm_reading_t rd = {0};
    read_inputs(&in, &rd, 0);
    while (true) {
        watchdog_update();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        battery_bus_service(time_us_32());
#if CHIPLER_OWNER_PHONE_TEST
        /* review 111: CANCEL / timeout / any other exit from the boot offer withdraws it, so a later manual load of the same
         * model starts at the default SOC like any other load (APPLY consumed the offer in the iteration that closed it) */
        if (s_offer_open && !(s_nav.screen == SCR_CONFIRM && s_nav.confirm == CONF_MODEL && s_nav.confirm_arg == s_offer_prof)) {
            if (s_offer_prof >= 0) LOG("OWNER", "boot offer withdrawn (not applied); responses stay OFF");
            s_offer_prof = -1; s_offer_open = false;
        }
#endif
        if (s_nav.screen == SCR_STARTUP && splash_gate_leave(&s_splash_gate, now)) {   /* ack = successful push of the splash frame; 8 s fallback so a dead panel never blocks the UI */
            s_nav.screen = SCR_DASH;
#if CHIPLER_OWNER_PHONE_TEST
            /* R110-1: the saved profile is offered, never started without a tap; CANCEL or the 10 s timeout leaves replies OFF */
            if (s_offer_prof >= 0 && s_safety.state == OUT_OFF) { nav_open_confirm(&s_nav, CONF_MODEL, s_offer_prof, s_prof, now); s_offer_open = true; }
            else s_offer_prof = -1;   /* not shown (output not OFF): never pending in the background */
#endif
        }

        /* touch-only variant: no external keys (GP29 is an unconnected reserve). The decoder stays wired to
         * constant "not pressed" inputs so the shared nav code path is identical to the historical builds. */
        btn_event_t ev = buttons_update(&s_btn, now, btn_up_pressed(), btn_down_pressed(), btn_ok_pressed());
        nav_ctx_t nc = {s_safety.state, s_prof, s_soc.requested, now, s_chg_ok};
        if (ev != BTN_EV_NONE) {
            int arg = -1; const char *t = 0;
            ui_action_t a = nav_handle(&s_nav, ev, &nc, &arg, &t);
            LOG("BTN", "%s -> %s", btn_event_name(ev), action_name(a));
            if (t) toast(t, now);
            handle_action(a, arg, &in, now);
        }
        /* touch commands posted by core 1 (bounded per pass so the I2C service loop is never starved) */
        {   uic_t uc;
            if (uicq_take_stop(&s_uiq)) process_ui_cmd(&(uic_t){UIC_STOP, (uint8_t)s_nav.screen, 0}, &nc, &in, now);
            for (int k = 0; k < 4 && uicq_pop(&s_uiq, &uc); k++) process_ui_cmd(&uc, &nc, &in, now);
        }

        /* fast hardware alert path: drop output immediately */
        if (out_enabled() && pm_fault_latched()) {
            out_enable(false);
            LOG("SAFE", "ALERT pin -> output forced OFF");
        }

        /* control @100 Hz */
        if ((uint32_t)(now - last_ctrl) >= 10) {
            last_ctrl = now;
            read_inputs(&in, &rd, now);
            if (in.q != TQ_ABSENT) { s_lg_v = rd.v_bus; s_lg_a = rd.i; s_lg_w = rd.p; }
#if CHIPLER_OWNER_PHONE_TEST
            s_ow_v_ok = in.q == TQ_MEASURED; s_ow_v = rd.v_bus;   /* review 105 R1: for the pre-activation words */
#endif
            update_charge(in.q);
            /* V02-02: safety_update -> EN pin written -> only then diagnostics (non-blocking ring).
             * EN = gate(ON state AND qualified profile), recomputed every cycle (Codex R-02). */
            ctrl_event_t cev = ctrl_cycle(&s_safety, now, &in, profile_get(s_prof), &s_ctrl_hal);
            ctrl_log_event(&g_log, now, &cev, &s_safety);
            nav_sync(&s_nav, s_safety.state);
            handle_action(nav_tick(&s_nav, &nc), -1, &in, now);
            if (s_soc.retry_pending) log_soc(soc_service(&s_soc, profile_get(s_prof), publish_to_engine, battery_bus_engine()));
            apply_bus_policy(now);
#if CHIPLER_OWNER_PHONE_TEST
            owner_battery_words(now, &in, &rd);
#if CHIPLER_AUTO_LEARN
            auto_service(now);
#else
            owner_state_service(now);
#endif
#endif
            drain_bus_log(now);

            target_engine_t *e = battery_bus_engine();
            static uint32_t previous_txns;
            if(e->txns!=previous_txns){s_last_txn_ms=now;s_sec_txns+=e->txns>=previous_txns?e->txns-previous_txns:e->txns;previous_txns=e->txns;}
            if ((uint32_t)(now - last_sec) >= 1000) {
                last_sec = now;
                memmove(s_spark, s_spark + 1, SPARK_N - 1);
                s_spark[SPARK_N - 1] = s_sec_txns > 255 ? 255 : (uint8_t)s_sec_txns;
                s_sec_txns = 0;
            }
            link_state_t link = LINK_DISABLED;
            if (battery_bus_running()) {
                link = (now - s_last_txn_ms < 1000 && s_last_txn_ms) ? LINK_ACTIVE : LINK_IDLE;
                if (e->overflows != s_prev_overflows) link = LINK_ERROR;
            }
            s_prev_overflows = e->overflows;
            const batt_profile_t *p = profile_get(s_prof);

            critical_section_enter_blocking(&s_lock);
            s_model.now_ms = now;
            s_model.tq = in.q; s_model.sensor_uncalibrated = rd.uncalibrated;
            s_model.v = rd.v_bus; s_model.a = rd.i; s_model.w = rd.p;
            s_model.soc_requested = s_soc.requested; s_model.soc_applied = s_soc.applied;
            s_model.soc_result = s_soc.last;
            s_model.link = link;
            s_model.addr = p->i2c_addr; s_model.addr_verified = p->addr_status == ADDR_VERIFIED;
            s_model.bus_enabled = battery_bus_running();
            capture_stats_t replay = capture_replay_stats();
            s_model.replay_frames = replay.frames; s_model.replay_total = capture_replay_count(); s_model.replay_errors = replay.errors;
            s_model.txns = e->txns; s_model.rx = e->rx_bytes; s_model.tx = e->tx_bytes;
            s_model.unknown = e->unknown_reads; s_model.ignored = e->ignored_writes; s_model.overflows = e->overflows;
            s_model.last_reg = e->last_reg;
            memcpy(s_model.spark, s_spark, SPARK_N);
            s_model.prof_idx = s_prof;
            {   bool have = in.q == TQ_SIMULATED || s_fresh.have_sample;
                uint32_t age = in.q == TQ_SIMULATED ? 0 : (uint32_t)(now - s_fresh.last_new_ms);
                s_model.t_have = have; s_model.t_age_ms = have ? age : 0;
                s_model.lg_v = s_lg_v; s_model.lg_a = s_lg_a; s_model.lg_w = s_lg_w;
                s_model.chg_flow = chg_flow(in.q, have, age, rd.i);
                s_model.chg_checks = s_chk; s_model.chg_start_ok = s_chg_ok;
                s_model.chg_sink_fitted = CHG_SINK_FITTED; s_model.chg_input_fitted = CHG_INPUT_SENSOR_FITTED;
                s_model.chg_page = s_nav.chg_page; s_model.chg_menu_cur = s_nav.chg_menu_cur;
            }
            s_model.soc_capable = p && (p->soc_build != 0 || p->soc_words != 0);
            s_model.chg_ui = CHG_UI_ENABLED != 0;
            s_model.rot180 = s_set.rot180; s_model.touch_cal_valid = s_set.cal.valid; s_model.settings_saved = s_set_saved;
            s_model.ina_present = pm_present();
            s_model.out = s_safety.state; s_model.fault = s_safety.fault; s_model.fault_value = s_safety.fault_value;
            s_model.screen = s_nav.screen; s_model.menu_cur = s_nav.menu_cur;
            s_model.model_cur = s_nav.model_cur; s_model.soc_cur = s_nav.soc_cur; s_model.soc_pick = s_nav.soc_pick;
            s_model.confirm = s_nav.confirm; s_model.confirm_cur = s_nav.confirm_cur;
            s_model.confirm_arg = s_nav.confirm_arg; s_model.confirm_from = s_nav.confirm_from;
            memcpy(s_model.toast, s_toast, sizeof s_toast); s_model.toast_until = s_toast_until;
#if CHIPLER_AUTO_LEARN
            s_model.auto_build = true;
            s_model.auto_state = (uint8_t)s_auto.st; s_model.auto_bus = (uint8_t)auto_ctl_bus(&s_auto);
            s_model.auto_have = s_auto.have_cur; s_model.auto_reused = s_auto.reused;
            s_model.auto_partial = s_auto.have_cur && (s_auto.cur.flags & AUTO_PF_PARTIAL);
            s_model.auto_fp = s_auto.cur.fp; s_model.auto_gen = s_auto.cur.gen;
            s_model.auto_saved = (uint8_t)s_auto.store.n;
            s_model.auto_dist = s_auto.m.distinct; s_model.auto_cov = s_auto.m.covered; s_model.auto_unc = s_auto.m.uncovered;
            s_model.auto_reads = s_auto.obs.n_read; s_model.auto_drops = battery_bus_obs_dropped();
            snprintf(s_model.auto_note, sizeof s_model.auto_note, "%s", s_auto.note);
#endif
            critical_section_exit(&s_lock);
        }
        if (capture_replay_step(1)) {
            capture_stats_t st = capture_replay_stats();
            LOG("REPLAY", "complete: frames=%lu validPEC=%lu rejected=%lu offline71=%lu shortA8=%lu errors=%lu",
                (unsigned long)st.frames,(unsigned long)st.valid,(unsigned long)st.rejected,
                (unsigned long)st.offline71,(unsigned long)st.short_a8,(unsigned long)st.errors);
        }
        core1_stall_monitor(now);
        serial_cmd();
        log_pump();                        /* only what USB can take now; never waits */
        sleep_us(1000 - (time_us_32() % 1000) + 1);  /* ~1 ms tick */
    }
}
