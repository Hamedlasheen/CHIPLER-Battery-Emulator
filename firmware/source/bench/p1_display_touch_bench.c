/*
 * CHIPLER P1_r3 - BRING-UP IMAGE (bench only, NOT the release): display + touch, nothing else.
 * No I2C, no battery bus, no INA226, no flash writes, no power path. Safe to run with only the display module wired.
 *
 *   stage 1  (2.5 s) 8 colour bars: white yellow cyan green magenta red blue black  -> checks BGR/RGB, inversion, width
 *   stage 2  (4 s)   geometry: border, corner markers TL red / TR green / BL blue / BR white, arrow "TOP" -> orientation
 *   stage 3  RAW MONITOR: shows raw X Y Z, T_IRQ pin level, counters. Touch the panel anywhere.
 *            If raw stays 0 / constant, check T_DO/T_CS/T_IRQ wiring (module chip U1 = HR2046). Serial (USB CDC) prints the same at 20 Hz while touched.
 *            Tap anywhere (uncalibrated) or the CALIBRATE box (calibrated) to start stage 4.
 *   stage 4  5-point calibration (same code as the firmware): residual error is printed, > 10 px is rejected
 *   stage 5  PAINT: calibrated crosshair follows the finger, corners are marked, shows screen X Y
 *            tap "RESTART CAL" (top right) to clear the calibration and go back to stage 3.
 * Serial keys: r = rotate 180 (display + mapping), s = status, h = help.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "board_config.h"
#include "hal/lcd.h"
#include "hal/spi_bus.h"
#include "gfx/gfx.h"
#include "gfx/fonts_data.h"
#include "input/touch_core.h"
#include "input/touch_hw.h"

static uint16_t fb[GFX_W * GFX_H];
static touch_core_t tc;
static bool rot180;

static void present(void) { lcd_push_frame_dirty(fb, false); }

static void stage_bars(void) {
    lcd_test_pattern();
    printf("[BRINGUP] stage 1: colour bars (white yellow cyan green magenta red blue black), controller=ILI9341 madctl=0x%02X invert=%d spi=%lu Hz\n",
           rot180 ? LCD_MADCTL_ROT180 : LCD_MADCTL, LCD_INVERT, (unsigned long)lcd_spi_actual_hz());
    sleep_ms(2500);
    lcd_invalidate();
}

static void stage_geometry(void) {
    gfx_clear(RGB(0x000000));
    gfx_round_rect(1, 1, 319, 239, 0, 0, false, RGB(0xFFFFFF), 2);
    gfx_fill_rect(0, 0, 24, 24, RGB(0xFF0000)); gfx_fill_rect(296, 0, 320, 24, RGB(0x00FF00));
    gfx_fill_rect(0, 216, 24, 240, RGB(0x0000FF)); gfx_fill_rect(296, 216, 320, 240, RGB(0xFFFFFF));
    gfx_text(&font_sb12, 160, 40, "TOP", RGB(0xFFFFFF), ALIGN_C, true, 1);
    gfx_triangle(160, 54, 168, 68, 152, 68, RGB(0xFFFFFF));
    gfx_text(&font_sb9, 160, 120, "320 x 240 LANDSCAPE", RGB(0x38D6F5), ALIGN_C, true, 1);
    gfx_text(&font_m8, 160, 140, "red = top-left, green = top-right", RGB(0xC0C0C0), ALIGN_C, true, 0);
    gfx_text(&font_m8, 160, 152, "blue = bottom-left, white = bottom-right", RGB(0xC0C0C0), ALIGN_C, true, 0);
    gfx_text(&font_m8, 160, 176, "text must read left-to-right, not mirrored", RGB(0xFFB224), ALIGN_C, true, 0);
    gfx_text(&font_m8, 160, 192, "if wrong: serial key r (rotate 180)", RGB(0x8C98A8), ALIGN_C, true, 0);
    present();
    printf("[BRINGUP] stage 2: geometry (verify orientation, mirroring, colour of the 4 corners)\n");
    sleep_ms(4000);
}

static void draw_raw(const touch_raw_t *r, bool irq) {
    char b[64];
    gfx_clear(RGB(0x06080B));
    gfx_text(&font_sb12, 160, 20, "TOUCH RAW MONITOR", RGB(0xF3F6F9), ALIGN_C, true, 1);
    snprintf(b, sizeof b, "X %4u   Y %4u   Z %5u", r->x, r->y, r->z);
    gfx_text(&font_sb12, 160, 60, b, r->pen ? RGB(0x2FD39A) : RGB(0x8C98A8), ALIGN_C, true, 0);
    snprintf(b, sizeof b, "pen %s    T_IRQ pin %s", r->pen ? "DOWN" : "up", irq ? "ACTIVE" : "idle");
    gfx_text(&font_m10, 160, 84, b, RGB(0xF3F6F9), ALIGN_C, true, 0);
    snprintf(b, sizeof b, "polls %lu  presses %lu  taps %lu  rail %lu  spi-refused %lu", (unsigned long)tc.polls, (unsigned long)tc.presses,
             (unsigned long)tc.taps, (unsigned long)tc.rejected_raw, (unsigned long)spi_bus_violations());
    gfx_text(&font_m7, 160, 104, b, RGB(0x8C98A8), ALIGN_C, true, 0);
    gfx_text(&font_m8, 160, 126, "all zero/constant = check T_DO, T_CS wiring", RGB(0xFFB224), ALIGN_C, true, 0);
    gfx_round_rect(60, 150, 260, 200, 10, RGB(0x0F2A22), true, RGB(0x2FD39A), 1.5f);
    gfx_text(&font_sb12, 160, 175, tc.cal.valid ? "RE-CALIBRATE" : "TAP = CALIBRATE", RGB(0x2FD39A), ALIGN_C, true, 1);
    present();
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);
    printf("[BRINGUP] CHIPLER P1_r3 display+touch bring-up image. Keys: r=rotate 180, s=status, h=help\n");
    lcd_init();
    gfx_init(fb);
    touch_hw_init();
    touch_core_init(&tc, LCD_WIDTH, LCD_HEIGHT);
    stage_bars();
    stage_geometry();
    int stage = 3;
    uint32_t last_print = 0, cal_step = 0, last_poll = 0, last_draw = 0;
    tch_pt_t tgt[5], rawp[5];
    touch_cal_targets(5, LCD_WIDTH, LCD_HEIGHT, tgt);
    float worst = 0;
    int16_t px = -1, py = -1;
    touch_raw_t raw = {0};
    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        int ch = getchar_timeout_us(0);
        if (ch == 'r') {
            rot180 = !rot180; lcd_set_rotation(rot180); touch_core_set_rotation(&tc, rot180); lcd_invalidate(); stage = 3;
            printf("[BRINGUP] rotation %s\n", rot180 ? "180" : "normal");
        } else if (ch == 's') {
            printf("[BRINGUP] stage=%d cal=%d polls=%lu presses=%lu taps=%lu rejected=%lu spi_refused=%lu\n", stage, tc.cal.valid, (unsigned long)tc.polls,
                   (unsigned long)tc.presses, (unsigned long)tc.taps, (unsigned long)tc.rejected_raw, (unsigned long)spi_bus_violations());
        } else if (ch == 'h') printf("[BRINGUP] r rotate, s status\n");
        if ((uint32_t)(now - last_poll) < 10) { sleep_us(300); continue; }
        last_poll = now;
        bool ok = touch_hw_read(&raw);
        tch_event_t ev = touch_core_feed(&tc, now, ok ? &raw : 0);
        bool irq = touch_hw_irq_active();
        if (raw.pen && (uint32_t)(now - last_print) >= 50) {
            last_print = now; printf("[TOUCH] raw x=%u y=%u z=%u irq=%d\n", raw.x, raw.y, raw.z, irq);
        }
        if (stage == 3) {
            if ((uint32_t)(now - last_draw) >= 100) { last_draw = now; draw_raw(&raw, irq); }
            if (ev.kind == TCH_EV_TAP) {
                bool start = true;   /* uncalibrated: the box cannot be hit-tested honestly, any tap starts calibration */
                int16_t sx, sy;
                if (tc.cal.valid && touch_core_map(&tc, tc.press_rx, tc.press_ry, &sx, &sy)) start = sx >= 60 && sx < 260 && sy >= 150 && sy < 200;
                if (start) { stage = 4; cal_step = 0; printf("[BRINGUP] stage 4: calibration, tap the 5 crosses\n"); }
            }
        } else if (stage == 4) {
            if ((uint32_t)(now - last_draw) >= 60) {
                last_draw = now;
                char b[48];
                gfx_clear(RGB(0x06080B));
                snprintf(b, sizeof b, "CALIBRATION  %u / 5", (unsigned)cal_step + 1);
                gfx_text(&font_sb12, 160, 100, b, RGB(0xF3F6F9), ALIGN_C, true, 1);
                gfx_text(&font_m8, 160, 122, "tap the centre of the cross", RGB(0x8C98A8), ALIGN_C, true, 0);
                float x = tgt[cal_step].x, y = tgt[cal_step].y;
                gfx_line(x - 12, y, x + 12, y, 2, RGB(0xFFB224)); gfx_line(x, y - 12, x, y + 12, 2, RGB(0xFFB224)); gfx_circle(x, y, 3, RGB(0xFFB224));
                present();
            }
            if (ev.kind == TCH_EV_TAP) {
                rawp[cal_step] = (tch_pt_t){(float)tc.press_rx, (float)tc.press_ry};
                printf("[BRINGUP] cal point %u raw=(%u,%u) target=(%.0f,%.0f)\n", (unsigned)cal_step + 1, tc.press_rx, tc.press_ry,
                       (double)tgt[cal_step].x, (double)tgt[cal_step].y);
                if (++cal_step >= 5) {
                    touch_cal_t c;
                    if (touch_cal_solve(rawp, tgt, 5, LCD_WIDTH, LCD_HEIGHT, rot180, &c, &worst)) {
                        touch_core_set_cal(&tc, &c); stage = 5; px = py = -1;
                        printf("[BRINGUP] calibration OK worst residual %.2f px  a=%.5f b=%.5f c=%.2f d=%.5f e=%.5f f=%.2f\n", (double)worst,
                               (double)c.a, (double)c.b, (double)c.c, (double)c.d, (double)c.e, (double)c.f);
                    } else {
                        printf("[BRINGUP] calibration REJECTED (worst %.2f px, limit %.1f) or degenerate: repeat\n", (double)worst, (double)TCH_CAL_MAX_ERR_PX);
                        cal_step = 0;
                    }
                }
            }
        } else {
            int16_t sx, sy;
            if (raw.pen && touch_core_map(&tc, raw.x, raw.y, &sx, &sy)) { px = sx; py = sy; }
            if ((uint32_t)(now - last_draw) >= 40) {
                last_draw = now;
                char b[64];
                gfx_clear(RGB(0x06080B));
                for (int i = 0; i < 5; i++) {
                    gfx_line(tgt[i].x - 6, tgt[i].y, tgt[i].x + 6, tgt[i].y, 1, RGB(0x4B5665));
                    gfx_line(tgt[i].x, tgt[i].y - 6, tgt[i].x, tgt[i].y + 6, 1, RGB(0x4B5665));
                }
                gfx_round_rect(214, 4, 316, 36, 8, RGB(0x2A2110), true, RGB(0xFFB224), 1.2f);
                gfx_text(&font_sb9, 265, 20, "RESTART CAL", RGB(0xFFB224), ALIGN_C, true, 0);
                snprintf(b, sizeof b, "PAINT  screen x=%d y=%d  worst fit %.1f px", px, py, (double)worst);
                gfx_text(&font_m8, 160, 120, b, RGB(0xF3F6F9), ALIGN_C, true, 0);
                gfx_text(&font_m7, 160, 136, "the crosshair must sit under the fingertip, also at the corners", RGB(0x8C98A8), ALIGN_C, true, 0);
                if (px >= 0) {
                    gfx_line(px - 14, py, px + 14, py, 1.5f, RGB(0x38D6F5)); gfx_line(px, py - 14, px, py + 14, 1.5f, RGB(0x38D6F5));
                    gfx_circle(px, py, 3, RGB(0x2FD39A));
                }
                present();
            }
            if (ev.kind == TCH_EV_TAP && ev.x >= 214 && ev.x < 316 && ev.y >= 4 && ev.y < 36) {
                touch_core_set_cal(&tc, 0); stage = 3; printf("[BRINGUP] calibration cleared\n");
            }
        }
    }
}
