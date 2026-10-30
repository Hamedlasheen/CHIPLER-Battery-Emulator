#include "lcd.h"
#include "board_config.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "spi_bus.h"
#include "diag/core1_diag.h"
#include "spi_regs.h"
#include <stdatomic.h>

/* V02-04: machine-checkable build identity (tools/verify_variants.py reads it from every ELF) */
#define LCD_STR_(x) #x
#define LCD_STR(x) LCD_STR_(x)
const char lcd_build_id[] = "CHIPLER-LCD-ID set=" LCD_SET_ID " ctrl=" LCD_STR(LCD_CONTROLLER)
                            " madctl=" LCD_STR(LCD_MADCTL) " inv=" LCD_STR(LCD_INVERT);

static int s_dma = -1;
static uint32_t s_hz;
static bool s_hash_valid;
static void s_hash_valid_clear(void) { s_hash_valid = false; }

static void cmd(uint8_t c) {
    if (!spi_bus_select(SPI_DEV_LCD)) return;
    gpio_put(PIN_LCD_DC, 0);
    gpio_put(PIN_LCD_CS, 0);
    spi_bus_write(&c, 1);               /* bounded; CS goes high only after the write finished or was recovered */
    gpio_put(PIN_LCD_CS, 1);
}

static void data(const uint8_t *d, size_t n) {
    if (!spi_bus_select(SPI_DEV_LCD)) return;
    gpio_put(PIN_LCD_DC, 1);
    gpio_put(PIN_LCD_CS, 0);
    spi_bus_write(d, n);
    gpio_put(PIN_LCD_CS, 1);
}

/* init table: cmd, nargs (0x80 flag = delay follows in ms), args... ; 0x00,0x00 ends */
#define DLY 0x80
#if LCD_CONTROLLER == 1
/* GC9A01 sequence as used by widely deployed open-source GC9A01 drivers (Waveshare
 * LCD_1in28 demo / Arduino_GFX). Registers 0xEB..0x98 are vendor-specific panel tuning;
 * kept verbatim. VERIFY on the owner's module (gate S2). */
static const uint8_t k_init[] = {
    0xEF, 0, 0xEB, 1, 0x14, 0xFE, 0, 0xEF, 0, 0xEB, 1, 0x14,
    0x84, 1, 0x40, 0x85, 1, 0xFF, 0x86, 1, 0xFF, 0x87, 1, 0xFF, 0x88, 1, 0x0A, 0x89, 1, 0x21,
    0x8A, 1, 0x00, 0x8B, 1, 0x80, 0x8C, 1, 0x01, 0x8D, 1, 0x01, 0x8E, 1, 0xFF, 0x8F, 1, 0xFF,
    0xB6, 2, 0x00, 0x20,
    0x36, 1, LCD_MADCTL,
    0x3A, 1, 0x05,                      /* 16-bit RGB565 */
    0x90, 4, 0x08, 0x08, 0x08, 0x08,
    0xBD, 1, 0x06, 0xBC, 1, 0x00,
    0xFF, 3, 0x60, 0x01, 0x04,
    0xC3, 1, 0x13, 0xC4, 1, 0x13, 0xC9, 1, 0x22, 0xBE, 1, 0x11,
    0xE1, 2, 0x10, 0x0E, 0xDF, 3, 0x21, 0x0C, 0x02,
    0xF0, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
    0xF1, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
    0xF2, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,
    0xF3, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,
    0xED, 2, 0x1B, 0x0B, 0xAE, 1, 0x77, 0xCD, 1, 0x63,
    0x70, 9, 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03,
    0xE8, 1, 0x34,
    0x62, 12, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70,
    0x63, 12, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70,
    0x64, 7, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07,
    0x66, 10, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00,
    0x67, 10, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98,
    0x74, 7, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00,
    0x98, 2, 0x3E, 0x07,
    0x35, 0,                            /* tearing effect line on */
#if LCD_INVERT
    0x21, 0,                            /* display inversion on (IPS panels) */
#else
    0x20, 0,
#endif
    0x11, DLY, 120,                     /* sleep out */
    0x29, DLY, 20,                      /* display on */
    0x00, 0x00
};
#elif LCD_CONTROLLER == 3
/* ILI9341 power-on sequence (vendor application-note values, as in widely used drivers).
 * UNVERIFIED on the owner's module: the BGR bit (0x08 in MADCTL) and inversion are bring-up knobs. */
static const uint8_t k_init[] = {
    0x01, DLY, 150,                     /* software reset */
    0xCF, 3, 0x00, 0xC1, 0x30,
    0xED, 4, 0x64, 0x03, 0x12, 0x81,
    0xE8, 3, 0x85, 0x00, 0x78,
    0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
    0xF7, 1, 0x20,
    0xEA, 2, 0x00, 0x00,
    0xC0, 1, 0x23,                      /* power control 1 */
    0xC1, 1, 0x10,                      /* power control 2 */
    0xC5, 2, 0x3E, 0x28,                /* VCOM 1 */
    0xC7, 1, 0x86,                      /* VCOM 2 */
    0x36, 1, LCD_MADCTL,
    0x3A, 1, 0x55,                      /* 16-bit RGB565 */
    0xB1, 2, 0x00, 0x18,                /* frame rate */
    0xB6, 3, 0x08, 0x82, 0x27,          /* display function control */
    0xF2, 1, 0x00,                      /* 3-gamma off */
    0x26, 1, 0x01,                      /* gamma curve 1 */
    0xE0, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
    0xE1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
#if LCD_INVERT
    0x21, 0,
#else
    0x20, 0,
#endif
    0x11, DLY, 120,                     /* sleep out */
    0x29, DLY, 20,                      /* display on */
    0x00, 0x00
};
#else
/* ST7789 fallback (generic) */
static const uint8_t k_init[] = {
    0x01, DLY, 150,                     /* software reset */
    0x11, DLY, 120,                     /* sleep out */
    0x3A, 1, 0x55,                      /* 16-bit */
    0x36, 1, LCD_MADCTL,
#if LCD_INVERT
    0x21, 0,
#else
    0x20, 0,
#endif
    0x13, DLY, 10,                      /* normal display on */
    0x29, DLY, 20,
    0x00, 0x00
};
#endif

static void run_init(const uint8_t *t) {
    while (!(t[0] == 0x00 && t[1] == 0x00)) {
        uint8_t c = t[0], n = t[1];
        t += 2;
        cmd(c);
        if (n & DLY) { sleep_ms(t[0]); t += 1; }
        else if (n) { data(t, n); t += n; }
    }
}

void lcd_init(void) {
    spi_bus_init();                      /* both CS inactive, SPI0 pins, owner core = caller */
    gpio_init(PIN_LCD_DC);  gpio_set_dir(PIN_LCD_DC, GPIO_OUT);  gpio_put(PIN_LCD_DC, 1);
    gpio_init(PIN_LCD_RST); gpio_set_dir(PIN_LCD_RST, GPIO_OUT); gpio_put(PIN_LCD_RST, 1);
    spi_bus_select(SPI_DEV_LCD);
    s_hz = spi_get_baudrate(LCD_SPI_INSTANCE);
    /* hardware reset */
    sleep_ms(10); gpio_put(PIN_LCD_RST, 0); sleep_ms(20); gpio_put(PIN_LCD_RST, 1); sleep_ms(120);
    run_init(k_init);
    s_dma = dma_claim_unused_channel(true);
    spi_bus_set_dma_channel(s_dma);
}

uint32_t lcd_spi_actual_hz(void) { return s_hz; }

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t ca[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    uint8_t ra[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    cmd(0x2A); data(ca, 4);
    cmd(0x2B); data(ra, 4);
    cmd(0x2C);
}

static atomic_int s_push_cpu;            /* 0 = DMA (default), 1 = CPU register writes (isolation switch, same GPIO map) */
void lcd_set_push_mode(bool cpu) { atomic_store_explicit(&s_push_cpu, cpu ? 1 : 0, memory_order_relaxed); }
bool lcd_push_mode_cpu(void) { return atomic_load_explicit(&s_push_cpu, memory_order_relaxed) != 0; }
uint32_t lcd_push_budget_us(uint32_t count) { return spi_xfer_budget_us(count * 16u, s_hz); }

bool lcd_push(const uint16_t *px, uint32_t count) {
    /* 16-bit SPI frames => RGB565 sent MSB first, as the panel expects. Every wait below is bounded by the wire time of
     * THIS transfer (a full 320x240 frame is ~131 ms at 9.375 MHz), never by a fixed constant. */
    if (!spi_bus_select(SPI_DEV_LCD)) return false;
    spi_set_format(LCD_SPI_INSTANCE, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_put(PIN_LCD_DC, 1);
    gpio_put(PIN_LCD_CS, 0);
    uint32_t budget = lcd_push_budget_us(count);
    spi_wait_t w = SPI_WAIT_OK;
    if (lcd_push_mode_cpu()) {
        c1d_stage(C1S_CPU_PUSH);
        uint32_t dl = time_us_32() + budget;
        for (uint32_t i = 0; i < count && w == SPI_WAIT_OK; i++) {
            while (!spi_is_writable(LCD_SPI_INSTANCE)) {
                if (spi_is_readable(LCD_SPI_INSTANCE)) (void)SPI_DR_READ();
                if ((int32_t)(time_us_32() - dl) >= 0) { w = spi_bus_recover(C1F_CPU_PUSH_TIMEOUT); break; }
                tight_loop_contents();
            }
            if (w != SPI_WAIT_OK) break;
            SPI_DR_WRITE(px[i]);
        }
        if (w == SPI_WAIT_OK) w = spi_bus_wait_idle(5000u);      /* tail: <= 8 frames still in the TX FIFO */
    } else {
        c1d_stage(C1S_DMA_START);
        dma_channel_config c = dma_channel_get_default_config(s_dma);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI_INSTANCE, true));
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        dma_channel_configure(s_dma, &c, &spi_get_hw(LCD_SPI_INSTANCE)->dr, px, count, true);
        w = spi_bus_wait_idle(budget);      /* DMA idle, then SPI shifter idle; on timeout: abort DMA, wait, drain, THEN CS */
    }
    spi_bus_release();                  /* CS high only after the transfer finished or was recovered */
    /* the DMA only feeds TX, but the PL022 pushed every received frame into its 8-deep RX FIFO (AG-LCD F1): drop them */
    c1d_stage(C1S_SPI_DRAIN);
    while (spi_is_readable(LCD_SPI_INSTANCE)) (void)SPI_DR_READ();
    spi_get_hw(LCD_SPI_INSTANCE)->icr = SPI_SSPICR_RORIC_BITS;
    c1d_stage(C1S_SPI_FORMAT);
    spi_set_format(LCD_SPI_INSTANCE, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    if (w != SPI_WAIT_OK) { s_hash_valid_clear(); return false; }
    return true;
}

void lcd_set_rotation(bool rot180) {
    uint8_t v = rot180 ? LCD_MADCTL_ROT180 : LCD_MADCTL;
    cmd(0x36); data(&v, 1);
}

void lcd_push_frame(const uint16_t *fb) {
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    lcd_push(fb, LCD_WIDTH * LCD_HEIGHT);
}

/* Push only the row bands that changed since the previous push. Row hashes (FNV-1a, 240 words) replace a
 * second framebuffer. First call (or after lcd_invalidate) pushes everything. A hash collision would skip
 * a changed row until the next change; the 1 s forced full refresh bounds that. */
static uint32_t s_row_hash[LCD_HEIGHT];
static uint32_t s_seen_recoveries;
void lcd_invalidate(void) { s_hash_valid = false; }
static volatile bool s_last_push_ok;
bool lcd_last_push_ok(void) { return s_last_push_ok; }
uint32_t lcd_push_frame_dirty(const uint16_t *fb, bool force_full) {
    uint32_t pushed_rows = 0;
    s_last_push_ok = false;
    uint32_t t0 = time_us_32();
    uint32_t h[LCD_HEIGHT];
    if (spi_bus_recoveries() != s_seen_recoveries) { s_seen_recoveries = spi_bus_recoveries(); s_hash_valid = false; }   /* a recovered transfer left the panel half-written */
    c1d_stage(C1S_PUSH_HASH);
    for (int y = 0; y < LCD_HEIGHT; y++) {
        const uint32_t *p = (const uint32_t *)(const void *)(fb + (size_t)y * LCD_WIDTH);
        uint32_t x = 2166136261u;
        for (int i = 0; i < LCD_WIDTH / 2; i++) { x ^= p[i]; x *= 16777619u; }
        h[y] = x;
    }
    int y = 0;
    while (y < LCD_HEIGHT) {
        if (s_hash_valid && !force_full && h[y] == s_row_hash[y]) { y++; continue; }
        int y0 = y;
        /* extend the band; bridge gaps of up to 3 unchanged rows (a window command costs ~ as much as 3 rows) */
        int last = y;
        for (int k = y + 1; k < LCD_HEIGHT && k <= last + 4; k++)
            if (!s_hash_valid || force_full || h[k] != s_row_hash[k]) last = k;
        lcd_set_window(0, (uint16_t)y0, LCD_WIDTH - 1, (uint16_t)last);
        if (!lcd_push(fb + (size_t)y0 * LCD_WIDTH, (uint32_t)(last - y0 + 1) * LCD_WIDTH)) {
            c1d_note_push(time_us_32() - t0, pushed_rows);
            return pushed_rows;      /* failed transfer: hashes stay invalid, next push resends everything */
        }
        pushed_rows += (uint32_t)(last - y0 + 1);
        y = last + 1;
        c1d_stage(C1S_PUSH_HASH);
    }
    for (int i = 0; i < LCD_HEIGHT; i++) s_row_hash[i] = h[i];
    s_hash_valid = true;
    s_last_push_ok = true;     /* every dirty band of this frame completed (062 R4: the splash timer starts only after this) */
    c1d_note_push(time_us_32() - t0, pushed_rows);
    return pushed_rows;
}

void lcd_fill(uint16_t c) {
    static uint16_t line[LCD_WIDTH];
    for (int i = 0; i < LCD_WIDTH; i++) line[i] = c;
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    for (int y = 0; y < LCD_HEIGHT; y++) lcd_push(line, LCD_WIDTH);
}

void lcd_test_pattern(void) {
    /* 8 vertical bars: white, yellow, cyan, green, magenta, red, blue, black.
     * If red/blue appear swapped -> toggle LCD_MADCTL BGR bit. If colours look negative ->
     * toggle LCD_INVERT. Record the result in the bench report. */
    static const uint16_t bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
    static uint16_t line[LCD_WIDTH];
    for (int x = 0; x < LCD_WIDTH; x++) line[x] = bars[x / (LCD_WIDTH / 8)];
    lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    for (int y = 0; y < LCD_HEIGHT; y++) lcd_push(line, LCD_WIDTH);
}
