/* SPI LCD driver. P1_r1 adds ILI9341 2.8" 320x240 (LCD_CONTROLLER 3); GC9A01/ST7789 kept.
 * Original header: SPI LCD driver for the 1.28" round 240x240 panel.
 * Controller: GC9A01 expected (supplier photo + module docs), ST7789 kept as fallback.
 * Both are UNVERIFIED on the owner's module until the bench test (gate S2) passes. */
#ifndef CHIPLER_LCD_H
#define CHIPLER_LCD_H
#include <stdint.h>
#include <stdbool.h>

void lcd_init(void);                        /* GPIO, SPI, reset, controller init */
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);  /* inclusive */
bool lcd_push(const uint16_t *px, uint32_t count);  /* bounded DMA (or CPU) push, RGB565 native order; false = timed out + recovered, panel half-written */
void lcd_set_push_mode(bool cpu);                   /* isolation switch: false = DMA (default), true = CPU register writes */
bool lcd_push_mode_cpu(void);
uint32_t lcd_push_budget_us(uint32_t count);        /* wait budget used for a push of `count` pixels */
void lcd_push_frame(const uint16_t *fb);            /* full 320x240 */
void lcd_fill(uint16_t c);
bool lcd_last_push_ok(void);   /* true only when the most recent lcd_push_frame_dirty() completed every band */
uint32_t lcd_push_frame_dirty(const uint16_t *fb, bool force_full);   /* returns rows sent */
void lcd_invalidate(void);                                              /* next dirty push sends everything */
void lcd_set_rotation(bool rot180);               /* MADCTL 0x28 <-> 0xE8 (ILI9341 landscape) */
void lcd_test_pattern(void);                         /* colour bars + border for bring-up */
uint32_t lcd_spi_actual_hz(void);
extern const char lcd_build_id[];
#endif
