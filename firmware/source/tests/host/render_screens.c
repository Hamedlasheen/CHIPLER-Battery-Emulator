/* Host tool: renders every firmware UI screen with the REAL firmware renderer (gfx + ui_render)
 * into PPM files, so the on-device output can be compared with the approved mockups before
 * hardware exists. Values below are SAMPLE inputs for rendering only. */
#include <stdio.h>
#include <string.h>
#include "../../src/gfx/gfx.h"
#include "../../src/ui/ui_render.h"
#include "../../src/batt/profile.h"
#include "../../src/batt/soc_ctrl.h"
#include "../../src/ui/ui_nav.h"
#include "../../src/charge/charge_diag.h"

static uint16_t fb[GFX_W * GFX_H];

static void save(const char *dir, const char *name) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", GFX_W, GFX_H);
    for (int i = 0; i < GFX_W * GFX_H; i++) {
        rgb_t c = gfx_888(fb[i]);
        unsigned char px[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    gfx_init(fb);
    ui_model_t m;
    memset(&m, 0, sizeof m);
    m.fw = "beta1-CODEX"; m.disp = "A \x80 GC9A01 BGR INV"; m.lim_ov = 4.45f; m.lim_oc = 3.5f; m.now_ms = 1000;
    m.prof_idx = profile_find("P12PM-r0");
    m.tq = TQ_MEASURED; m.v = 4.012f; m.a = 1.284f; m.w = 5.15f;
    m.soc_requested = 50; m.soc_applied = -1; m.soc_result = SOC_RES_NOT_SUPPORTED;
    m.link = LINK_DISABLED; m.addr = 0x0B; m.addr_verified = true; m.replay_frames = m.replay_total = 8323; m.sensor_uncalibrated = true; m.rx = 0; m.tx = 0; m.unknown = 0; m.ignored = 0; m.last_reg = -1;
    uint8_t sp[SPARK_N] = {3,5,4,7,9,8,6,9,12,10,7,6,8,11,9,6,5,7,8,10,9,7};
    memcpy(m.spark, sp, sizeof sp);
    m.out = OUT_OFF;

    m.screen = SCR_STARTUP; m.boot_progress = 62; ui_render(&m); save(dir, "fw_A5_startup");
    m.screen = SCR_DASH; ui_render(&m); save(dir, "fw_A1_dashboard_soc_not_sent");
    m.screen = SCR_MENU; m.menu_cur = MI_SOC; ui_render(&m); save(dir, "fw_B1_menu");
    m.screen = SCR_SOC; m.soc_cur = 2; ui_render(&m); save(dir, "fw_B2_soc_select");
    m.screen = SCR_CONFIRM; m.confirm = CONF_SOC; m.confirm_cur = 0; m.confirm_from = 50; m.confirm_arg = 100;
    ui_render(&m); save(dir, "fw_B3_soc_confirm_cancel_default");
    m.confirm_cur = 1; ui_render(&m); save(dir, "fw_B4_soc_confirm_apply_highlighted");
    /* after confirming 100 % on an unsupported phone profile: requested 100, NOT SENT */
    m.screen = SCR_DASH; m.soc_requested = 100; ui_render(&m); save(dir, "fw_B5_dashboard_100_not_sent");
    /* bench synthetic backend */
    m.prof_idx = profile_find("BENCH-r0"); m.soc_applied = 100; m.soc_result = SOC_RES_APPLIED_SYNTHETIC;
    ui_render(&m); save(dir, "fw_B6_dashboard_bench_applied");
    m.screen = SCR_SOC; m.soc_cur = 0; ui_render(&m); save(dir, "fw_B7_soc_bench");
    m.prof_idx = profile_find("P12PM-r0"); m.soc_applied = -1; m.soc_result = SOC_RES_NOT_SUPPORTED;
    m.screen = SCR_MODELS; m.model_cur = m.prof_idx + 1; ui_render(&m); save(dir, "fw_A2_models");
    m.model_cur = 0; ui_render(&m); save(dir, "fw_A2b_models_back");
    m.model_cur = profile_find("CAP12PM-B1") + 1; ui_render(&m); save(dir,"beta1_capture_bench_model");
    m.model_cur = profile_find("SMB-BENCH") + 1; ui_render(&m); save(dir,"beta1_smbus_soc_bench_model");
    m.screen = SCR_CONFIRM; m.confirm = CONF_MODEL; m.confirm_arg = profile_find("P13-r0"); m.confirm_cur = 0;
    ui_render(&m); save(dir, "fw_B8_model_confirm");
    m.screen = SCR_PROTO; ui_render(&m); save(dir, "fw_A3_protocol");
    m.screen = SCR_FAULT; m.fault = FLT_NO_MONITOR; ui_render(&m); save(dir, "fw_A4_fault");
    m.screen = SCR_CONFIRM; m.confirm = CONF_ARM; m.confirm_cur = 0; ui_render(&m); save(dir, "fw_A6_arm_confirm");
    m.screen = SCR_DASH; m.tq = TQ_ABSENT; m.link = LINK_DISABLED;
    ui_render(&m); save(dir, "fw_A1b_dashboard_no_monitor");

    /* ---- CHARGE TEST mode (v0.3.0-dev). Values are SAMPLE inputs for rendering only. ---- */
    m.tq = TQ_MEASURED; m.link = LINK_DISABLED; m.out = OUT_OFF; m.fault = FLT_NONE;
    m.prof_idx = profile_find("P12PM-r0"); m.soc_requested = 50; m.soc_applied = -1; m.soc_result = SOC_RES_NOT_SUPPORTED;
    m.chg_sink_fitted = false; m.chg_input_fitted = false; m.t_have = true; m.t_age_ms = 24;
    chg_eval_checks(&m.chg_checks, false, false, TQ_MEASURED, false, false, false); m.chg_start_ok = false;
    m.screen = SCR_MENU; m.menu_cur = MI_CHARGE; ui_render(&m); save(dir, "fw_C0_menu_charge_test");
    m.screen = SCR_CHG; m.chg_page = 0;
    m.v = 3.982f; m.a = 0.418f; m.w = 1.66f; m.chg_flow = chg_flow(m.tq, true, 24, m.a);
    ui_render(&m); save(dir, "fw_C1_charge_overview_supplying");
    m.v = 4.126f; m.a = -0.842f; m.w = -3.47f; m.chg_flow = chg_flow(m.tq, true, 24, m.a);
    ui_render(&m); save(dir, "fw_C2_charge_overview_receiving");
    m.soc_requested = 100; m.v = 4.180f; m.a = 0.004f; m.w = 0.02f; m.chg_flow = chg_flow(m.tq, true, 24, m.a);
    ui_render(&m); save(dir, "fw_C3_charge_overview_no_net_100");
    m.soc_requested = 50;
    m.tq = TQ_ABSENT; m.t_have = false; m.chg_flow = chg_flow(m.tq, false, 0, 0);
    ui_render(&m); save(dir, "fw_C4_charge_overview_unavailable");
    m.t_have = true; m.t_age_ms = 1800; m.lg_v = 4.101f; m.lg_a = 0.790f; m.lg_w = 3.24f;
    m.chg_flow = chg_flow(m.tq, true, 1800, 0); ui_render(&m); save(dir, "fw_C5_charge_overview_stale");
    m.tq = TQ_SIMULATED; m.v = 3.92f; m.a = 0.31f; m.w = 1.22f; m.t_age_ms = 0; m.chg_flow = chg_flow(m.tq, true, 0, m.a);
    ui_render(&m); save(dir, "fw_C6_charge_overview_uidemo_simulated");
    m.tq = TQ_MEASURED; m.t_age_ms = 24; m.v = 4.126f; m.a = 0.842f; m.w = 3.47f; m.chg_flow = chg_flow(m.tq, true, 24, m.a);
    m.chg_page = 1; ui_render(&m); save(dir, "fw_C7_charge_input_unavailable");
    m.chg_page = 2; ui_render(&m); save(dir, "fw_C8_charge_battery_terminal");
    m.chg_page = 3; ui_render(&m); save(dir, "fw_C9_charge_protocol");
    m.chg_page = 4; ui_render(&m); save(dir, "fw_C10_charge_soc_context");
    m.chg_page = 5; ui_render(&m); save(dir, "fw_C11_charge_result_not_evaluated");
    m.screen = SCR_CHG_MENU; m.chg_menu_cur = CMI_START; ui_render(&m); save(dir, "fw_C12_charge_menu");
    m.screen = SCR_CONFIRM; m.confirm = CONF_CHG_START; m.confirm_cur = 0; ui_render(&m); save(dir, "fw_C13_charge_start_blocked");

    /* ---- alerts / faults / toasts (v0.4.0-dev contact sheet) ---- */
    m.screen = SCR_FAULT; m.chg_page = 0;
    m.fault = FLT_OVERVOLTAGE; m.fault_value = 4.61f; ui_render(&m); save(dir, "fw_F1_fault_overvoltage");
    m.fault = FLT_OVERCURRENT; m.fault_value = 3.82f; ui_render(&m); save(dir, "fw_F2_fault_overcurrent");
    m.fault = FLT_ALERT_PIN; ui_render(&m); save(dir, "fw_F3_fault_alert_line");
    m.fault = FLT_UNSUPPORTED_MODEL; ui_render(&m); save(dir, "fw_F4_fault_unsupported_model");
    m.fault = FLT_NONE; m.screen = SCR_DASH; m.tq = TQ_MEASURED; m.v = 4.012f; m.a = 0.0f; m.w = 0.0f;
    m.now_ms = 5000; m.toast_until = 7000;
    snprintf(m.toast, sizeof m.toast, "%s", arm_result_str(ARM_REFUSED_PROFILE)); ui_render(&m); save(dir, "fw_T1_toast_enable_refused");
    snprintf(m.toast, sizeof m.toast, "NOT SENT: UNSUPPORTED"); ui_render(&m); save(dir, "fw_T2_toast_soc_not_sent");
    m.screen = SCR_CHG; m.chg_page = 0; m.chg_flow = chg_flow(m.tq, true, 24, m.a); snprintf(m.toast, sizeof m.toast, "START BLOCKED: NO SINK");
    ui_render(&m); save(dir, "fw_T3_toast_charge_start_blocked");
    m.toast[0] = 0;
    printf("rendered to %s\n", dir);
    return 0;
}
