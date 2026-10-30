/* Host tool: renders every P1 landscape (320x240) UI screen/state with the REAL firmware renderer
 * (gfx + ui_ls) into PPM files. Values are SAMPLE inputs for rendering only (no hardware, no phone). */
#include <stdio.h>
#include <string.h>
#include "../../src/gfx/gfx.h"
#include "../../src/ui/ui_ls.h"
#include "../../src/ui/ui_nav.h"
#include "../../src/batt/profile.h"
#include "../../src/batt/soc_ctrl.h"

static uint16_t fb[GFX_W * GFX_H];

static void save(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
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
    m.fw = "beta 1 u063"; m.disp = "P1 \x80 ILI9341 320x240 touch"; m.lim_ov = 4.45f; m.lim_oc = 3.5f; m.now_ms = 1000;
    m.prof_idx = profile_find("P12PM-r0");
    m.tq = TQ_MEASURED; m.v = 4.012f; m.a = 1.284f; m.w = 5.15f; m.ina_present = true; m.sensor_uncalibrated = true;
    m.soc_requested = 50; m.soc_applied = -1; m.soc_result = SOC_RES_NOT_SUPPORTED; m.soc_capable = false;
    m.link = LINK_DISABLED; m.addr = 0x0B; m.addr_verified = true; m.last_reg = -1;
    m.touch_cal_valid = true; m.settings_saved = false;
    m.out = OUT_OFF;
    ls_touch_info_t ti; memset(&ti, 0, sizeof ti);
    ti.hw_enabled = true; ti.cal_valid = true; ti.ever_pen = true; ti.rx = 2210; ti.ry = 1840; ti.rz = 1130;
    ti.sx = 161; ti.sy = 118; ti.down = true; ti.irq = true; ti.polls = 18432; ti.presses = 37; ti.taps = 31; ti.rejected = 2;

    m.screen = SCR_STARTUP; m.boot_progress = 62; ui_ls_render(&m, &ti); save(dir, "P1_01_startup");
    m.screen = SCR_DASH; m.boot_progress = 100; ui_ls_render(&m, &ti); save(dir, "P1_02_dashboard_soc_not_sent");
    m.link = LINK_ACTIVE; m.prof_idx = profile_find("CAP12PM-B1"); ui_ls_render(&m, &ti); save(dir, "P1_03_dashboard_capture_bench_link_active");
    m.link = LINK_DISABLED; m.prof_idx = profile_find("P12PM-r0");
    m.tq = TQ_ABSENT; m.ina_present = false; ui_ls_render(&m, &ti); save(dir, "P1_04_dashboard_no_ina226");
    m.tq = TQ_MEASURED; m.ina_present = true;
    m.screen = SCR_MENU; ui_ls_render(&m, &ti); save(dir, "P1_05_menu");
    m.screen = SCR_SOC; ui_ls_render(&m, &ti); save(dir, "P1_06_soc_locked");
    m.soc_capable = true; m.prof_idx = profile_find("SMB-BENCH"); m.soc_applied = 50; m.soc_result = SOC_RES_APPLIED_SYNTHETIC;
    ui_ls_render(&m, &ti); save(dir, "P1_07_soc_bench_available");
    m.prof_idx = profile_find("P12PM-r0"); m.soc_capable = false; m.soc_applied = -1; m.soc_result = SOC_RES_NOT_SUPPORTED;
    m.screen = SCR_CONFIRM; m.confirm = CONF_SOC; m.confirm_cur = 0; m.confirm_from = 50; m.confirm_arg = 100;
    ui_ls_render(&m, &ti); save(dir, "P1_08_confirm_soc_cancel_default");
    m.confirm_cur = 1; ui_ls_render(&m, &ti); save(dir, "P1_09_confirm_soc_apply_highlighted");
    m.confirm = CONF_MODEL; m.confirm_cur = 0; m.confirm_arg = profile_find("P13-r0"); ui_ls_render(&m, &ti); save(dir, "P1_10_confirm_model");
    m.screen = SCR_MODELS; m.model_cur = m.prof_idx + 1; ui_ls_render(&m, &ti); save(dir, "P1_11_models_page1");
    m.model_cur = profile_find("CAP12PM-B1") + 1; ui_ls_render(&m, &ti); save(dir, "P1_12_models_capture_bench_page");
    m.screen = SCR_PROTO; m.link = LINK_ACTIVE; m.replay_frames = m.replay_total = 8323; m.rx = 1204; m.tx = 986; m.unknown = 0; m.ignored = 2; m.last_reg = 0x09;
    ui_ls_render(&m, &ti); save(dir, "P1_13_protocol");
    m.screen = SCR_BATT; m.soc_result = SOC_RES_NOT_SUPPORTED; ui_ls_render(&m, &ti); save(dir, "P1_14_battery_data");
    m.link = LINK_DISABLED;
    m.screen = SCR_SETTINGS; ui_ls_render(&m, &ti); save(dir, "P1_15_settings");
    m.touch_cal_valid = false; ui_ls_render(&m, &ti); save(dir, "P1_16_settings_uncalibrated");
    m.screen = SCR_TOUCHDIAG; ui_ls_render(&m, &ti); save(dir, "P1_17_touch_diagnostic");
    m.screen = SCR_CAL; ti.cal_step = 2; ti.cal_total = 5; ti.cal_tx = 292; ti.cal_ty = 216; ti.cal_left_ms = 38000; ui_ls_render(&m, &ti); save(dir, "P1_18_touch_calibration");
    m.screen = SCR_FAULT; m.fault = FLT_ALERT_PIN; ui_ls_render(&m, &ti); save(dir, "P1_19_fault_alert");
    m.fault = FLT_OVERCURRENT; m.fault_value = 3.62f; ui_ls_render(&m, &ti); save(dir, "P1_20_fault_overcurrent");
    m.screen = SCR_DASH; strcpy(m.toast, "COUNTERS CLEARED"); m.toast_until = m.now_ms + 1000; ui_ls_render(&m, &ti); save(dir, "P1_21_dashboard_toast");
    /* ---- CHARGE TEST screens (hidden by owner choice; rendered for the record) ---- */
    m.toast[0] = 0; m.chg_ui = true; m.chg_flow = CHF_SUPPLYING; m.t_have = true; m.t_age_ms = 40;
    m.screen = SCR_MENU; ui_ls_render(&m, &ti); save(dir, "P1_40_menu_with_charge_tile");
    for (int pg = 0; pg < CHG_PAGES; pg++) { char nm[64]; m.screen = SCR_CHG; m.chg_page = pg; snprintf(nm, sizeof nm, "P1_4%d_charge_page%d", pg + 1, pg + 1); ui_ls_render(&m, &ti); save(dir, nm); }
    m.chg_page = 0; m.chg_flow = CHF_RECEIVING; m.a = -0.84f; ui_ls_render(&m, &ti); save(dir, "P1_47_charge_receiving");
    m.chg_flow = CHF_STALE; m.t_age_ms = 3200; m.lg_v = 3.91f; m.lg_a = 0.42f; m.lg_w = 1.64f; ui_ls_render(&m, &ti); save(dir, "P1_48_charge_stale");
    m.screen = SCR_CHG_MENU; m.chg_menu_cur = 0; ui_ls_render(&m, &ti); save(dir, "P1_49_charge_menu");
    m.screen = SCR_CONFIRM; m.confirm = CONF_CHG_START; m.confirm_cur = 0; m.chg_start_ok = false;
    m.chg_checks.c[CHK_PROFILE] = CHK_FAIL; m.chg_checks.c[CHK_SINK] = CHK_FAIL; m.chg_checks.c[CHK_SENSORS] = CHK_PASS; m.chg_checks.c[CHK_CHARGER] = CHK_UNKNOWN; m.chg_checks.c[CHK_RECIPE] = CHK_FAIL;
    ui_ls_render(&m, &ti); save(dir, "P1_50_charge_start_confirm_blocked");
    m.chg_ui = false; m.chg_flow = CHF_UNAVAILABLE; m.a = 1.284f; m.out = OUT_OFF;
    /* ---- PHONE DATA TEST (T06 samples, ported from beta2) and remaining fault / state variants ---- */
    m.toast[0] = 0;
    m.screen = SCR_MODELS; m.model_cur = profile_find("P12PM-B2") + 1; ui_ls_render(&m, &ti); save(dir, "P1_22_models_phone_data_test");
    m.screen = SCR_CONFIRM; m.confirm = CONF_MODEL; m.confirm_cur = 0; m.confirm_arg = profile_find("P12PM-B2"); ui_ls_render(&m, &ti); save(dir, "P1_23_confirm_phone_data_test");
    m.prof_idx = profile_find("P12PM-B2"); m.screen = SCR_DASH; m.link = LINK_ACTIVE; m.bus_enabled = true; m.out = OUT_OFF; ui_ls_render(&m, &ti); save(dir, "P1_24_dashboard_phone_data_active");
    m.screen = SCR_PROTO; m.rx = 20; m.tx = 34; m.last_reg = 0x0D; ui_ls_render(&m, &ti); save(dir, "P1_25_protocol_phone_data");
    m.link = LINK_ERROR; m.overflows = 1; ui_ls_render(&m, &ti); save(dir, "P1_26_protocol_link_error");
    m.prof_idx = profile_find("P12PM-r0"); m.link = LINK_DISABLED; m.bus_enabled = false; m.overflows = 0;
    m.screen = SCR_FAULT; m.fault = FLT_OVERVOLTAGE; m.fault_value = 4.71f; ui_ls_render(&m, &ti); save(dir, "P1_27_fault_overvoltage");
    m.fault = FLT_NO_MONITOR; ui_ls_render(&m, &ti); save(dir, "P1_28_fault_no_monitor");
    m.fault = FLT_UNSUPPORTED_MODEL; ui_ls_render(&m, &ti); save(dir, "P1_29_fault_unsupported_model");
    m.screen = SCR_DASH; m.out = OUT_ARMED; ui_ls_render(&m, &ti); save(dir, "P1_30_dashboard_armed");
    m.out = OUT_OFF; m.tq = TQ_SIMULATED; ui_ls_render(&m, &ti); save(dir, "P1_31_dashboard_simulated_ui_demo");
    m.tq = TQ_MEASURED; m.sensor_uncalibrated = false; m.screen = SCR_DASH; ui_ls_render(&m, &ti); save(dir, "P1_32_dashboard_calibrated");
    /* ---- 062: 12-family SOC data test, splash ---- */
    m.screen = SCR_STARTUP; m.boot_progress = 100; m.fw = "beta 1 u063"; m.touch_cal_valid = true; m.ina_present = true; ui_ls_render(&m, &ti); save(dir, "P1_33_splash");
    m.prof_idx = profile_find("P12P-B2"); m.soc_capable = true; m.tq = TQ_MEASURED; m.screen = SCR_SOC; m.soc_requested = 50; m.soc_applied = -1; m.soc_result = SOC_RES_RECORDED_RETAINED; ui_ls_render(&m, &ti); save(dir, "P1_34_soc_12pro_recorded_retained");
    m.soc_requested = 25; m.soc_applied = 25; m.soc_result = SOC_RES_APPLIED_PHONE_DATA; ui_ls_render(&m, &ti); save(dir, "P1_35_soc_12pro_reply_data_set");
    m.screen = SCR_DASH; m.link = LINK_ACTIVE; m.bus_enabled = true; ui_ls_render(&m, &ti); save(dir, "P1_36_dashboard_12pro_soc_data_set");
    m.screen = SCR_BATT; ui_ls_render(&m, &ti); save(dir, "P1_37_battery_data_12pro");
    m.screen = SCR_MODELS; m.model_cur = profile_find("P12m-B2") + 1; ui_ls_render(&m, &ti); save(dir, "P1_38_models_12mini_selectable");
    /* ---- 063 R4: guide page 9 states (four DATA TEST models, SOC select/confirm, recorded 51, REPLY DATA SET) ---- */
    m.prof_idx = profile_find("P12P-B2"); m.soc_capable = true; m.soc_requested = 50; m.soc_applied = -1; m.soc_result = SOC_RES_RECORDED_RETAINED;
    m.screen = SCR_CONFIRM; m.confirm = CONF_SOC; m.confirm_cur = 0; m.confirm_from = 50; m.confirm_arg = 25; ui_ls_render(&m, &ti); save(dir, "P1_60_soc_confirm_12pro");
    m.screen = SCR_MODELS; m.model_cur = profile_find("P12PM-B2") + 1; ui_ls_render(&m, &ti); save(dir, "P1_61_models_12promax_entry");
    m.screen = SCR_CONFIRM; m.confirm = CONF_MODEL; m.confirm_cur = 0; m.confirm_arg = profile_find("P12m-B2"); ui_ls_render(&m, &ti); save(dir, "P1_62_confirm_model_12mini");
    m.prof_idx = profile_find("P13-r0"); m.soc_capable = false; m.soc_applied = -1; m.soc_result = SOC_RES_NOT_SUPPORTED; m.screen = SCR_SOC; ui_ls_render(&m, &ti); save(dir, "P1_63_soc_locked_iphone13_locked_profile");
    m.screen = SCR_MODELS; m.model_cur = profile_find("P12P-B2") + 1; ui_ls_render(&m, &ti); save(dir, "P1_64_models_12_family_page");
    /* review 106: shared 12 Pro engine (owner build) vs an excluded family */
    m.screen = SCR_CONFIRM; m.confirm = CONF_MODEL; m.confirm_cur = 0; m.confirm_arg = profile_find("P14PM-r0"); ui_ls_render(&m, &ti); save(dir, "P1_65_confirm_shared12_iphone14promax");
    m.confirm_arg = profile_find("P15-r0"); ui_ls_render(&m, &ti); save(dir, "P1_66_confirm_excluded_iphone15");
    m.confirm = CONF_NONE; m.prof_idx = profile_find("PX-r0"); m.screen = SCR_DASH; m.link = LINK_ACTIVE; m.bus_enabled = true; m.out = OUT_OFF;
    ui_ls_render(&m, &ti); save(dir, "P1_67_dashboard_shared12_iphonex_active");
    m.screen = SCR_MODELS; m.model_cur = profile_find("P13-r0") + 1; ui_ls_render(&m, &ti); save(dir, "P1_68_models_13_family_page");
    return 0;
}
