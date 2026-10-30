/* AG-LCD: ILI9341 driver byte-sequence test (host, fake SDK in fake_lcd/).
 *
 * Compiles the PRODUCTION src/hal/lcd.c + src/hal/spi_bus.c + src/input/touch_hw.c against a fake SDK that
 * records every SPI frame with the DC / CS levels at the time it was shifted, then asserts:
 *   G1 lcd_init(): exact command/parameter byte sequence (hand-written below from the datasheet review,
 *      NOT copied from k_init), reset / SWRESET / SLPOUT delays vs ILI9341 V1.05 minimums, CS discipline.
 *   G2 documented parameter bit fields (fixed bits) for every non-vendor register; vendor-register
 *      deviations from the V1.05 bit layout are printed as INFO only.
 *   G3 lcd_set_window(): CASET/PASET/RAMWR bytes, inclusive end coordinates.
 *   G4 lcd_push_frame_dirty(): first push = full frame, 2 changed rows = only that band, gap bridging,
 *      no-change = no traffic, force_full; pixel words are 16-bit frames, DC=1, MSB first values.
 *   G5 lcd_set_rotation(): MADCTL 0x28 / 0xE8.
 *   G6 shared bus: a touch read right after a display DMA push must return the controller's values
 *      (fails on the unpatched driver: stale RX FIFO frames, see REPORT.md finding F1).
 */
#include <stdio.h>
#include <string.h>
#include "board_config.h"
#include "fake_lcd.h"
#include "lcd.h"
#include "spi_bus.h"
#include "touch_hw.h"
#include "diag/core1_diag.h"
#include "pico/stdlib.h"

static unsigned checks, fails, group_fails[10];
static int cur_group;
#define CHECK(c) do { checks++; if (!(c)) { fails++; group_fails[cur_group]++; printf("  FAIL G%d line %d: %s\n", cur_group, __LINE__, #c); } } while (0)

/* ---- token view of the LCD stream: only frames shifted while LCD CS is low ---- */
enum { T_CMD = 1, T_DAT = 2, T_PIX = 3, T_BAD = 4 };
#define TOK(k, v) (((uint32_t)(k) << 16) | (uint16_t)(v))
#define C(x) TOK(T_CMD, x)
#define D(x) TOK(T_DAT, x)
static uint32_t tok[200000]; static size_t ntok; static size_t tok_frame[200000];
static unsigned both_cs_low, lost_frames;
static void tokenize(void) {
    ntok = 0; both_cs_low = 0; lost_frames = 0;
    for (size_t i = 0; i < fl_nframes; i++) {
        const fl_frame_t *f = &fl_frames[i];
        if (f->lcd_cs == 0 && f->touch_cs == 0) both_cs_low++;
        if (f->lcd_cs != 0) { if (f->touch_cs != 0) lost_frames++; continue; }
        uint32_t t;
        if (f->bits == 8 && f->dc == 0 && !f->via_dma) t = C(f->v);
        else if (f->bits == 8 && f->dc == 1) t = D(f->v);
        else if (f->bits == 16 && f->dc == 1 && f->via_dma) t = TOK(T_PIX, f->v);
        else t = TOK(T_BAD, f->v);
        if (ntok < sizeof tok / sizeof tok[0]) { tok_frame[ntok] = i; tok[ntok++] = t; }
    }
}
static size_t find_cmd(uint8_t c, size_t from) { for (size_t i = from; i < ntok; i++) if (tok[i] == C(c)) return i; return (size_t)-1; }
static int match_at(size_t at, const uint32_t *exp, size_t n) {
    if (at + n > ntok) { printf("    stream too short: have %zu tokens, need %zu\n", ntok - at, n); return 0; }
    for (size_t i = 0; i < n; i++) if (tok[at + i] != exp[i]) {
        printf("    mismatch at token %zu: got kind %u val 0x%02X, expected kind %u val 0x%02X\n", at + i,
               (unsigned)(tok[at + i] >> 16), (unsigned)(tok[at + i] & 0xFFFF), (unsigned)(exp[i] >> 16), (unsigned)(exp[i] & 0xFFFF));
        return 0;
    }
    return 1;
}

/* ---- G1 expected init sequence (ILI9341 branch, LCD_MADCTL 0x28, LCD_INVERT 0) ---- */
static const uint32_t k_expect_init[] = {
    C(0x01),
    C(0xCF), D(0x00), D(0xC1), D(0x30),
    C(0xED), D(0x64), D(0x03), D(0x12), D(0x81),
    C(0xE8), D(0x85), D(0x00), D(0x78),
    C(0xCB), D(0x39), D(0x2C), D(0x00), D(0x34), D(0x02),
    C(0xF7), D(0x20),
    C(0xEA), D(0x00), D(0x00),
    C(0xC0), D(0x23),
    C(0xC1), D(0x10),
    C(0xC5), D(0x3E), D(0x28),
    C(0xC7), D(0x86),
    C(0x36), D(0x28),
    C(0x3A), D(0x55),
    C(0xB1), D(0x00), D(0x18),
    C(0xB6), D(0x08), D(0x82), D(0x27),
    C(0xF2), D(0x00),
    C(0x26), D(0x01),
    C(0xE0), D(0x0F), D(0x31), D(0x2B), D(0x0C), D(0x0E), D(0x08), D(0x4E), D(0xF1), D(0x37), D(0x07), D(0x10), D(0x03), D(0x0E), D(0x09), D(0x00),
    C(0xE1), D(0x00), D(0x0E), D(0x14), D(0x03), D(0x11), D(0x07), D(0x31), D(0xC1), D(0x48), D(0x08), D(0x0F), D(0x0C), D(0x31), D(0x36), D(0x0F),
    C(0x20),
    C(0x11),
    C(0x29),
};

/* ---- G2 documented fixed bits per parameter (ILI9341 V1.05 command tables; mask = fixed bits, val = required) ---- */
typedef struct { uint8_t cmd; uint8_t n; uint8_t vendor; uint8_t mask[15], val[15]; const char *ref; } field_t;
static const field_t k_fields[] = {
    /* level 1 / level 2 */
    {0x36, 1, 0, {0x03}, {0x00}, "8.2.29 p127: MY MX MV ML BGR MH 0 0"},
    {0x3A, 1, 0, {0x88}, {0x00}, "8.2.33 p134: 0 DPI[2:0] 0 DBI[2:0]"},
    {0x26, 1, 0, {0xFF}, {0x01}, "8.2.17 p107: GC=01h gamma curve 1"},
    {0xB1, 2, 0, {0xFC, 0xE0}, {0x00, 0x00}, "8.3.2 p155: 000000 DIVA / 000 RTNA"},
    {0xB6, 3, 0, {0xF0, 0x00, 0xC0}, {0x00, 0x00, 0x00}, "8.3.7 p164: 0000 PTG PT / REV GS SS SM ISC / 00 NL"},
    {0xC0, 1, 0, {0xC0}, {0x00}, "8.3.16 p178: 00 VRH[5:0]"},
    {0xC1, 1, 0, {0xF8}, {0x10}, "8.3.17 p179: 0 0 0 1 0 BT[2:0]"},
    {0xC5, 2, 0, {0x80, 0x80}, {0x00, 0x00}, "8.3.18 p180: 0 VMH / 0 VML"},
    {0xC7, 1, 0, {0x00}, {0x00}, "8.3.19 p182: nVM VMF[6:0]"},
    {0xE0, 15, 0, {0xF0,0xC0,0xC0,0xF0,0xE0,0xF0,0x80,0x00,0x80,0xF0,0xE0,0xF0,0xC0,0xC0,0xF0}, {0}, "8.3.24 p188 PGAMCTRL field widths"},
    {0xE1, 15, 0, {0xF0,0xC0,0xC0,0xF0,0xE0,0xF0,0x80,0x00,0x80,0xF0,0xE0,0xF0,0xC0,0xC0,0xF0}, {0}, "8.3.25 p189 NGAMCTRL field widths"},
    {0xCB, 5, 0, {0xFF, 0xFF, 0xFF, 0xF8, 0xF8}, {0x39, 0x2C, 0x00, 0x30, 0x00}, "8.4.1 p195: 39 2C 00 00110 REG_VD / 00000 VBC"},
    {0xF7, 1, 0, {0x0F}, {0x00}, "8.4.7 p201: X X Ratio[1:0] 0000"},
    {0xEA, 2, 0, {0x00, 0x03}, {0x00, 0x00}, "8.4.4 p198: VG_SW_T4..T1 / XXXXXX 00"},
    /* vendor-tuning registers whose app-note values do not follow the V1.05 fixed-bit layout -> INFO only */
    {0xCF, 3, 1, {0xFF, 0xE7, 0xEF}, {0x00, 0xA2, 0xE0}, "8.4.2 p196: 00 / 1 0 1 PC[1:0] 0 1 0 / 1 1 1 DC_ena 0000"},
    {0xED, 4, 1, {0x44, 0x44, 0x44, 0x7F}, {0x44, 0x44, 0x44, 0x01}, "8.4.5 p199: X1xxX1xx x3 / DDVDH_ENH 0000001"},
    {0xE8, 3, 1, {0xFE, 0xEF, 0xFC}, {0x84, 0x01, 0x78}, "8.4.3 p197: 1000010 NOW / 000 EQ 0001 / 011110 PC"},
    {0xF2, 1, 1, {0xFE}, {0x02}, "8.4.6 p200: 0000001 3G_enb"},
};

static void check_fields(void) {
    for (size_t k = 0; k < sizeof k_fields / sizeof k_fields[0]; k++) {
        const field_t *f = &k_fields[k];
        size_t at = find_cmd(f->cmd, 0);
        CHECK(at != (size_t)-1);
        if (at == (size_t)-1) continue;
        for (unsigned p = 0; p < f->n; p++) {
            uint32_t t = tok[at + 1 + p];
            CHECK((t >> 16) == T_DAT);
            uint8_t v = (uint8_t)t;
            int ok = (v & f->mask[p]) == f->val[p];
            if (f->vendor) {
                if (!ok) printf("  INFO vendor reg 0x%02X param %u = 0x%02X: fixed bits 0x%02X expected 0x%02X per %s\n",
                                f->cmd, p + 1, v, (unsigned)(v & f->mask[p]), f->val[p], f->ref);
            } else {
                if (!ok) printf("    0x%02X param %u = 0x%02X violates %s\n", f->cmd, p + 1, v, f->ref);
                CHECK(ok);
            }
        }
        /* the next token after the documented parameters must be a command (no surplus parameters) */
        if (at + 1 + f->n < ntok) CHECK((tok[at + 1 + f->n] >> 16) == T_CMD);
    }
    /* semantic spot checks of the chosen values (datasheet tables) */
    size_t a;
    a = find_cmd(0x3A, 0); CHECK(a != (size_t)-1 && ((tok[a + 1] >> 4) & 7) == 5 && (tok[a + 1] & 7) == 5);   /* DPI=DBI=101: 16 bpp */
    a = find_cmd(0xB6, 0); CHECK(a != (size_t)-1 && (tok[a + 3] & 0x3F) == 0x27);                             /* NL=100111: 320 lines */
    a = find_cmd(0x36, 0); CHECK(a != (size_t)-1 && (tok[a + 1] & 0xFF) == 0x28);                             /* MV | BGR */
}

static uint64_t t_of_tok(size_t i) { return fl_frames[tok_frame[i]].t_us; }

static void g1_init(void) {
    cur_group = 1; printf("G1 lcd_init sequence + timing\n");
    fl_reset_log();
    lcd_init();
    tokenize();
    CHECK(match_at(0, k_expect_init, sizeof k_expect_init / sizeof k_expect_init[0]));
    CHECK(ntok == sizeof k_expect_init / sizeof k_expect_init[0]);      /* nothing else sent */
    CHECK(both_cs_low == 0 && lost_frames == 0 && fl_cs_while_busy == 0 && fl_fmt_while_busy == 0);
    /* hardware reset: RST high -> low (>= 10 us, tRW p225) -> high; first command >= 5 ms after (note 7 p225) */
    CHECK(fl_nrst >= 3);
    int lo = -1, hi = -1;
    for (int i = 0; i < fl_nrst; i++) { if (fl_rst[i].level == 0) lo = i; if (lo >= 0 && i > lo && fl_rst[i].level == 1) { hi = i; break; } }
    CHECK(lo >= 0 && hi > lo);
    if (lo >= 0 && hi > lo) {
        CHECK(fl_rst[hi].t_us - fl_rst[lo].t_us >= 10);
        CHECK(t_of_tok(0) - fl_rst[hi].t_us >= 5000);
        size_t slp = find_cmd(0x11, 0);
        CHECK(slp != (size_t)-1 && t_of_tok(slp) - fl_rst[hi].t_us >= 120000);   /* SLPOUT not within 120 ms of RESX */
    }
    /* SWRESET: 5 ms before next command (p90); 120 ms before SLPOUT (p90, worst case) */
    CHECK(ntok > 1 && t_of_tok(1) - t_of_tok(0) >= 5000);
    size_t slp = find_cmd(0x11, 0), on = find_cmd(0x29, 0);
    CHECK(slp != (size_t)-1 && t_of_tok(slp) - t_of_tok(0) >= 120000);
    /* SLPOUT: 5 ms before the next command (p101); module needs 120 ms to reach Sleep Out */
    CHECK(on != (size_t)-1 && slp != (size_t)-1 && t_of_tok(on) - t_of_tok(slp) >= 5000);
    printf("  timing: RST low %llu us, RST->SWRESET %llu ms, SWRESET->next %llu ms, SLPOUT->DISPON %llu ms\n",
           (unsigned long long)(hi > lo && lo >= 0 ? fl_rst[hi].t_us - fl_rst[lo].t_us : 0),
           (unsigned long long)(hi >= 0 ? (t_of_tok(0) - fl_rst[hi].t_us) / 1000 : 0),
           (unsigned long long)((t_of_tok(1) - t_of_tok(0)) / 1000), (unsigned long long)((t_of_tok(on) - t_of_tok(slp)) / 1000));
    /* SPI clock actually programmed by the production path (LCD_SPI_HZ -> spi_bus_select -> SDK divider):
     * datasheet 4-line write cycle twc >= 100 ns (p237) => <= 10 MHz; 150 MHz clk_peri gives 9.375 MHz */
    CHECK(lcd_spi_actual_hz() <= 10000000u);
    CHECK(lcd_spi_actual_hz() == 9375000u);
    printf("  INFO SPI write clock %u Hz = %u ns cycle; ILI9341 V1.05 twc min 100 ns (10 MHz) -> %s\n",
           lcd_spi_actual_hz(), 1000000000u / lcd_spi_actual_hz(), lcd_spi_actual_hz() > 10000000u ? "OUT OF DATASHEET SPEC" : "within spec");
    cur_group = 2; printf("G2 documented parameter bit fields\n");
    check_fields();
}

static void g3_window(void) {
    cur_group = 3; printf("G3 lcd_set_window\n");
    fl_reset_log(); lcd_set_window(10, 20, 300, 230); tokenize();
    const uint32_t e1[] = { C(0x2A), D(0x00), D(0x0A), D(0x01), D(0x2C), C(0x2B), D(0x00), D(0x14), D(0x00), D(0xE6), C(0x2C) };
    CHECK(match_at(0, e1, 11) && ntok == 11);
    fl_reset_log(); lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1); tokenize();
    /* MV=1: columns 0..013Fh, pages 0..00EFh (8.2.20 note 1 p110, 8.2.21 note 1 p112) */
    const uint32_t e2[] = { C(0x2A), D(0x00), D(0x00), D(0x01), D(0x3F), C(0x2B), D(0x00), D(0x00), D(0x00), D(0xEF), C(0x2C) };
    CHECK(match_at(0, e2, 11) && ntok == 11);
    CHECK(both_cs_low == 0 && lost_frames == 0 && fl_cs_while_busy == 0);
}

static uint16_t fb[LCD_WIDTH * LCD_HEIGHT] __attribute__((aligned(4)));

/* expects: one window per band then the band's pixels as 16-bit DMA frames */
static int expect_bands(const int (*bands)[2], int nb) {
    size_t at = 0; int ok = 1;
    for (int b = 0; b < nb; b++) {
        int y0 = bands[b][0], y1 = bands[b][1];
        const uint32_t w[] = { C(0x2A), D(0x00), D(0x00), D(0x01), D(0x3F),
                               C(0x2B), D(y0 >> 8), D(y0 & 0xFF), D(y1 >> 8), D(y1 & 0xFF), C(0x2C) };
        if (!match_at(at, w, 11)) return 0;
        at += 11;
        size_t n = (size_t)(y1 - y0 + 1) * LCD_WIDTH;
        if (at + n > ntok) { printf("    band %d: only %zu pixel tokens\n", b, ntok - at); return 0; }
        for (size_t i = 0; i < n; i++) if (tok[at + i] != TOK(T_PIX, fb[(size_t)y0 * LCD_WIDTH + i])) {
            printf("    band %d pixel %zu: got 0x%05X want 0x%05X\n", b, i, (unsigned)tok[at + i], (unsigned)TOK(T_PIX, fb[(size_t)y0 * LCD_WIDTH + i])); ok = 0; break; }
        at += n;
    }
    if (at != ntok) { printf("    %zu surplus tokens\n", ntok - at); ok = 0; }
    return ok;
}

static void g4_dirty(void) {
    cur_group = 4; printf("G4 lcd_push_frame_dirty\n");
    for (size_t i = 0; i < (size_t)LCD_WIDTH * LCD_HEIGHT; i++) fb[i] = (uint16_t)((i * 2654435761u) >> 13);
    fl_reset_log(); lcd_invalidate();
    uint32_t r = lcd_push_frame_dirty(fb, false); tokenize();
    { const int b[][2] = {{0, 239}}; CHECK(r == 240 && expect_bands(b, 1)); }
    CHECK(lcd_last_push_ok());                       /* 062 R4: the splash timer is armed only from this acknowledgement */
    CHECK(both_cs_low == 0 && lost_frames == 0 && fl_cs_while_busy == 0 && fl_fmt_while_busy == 0 && fl_dma_errors == 0);

    fb[100 * LCD_WIDTH + 7] ^= 0x1234; fb[101 * LCD_WIDTH + 319] ^= 0x00FF;              /* change 2 rows */
    fl_reset_log(); r = lcd_push_frame_dirty(fb, false); tokenize();
    { const int b[][2] = {{100, 101}}; CHECK(r == 2 && expect_bands(b, 1)); }

    fl_reset_log(); r = lcd_push_frame_dirty(fb, false); tokenize();                       /* nothing changed */
    CHECK(r == 0 && fl_nframes == 0);
    CHECK(lcd_last_push_ok());                       /* an unchanged frame needs no transfer and is not a failure */

    fb[50 * LCD_WIDTH] ^= 1; fb[54 * LCD_WIDTH] ^= 1;                                      /* gap of 3 rows: bridged */
    fl_reset_log(); r = lcd_push_frame_dirty(fb, false); tokenize();
    { const int b[][2] = {{50, 54}}; CHECK(r == 5 && expect_bands(b, 1)); }

    fb[60 * LCD_WIDTH] ^= 1; fb[65 * LCD_WIDTH] ^= 1; fb[239 * LCD_WIDTH + 319] ^= 1;     /* gap of 4: two windows; last row */
    fl_reset_log(); r = lcd_push_frame_dirty(fb, false); tokenize();
    { const int b[][2] = {{60, 60}, {65, 65}, {239, 239}}; CHECK(r == 3 && expect_bands(b, 3)); }

    fl_reset_log(); r = lcd_push_frame_dirty(fb, true); tokenize();                        /* forced full */
    { const int b[][2] = {{0, 239}}; CHECK(r == 240 && expect_bands(b, 1)); }
    CHECK(fl_dma_errors == 0 && fl_cs_while_busy == 0);
    /* after a push the bus is back to 8-bit frames: a following command must be a single 8-bit DC=0 frame */
    fl_reset_log(); lcd_set_rotation(false); tokenize();
    { const uint32_t e[] = { C(0x36), D(0x28) }; CHECK(match_at(0, e, 2) && ntok == 2); }
}

static void g5_rotation(void) {
    cur_group = 5; printf("G5 lcd_set_rotation\n");
    fl_reset_log(); lcd_set_rotation(true); tokenize();
    { const uint32_t e[] = { C(0x36), D(0xE8) }; CHECK(match_at(0, e, 2) && ntok == 2); }   /* MY|MX|MV|BGR */
    fl_reset_log(); lcd_set_rotation(false); tokenize();
    { const uint32_t e[] = { C(0x36), D(0x28) }; CHECK(match_at(0, e, 2) && ntok == 2); }   /* MV|BGR */
    CHECK((LCD_MADCTL ^ LCD_MADCTL_ROT180) == 0xC0);    /* 180 deg = flip MY and MX only, MV/BGR unchanged */
}

static void g6_shared_bus(void) {
    cur_group = 6; printf("G6 touch read after display DMA push (shared SPI0)\n");
    fl_xpt_set(1000, 500, 1234, 2345);
    touch_raw_t r;
    /* control: last LCD operation was spi_write_blocking (drains RX) */
    lcd_set_rotation(false);
    fl_reset_log(); CHECK(touch_hw_read(&r));
    printf("  after LCD command : pen=%d x=%u y=%u z=%u cs_while_busy=%u\n", r.pen, r.x, r.y, r.z, fl_cs_while_busy);
    CHECK(r.pen && r.x == 1234 && r.y == 2345 && r.z == 4595 && fl_cs_while_busy == 0);
    /* real main-loop order: frame push (ends with a DMA transfer) then the next touch poll */
    lcd_push_frame_dirty(fb, true);
    printf("  RX FIFO level after lcd_push: %u\n", fl_rx_level());
    fl_reset_log(); CHECK(touch_hw_read(&r));
    printf("  after DMA push    : pen=%d x=%u y=%u z=%u cs_while_busy=%u\n", r.pen, r.x, r.y, r.z, fl_cs_while_busy);
    CHECK(r.pen && r.x == 1234 && r.y == 2345 && r.z == 4595);
    CHECK(fl_cs_while_busy == 0);                         /* T_CS must not rise while bytes are still shifting */
    fl_reset_log(); CHECK(touch_hw_read(&r));            /* the next poll, with no LCD traffic in between */
    printf("  following poll    : pen=%d x=%u y=%u z=%u cs_while_busy=%u\n", r.pen, r.x, r.y, r.z, fl_cs_while_busy);
    CHECK(r.pen && r.x == 1234 && r.y == 2345 && r.z == 4595 && fl_cs_while_busy == 0);
    /* no finger on the panel (z1 = 0): a frame push must not turn into a phantom press.
     * Depends on the idle MISO level during the DMA push; the fake assumes MISO high (0xFF / 0xFFFF). */
    fl_xpt_set(0, 4095, 0, 0);
    lcd_push_frame_dirty(fb, true);
    fl_reset_log(); CHECK(touch_hw_read(&r));
    printf("  no touch, after DMA push: pen=%d x=%u y=%u z=%u\n", r.pen, r.x, r.y, r.z);
    CHECK(!r.pen);
    fl_reset_log(); CHECK(touch_hw_read(&r));
    printf("  no touch, following poll: pen=%d x=%u y=%u z=%u\n", r.pen, r.x, r.y, r.z);
    CHECK(!r.pen);
}

/* P1_r10: bounded waits + recovery order + core-1 diagnostics, against the PRODUCTION lcd.c / spi_bus.c / touch_hw.c. */
static c1d_snapshot_t snap(void) { c1d_snapshot_t d; memset(&d, 0, sizeof d); c1d_read(&d); return d; }
static void g7_hang_handling(void) {
    cur_group = 7; printf("G7 bounded waits, recovery order, no silent hang\n");
    touch_raw_t r;
    c1d_init();
    /* budget follows the transfer: full frame ~131 ms on the wire, never a fixed 100 ms */
    uint32_t full = lcd_push_budget_us((uint32_t)LCD_WIDTH * LCD_HEIGHT);
    printf("  full-frame wait budget %u us (wire time %u us)\n", full, (unsigned)((uint64_t)LCD_WIDTH * LCD_HEIGHT * 16u * 1000000u / lcd_spi_actual_hz()));
    CHECK(full >= 2u * 131072u && full < 1000000u);
    CHECK(lcd_push_budget_us(8) < 10000u);

    /* (a) DMA never finishes */
    for (size_t i = 0; i < (size_t)LCD_WIDTH * LCD_HEIGHT; i++) fb[i] = (uint16_t)(i * 7u + 3u);
    fl_reset_log(); lcd_invalidate();
    c1d_snapshot_t a0 = snap();
    fl_inject_dma_stuck = 1;
    uint32_t r1 = lcd_push_frame_dirty(fb, true);
    c1d_snapshot_t a1 = snap();
    printf("  DMA stuck: rows=%u aborts=%u cs_before_abort=%u tmo_dma=%u recoveries=%u fault=%s in %s\n", r1, fl_dma_aborts,
           fl_order_cs_before_abort, a1.tmo_dma, a1.recoveries, c1d_fault_name(a1.fault_kind), c1d_stage_name(a1.fault_stage));
    CHECK(r1 == 0);                                                   /* failed transfer is not reported as sent */
    CHECK(!lcd_last_push_ok());                                       /* 062 R4: a failed splash push never arms the splash timer */
    CHECK(fl_dma_aborts == 1 && fl_order_cs_before_abort == 0);       /* DMA aborted first, CS raised only afterwards */
    CHECK(a1.tmo_dma == a0.tmo_dma + 1 && a1.recoveries == a0.recoveries + 1);
    CHECK(a1.fault_kind == C1F_DMA_TIMEOUT && a1.fault_stage == C1S_DMA_WAIT && !a1.bus_failed && !spi_bus_failed());
    fl_reset_log();
    uint32_t r2 = lcd_push_frame_dirty(fb, false);                    /* panel was half-written: full frame again, normally */
    { const int b[][2] = {{0, 239}}; tokenize(); CHECK(r2 == 240 && expect_bands(b, 1)); }
    CHECK(lcd_last_push_ok());
    CHECK(fl_order_cs_before_abort == 0 && fl_dma_aborts == 0);

    /* (b) touch exchange never answers (dead MISO / disabled block): bounded, T_CS released only after recovery */
    fl_xpt_set(1000, 500, 1234, 2345);
    fl_reset_log(); fl_inject_rx_never = 1;
    c1d_snapshot_t b0 = snap();
    CHECK(!touch_hw_read(&r));
    c1d_snapshot_t b1 = snap();
    CHECK(b1.tmo_touch == b0.tmo_touch + 1 && b1.fault_kind == C1F_TOUCH_TIMEOUT && b1.fault_stage == C1S_TOUCH_XFER && !spi_bus_failed());
    fl_reset_log(); CHECK(touch_hw_read(&r) && r.pen && r.x == 1234);   /* works again once the line answers */

    /* (c) CPU push mode (isolation switch): same GPIO map, pixels sent by register writes, not by DMA */
    fl_reset_log(); lcd_set_push_mode(true); CHECK(lcd_push_mode_cpu());
    fb[5] ^= 0x00F0;
    uint32_t r3 = lcd_push_frame_dirty(fb, true);
    { int dma_frames = 0, pix16 = 0;
      for (size_t i = 0; i < fl_nframes; i++) { dma_frames += fl_frames[i].via_dma; if (fl_frames[i].bits == 16) { pix16++; fl_frames[i].via_dma = 1; } }   /* tag as pixels for the tokenizer */
      printf("  CPU push: rows=%u pixel frames=%d dma frames=%d\n", r3, pix16, dma_frames);
      CHECK(dma_frames == 0 && r3 == 240 && pix16 == LCD_WIDTH * LCD_HEIGHT); }
    tokenize();
    { const int b[][2] = {{0, 239}}; CHECK(expect_bands(b, 1)); }
    fl_reset_log(); fl_inject_tx_full = 1;                            /* TX FIFO never drains: CPU push must time out, not spin */
    c1d_snapshot_t c0 = snap();
    uint32_t r4 = lcd_push_frame_dirty(fb, true);
    c1d_snapshot_t c1 = snap();
    CHECK(r4 == 0 && c1.tmo_cpu == c0.tmo_cpu + 1 && c1.fault_kind == C1F_CPU_PUSH_TIMEOUT && !spi_bus_failed());
    lcd_set_push_mode(false); CHECK(!lcd_push_mode_cpu());
    fl_reset_log(); CHECK(lcd_push_frame_dirty(fb, false) == 240);

    /* (e) the aborted DMA channel never leaves BUSY (RP2350-E5 style hang): E5 order, bounded, TERMINAL, SPI disabled before CS */
    fl_reset_log(); fl_inject_dma_stuck = 1; fl_inject_abort_stuck = 1;
    c1d_snapshot_t e0 = snap();
    uint32_t r6 = lcd_push_frame_dirty(fb, true);
    c1d_snapshot_t e1 = snap();
    printf("  abort stuck: rows=%u failed=%d sse_disabled=%u abort_without_en_clear=%u cs_before_quiesce=%u fault=%s\n", r6, spi_bus_failed(),
           fl_sse_disabled, fl_abort_without_en_clear, fl_order_cs_before_abort, c1d_fault_name(e1.fault_kind));
    CHECK(r6 == 0 && spi_bus_failed() && e1.bus_failed == 1);
    CHECK(fl_abort_without_en_clear == 0 && fl_sse_disabled >= 1 && fl_order_cs_before_abort == 0);
    CHECK(e1.tmo_dma >= e0.tmo_dma + 1);
    fl_reset_log(); spi_bus_init(); CHECK(!spi_bus_failed());
    lcd_invalidate(); CHECK(lcd_push_frame_dirty(fb, false) == 240);

    /* (d) the SPI shifter never goes idle, even after the DMA abort: bus latched FAILED, logged, no hang, no CS before the wait */
    fl_reset_log(); fl_inject_spi_stuck = 1;
    c1d_snapshot_t d0 = snap();
    uint32_t r5 = lcd_push_frame_dirty(fb, true);
    c1d_snapshot_t d1 = snap();
    CHECK(r5 == 0 && spi_bus_failed() && d1.bus_failed == 1 && d1.fault_kind == C1F_BUS_FAILED && d1.tmo_spi >= d0.tmo_spi + 1);
    CHECK(!touch_hw_read(&r));                                         /* refused, not stuck */
    CHECK(lcd_push_frame_dirty(fb, true) == 0);
    fl_reset_log(); spi_bus_init();                                    /* only a bus re-init clears the latch (never automatic) */
    CHECK(!spi_bus_failed());
    lcd_invalidate(); CHECK(lcd_push_frame_dirty(fb, false) == 240);
}

static void g8_diag_nonblocking(void) {
    cur_group = 8; printf("G8 core-1 diagnostics readable while core 1 is stuck\n");
    c1d_init();
    c1d_stage(C1S_DMA_WAIT);                 /* writer 'hangs' here: it never writes again */
    uint32_t t0 = time_us_32(); sleep_us(250000);
    c1d_snapshot_t d;
    CHECK(c1d_read(&d));
    printf("  stuck in %s for %u ms, torn=%d serial=%u\n", c1d_stage_name(d.stage), d.stage_age_us / 1000u, d.torn, d.stage_serial);
    CHECK(d.stage == C1S_DMA_WAIT && d.stage_age_us >= 250000u && !d.torn && d.stage_serial == 1);
    (void)t0;
    c1d_note_poll(120, true); c1d_note_poll(80, false); c1d_note_render(5000); c1d_note_push(131000, 240); c1d_note_loop(900);
    CHECK(c1d_read(&d) && d.polls == 2 && d.touch_ok == 1 && d.touch_fail == 1 && d.max_touch_us == 120 && d.last_touch_us == 80);
    CHECK(d.renders == 1 && d.pushes == 1 && d.rows_total == 240 && d.max_push_us == 131000 && d.loops == 1);
    CHECK(d.stage == C1S_DMA_WAIT);          /* counters did not move the stage */
}

int main(void) {
    printf("AG-LCD test_lcd_sequence (%s)\n", lcd_build_id);
    g1_init();
    g3_window();
    g4_dirty();
    g5_rotation();
    g6_shared_bus();
    g7_hang_handling();
    g8_diag_nonblocking();
    printf("lcd_sequence: %u checks, %u failures", checks, fails);
    for (int g = 1; g <= 8; g++) printf(" G%d=%s", g, group_fails[g] ? "FAIL" : "ok");
    printf(" -> %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
