/*
 * Gate S2 display + three-button (UP / DOWN / OK) bring-up (no phone, no power path, no INA228 needed).
 * Sequence (see docs/ar/03_FIRST_POWER_TEST_AR.md):
 *   1) 3 s colour bars: white, yellow, cyan, green, magenta, red, blue, black (left -> right)
 *   2) 4 s geometry screen: thin circle at radius 118 + "TOP"/"LEFT"/"RIGHT" labels + red dot
 *      near the top-left -> checks offset, rotation and mirroring
 *   3) live screen: variant name, last button gesture, A/B raw state, render/push timing
 * Each prebuilt variant (GC9A01 BGR/RGB, inversion on/off, ST7789) shows its name on screen and
 * prints it on USB serial, so the owner can record which one is correct.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "board_config.h"
#include "hal/lcd.h"
#include "hal/board_io.h"
#include "gfx/gfx.h"
#include "gfx/fonts_data.h"
#include "input/buttons.h"


static uint16_t fb[GFX_W * GFX_H];

static void geometry_screen(void) {
    gfx_clear(RGB(0x000000));
    gfx_arc(119, 1.5f, 0, 360, RGB(0xFFFFFF), false);       /* should touch the glass edge evenly */
    gfx_line(120, 6, 120, 234, 1, RGB(0x303030));
    gfx_line(6, 120, 234, 120, 1, RGB(0x303030));
    gfx_text(&font_sb12, 120, 30, "TOP", RGB(0xFFFFFF), ALIGN_C, true, 1.0f);
    gfx_triangle(120, 44, 126, 54, 114, 54, RGB(0xFFFFFF));
    gfx_text(&font_sb9, 26, 120, "LEFT", RGB(0x38D6F5), ALIGN_L, true, 0.5f);
    gfx_text(&font_sb9, 214, 120, "RIGHT", RGB(0xFFB224), ALIGN_R, true, 0.5f);
    gfx_circle(66, 66, 6, RGB(0xFF0000));                   /* red dot = top-left quadrant */
    gfx_text(&font_m8, 120, 160, "red dot must be TOP-LEFT", RGB(0xC0C0C0), ALIGN_C, true, 0);
    gfx_text(&font_m8, 120, 174, "SET " LCD_SET_ID " \x80 " DISPLAY_VARIANT, RGB(0xC0C0C0), ALIGN_C, true, 0);
    gfx_mask_round();
    lcd_push_frame(fb);
}

int main(void) {
    board_io_init();                 /* phone power path (if ever wired) forced OFF */
    stdio_init_all();
    lcd_init();
    lcd_test_pattern();
    printf("[LCD] %s\n", lcd_build_id);
    printf("[LCD] set=%s variant=%s controller=%s spi=%lu Hz madctl=0x%02X invert=%d\n", LCD_SET_ID, DISPLAY_VARIANT,
           LCD_CONTROLLER == 1 ? "GC9A01" : "ST7789", (unsigned long)lcd_spi_actual_hz(), LCD_MADCTL, LCD_INVERT);
    sleep_ms(3000);
    gfx_init(fb);
    geometry_screen();
    sleep_ms(4000);
    buttons_t b; buttons_init(&b, 0);
    char last[24] = "PRESS UP/DOWN/OK";
    uint32_t frames = 0, t_render = 0, t_push = 0;
    while (true) {
        for (int i = 0; i < 50; i++) {
            uint32_t now = to_ms_since_boot(get_absolute_time());
            btn_event_t e = buttons_update(&b, now, btn_up_pressed(), btn_down_pressed(), btn_ok_pressed());
            if (e != BTN_EV_NONE) { snprintf(last, sizeof last, "%s", btn_event_name(e)); printf("[BTN] %s\n", last); }
            sleep_ms(1);
        }
        uint32_t t0 = time_us_32();
        gfx_clear(RGB(0x06080B));
        gfx_arc(114, 6, 135, 405, RGB(0x141B24), true);
        gfx_arc(114, 6, 135, 135 + 270 * ((frames % 60) / 60.0f), RGB(0xFFB224), true);
        gfx_text(&font_sb9, 120, 36, "CHIPLER DISPLAY TEST", RGB(0x8C98A8), ALIGN_C, true, 1.0f);
        gfx_text(&font_m7, 120, 50, "SET " LCD_SET_ID " \x80 " DISPLAY_VARIANT, RGB(0x38D6F5), ALIGN_C, true, 0.6f);
        gfx_text(&font_d40, 120, 94, "4.012", RGB(0xF3F6F9), ALIGN_C, true, 0);
        gfx_text(&font_sb12, 120, 136, last, RGB(0x38D6F5), ALIGN_C, true, 0);
        {
            static const char *nm[3] = {"UP", "DOWN", "OK"};
            bool st[3] = {btn_up_pressed(), btn_down_pressed(), btn_ok_pressed()};
            for (int k = 0; k < 3; k++) {
                float x = 76 + k * 44;
                gfx_round_rect(x - 19, 152, x + 19, 172, 6, st[k] ? RGB(0x0F2A22) : RGB(0x0E131A), true,
                               st[k] ? RGB(0x2FD39A) : RGB(0x1B232E), 1.2f);
                gfx_text(&font_sb8, x, 162, nm[k], st[k] ? RGB(0x2FD39A) : RGB(0x8C98A8), ALIGN_C, true, 0);
            }
        }
        char s[40]; snprintf(s, sizeof s, "render %lu us  push %lu us", (unsigned long)t_render, (unsigned long)t_push);
        gfx_text(&font_m6, 120, 200, s, RGB(0x4B5665), ALIGN_C, true, 0);
        gfx_mask_round();
        uint32_t t1 = time_us_32();
        lcd_push_frame(fb);
        uint32_t t2 = time_us_32();
        t_render = t1 - t0; t_push = t2 - t1; frames++;
        if (frames % 20 == 0) printf("[LCD] render=%lu us push=%lu us\n", (unsigned long)t_render, (unsigned long)t_push);
    }
}
