/* AG-LCD fake pico-sdk implementation: records every SPI frame with DC / CS state, models the PL022
 * 8-deep RX FIFO and the pico-sdk 2.1.1 blocking SPI helpers, and a minimal XPT2046-family responder.
 *
 * Timing model: the CPU is infinitely faster than SCL. A frame written to the TX FIFO is "pending"
 * and is only shifted (and its received frame pushed into the RX FIFO) when code would otherwise
 * have to wait for it: spi_is_busy(), a full TX FIFO, or the RX-wait inside spi_write_read_blocking.
 * A CS pin that changes while frames are pending is counted (fl_cs_while_busy) and those frames are
 * then shifted with the NEW CS level (the addressed device never sees them).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "board_config.h"
#include "fake_lcd.h"

#define CLK_PERI_HZ 150000000u
#define MAX_FRAMES 600000u

fl_frame_t *fl_frames;
size_t fl_nframes;
fl_rst_t fl_rst[16];
int fl_nrst;
unsigned fl_cs_while_busy, fl_fmt_while_busy, fl_dma_errors, fl_ror, fl_baud_actual;
int fl_inject_dma_stuck, fl_inject_spi_stuck, fl_inject_rx_never, fl_inject_tx_full, fl_inject_abort_stuck;
unsigned fl_sse_disabled, fl_abort_without_en_clear;
static int s_en_cleared;
unsigned fl_dma_aborts, fl_order_cs_before_abort;
static int s_dma_pending_stuck;

spi_inst_t fake_spi0;
static spi_hw_t s_hw;
static uint8_t s_pin[64];
static uint64_t s_t_us;
static unsigned s_bits = 8;
static struct { uint16_t v; uint8_t dma; } s_pend[8];
static unsigned s_npend;
static uint16_t s_rx[8];
static unsigned s_nrx;
static uint16_t s_xz1, s_xz2, s_xx, s_xy;
static unsigned s_xidx;
static uint8_t s_xctl;
static int s_dma_claimed = -1;

void fl_reset_log(void) {
    if (!fl_frames) fl_frames = calloc(MAX_FRAMES, sizeof *fl_frames);
    fl_nframes = 0; fl_nrst = 0;
    fl_cs_while_busy = fl_fmt_while_busy = fl_dma_errors = fl_ror = 0;
    fl_inject_dma_stuck = fl_inject_spi_stuck = fl_inject_rx_never = fl_inject_tx_full = fl_inject_abort_stuck = 0;
    fl_sse_disabled = fl_abort_without_en_clear = 0; s_en_cleared = 0;
    fl_dma_aborts = fl_order_cs_before_abort = 0; s_dma_pending_stuck = 0;
}
uint64_t fl_now_us(void) { return s_t_us; }
unsigned fl_rx_level(void) { return s_nrx; }
void fl_xpt_set(uint16_t z1, uint16_t z2, uint16_t x, uint16_t y) { s_xz1 = z1; s_xz2 = z2; s_xx = x; s_xy = y; }

static uint16_t xpt_value(uint8_t ctl) {
    switch ((ctl >> 4) & 7u) { case 3: return s_xz1; case 4: return s_xz2; case 5: return s_xx; case 1: return s_xy; default: return 0; }
}

/* shift one pending frame: MOSI recorded, MISO pushed into the RX FIFO (PL022 always receives) */
static void shift_one(void) {
    if (!s_npend) return;
    uint16_t v = s_pend[0].v; uint8_t dma = s_pend[0].dma;
    memmove(&s_pend[0], &s_pend[1], (s_npend - 1) * sizeof s_pend[0]);
    s_npend--;
    uint16_t miso = (s_bits == 16) ? 0xFFFFu : 0xFFu;            /* idle MISO assumed high */
    if (s_pin[PIN_TOUCH_CS] == 0 && s_bits == 8) {
        if (s_xidx == 0) { s_xctl = (uint8_t)v; miso = 0; }
        else if (s_xidx == 1) miso = (uint16_t)(((unsigned)xpt_value(s_xctl) << 3) >> 8) & 0xFFu;
        else if (s_xidx == 2) miso = (uint16_t)(((unsigned)xpt_value(s_xctl) << 3) & 0xFFu);
        else miso = 0;
        s_xidx++;
    }
    if (fl_frames && fl_nframes < MAX_FRAMES) {
        fl_frame_t *f = &fl_frames[fl_nframes++];
        f->v = v; f->bits = (uint8_t)s_bits; f->dc = s_pin[PIN_LCD_DC]; f->lcd_cs = s_pin[PIN_LCD_CS];
        f->touch_cs = s_pin[PIN_TOUCH_CS]; f->via_dma = dma; f->t_us = s_t_us;
    }
    if (s_nrx < 8) s_rx[s_nrx++] = miso; else fl_ror++;
}
static void shift_all(void) { while (s_npend) shift_one(); }
static void push_tx(uint16_t v, uint8_t dma) {
    if (s_npend == 8) shift_one();
    s_pend[s_npend].v = v; s_pend[s_npend].dma = dma; s_npend++;
}
static uint16_t pop_rx(void) {
    uint16_t v = s_rx[0];
    memmove(&s_rx[0], &s_rx[1], (s_nrx - 1) * sizeof s_rx[0]);
    s_nrx--;
    return v;
}

/* ---- pico/stdlib, multicore ---- */
void tight_loop_contents(void) { s_t_us += 1; if (!fl_inject_tx_full) shift_one(); }
void sleep_ms(uint32_t ms) { s_t_us += (uint64_t)ms * 1000u; }
void sleep_us(uint64_t us) { s_t_us += us; }
uint32_t time_us_32(void) { s_t_us += 1; return (uint32_t)s_t_us; }
unsigned get_core_num(void) { return 1; }

/* ---- gpio ---- */
void gpio_init(unsigned p) { (void)p; }
void gpio_set_dir(unsigned p, bool o) { (void)p; (void)o; }
void gpio_pull_up(unsigned p) { (void)p; }
void gpio_set_function(unsigned p, unsigned f) { (void)p; (void)f; }
bool gpio_get(unsigned p) { return p == PIN_TOUCH_IRQ ? true : s_pin[p & 63]; }
void gpio_put(unsigned p, bool v) {
    p &= 63;
    if ((p == PIN_LCD_CS || p == PIN_TOUCH_CS || p == PIN_LCD_DC) && s_npend && s_pin[p] != (uint8_t)v) fl_cs_while_busy++;
    if ((p == PIN_LCD_CS || p == PIN_TOUCH_CS) && v && s_dma_pending_stuck && !fl_sse_disabled) fl_order_cs_before_abort++;
    if (p == PIN_TOUCH_CS && v == 0 && s_pin[p] != 0) s_xidx = 0;
    if (p == PIN_LCD_RST && s_pin[p] != (uint8_t)v && fl_nrst < 16) { fl_rst[fl_nrst].level = v; fl_rst[fl_nrst].t_us = s_t_us; fl_nrst++; }
    s_pin[p] = (uint8_t)v;
}

/* ---- spi ---- */
static unsigned sdk_baud(unsigned baud) {               /* pico-sdk 2.1.1 spi_set_baudrate() divider search */
    unsigned prescale, postdiv;
    for (prescale = 2; prescale <= 254; prescale += 2)
        if (CLK_PERI_HZ < prescale * 256ull * baud) break;
    for (postdiv = 256; postdiv > 1; --postdiv)
        if (CLK_PERI_HZ / (prescale * (postdiv - 1)) > baud) break;
    return CLK_PERI_HZ / (prescale * postdiv);
}
unsigned spi_init(spi_inst_t *s, unsigned hz) { unsigned b = spi_set_baudrate(s, hz); spi_set_format(s, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST); return b; }
unsigned spi_set_baudrate(spi_inst_t *s, unsigned hz) { (void)s; shift_all(); fl_baud_actual = sdk_baud(hz); return fl_baud_actual; }
unsigned spi_get_baudrate(const spi_inst_t *s) { (void)s; return fl_baud_actual; }
void spi_set_format(spi_inst_t *s, unsigned bits, spi_cpol_t cpol, spi_cpha_t cpha, spi_order_t order) {
    (void)s;
    if (s_npend) fl_fmt_while_busy++;
    if (cpol != SPI_CPOL_0 || cpha != SPI_CPHA_0 || order != SPI_MSB_FIRST) fl_dma_errors++;  /* ILI9341 needs mode 0, MSB first */
    shift_all();
    s_bits = bits;
}
bool spi_is_busy(const spi_inst_t *s) { (void)s; if (fl_inject_spi_stuck) return true; shift_all(); return false; }
bool spi_is_writable(const spi_inst_t *s) { (void)s; if (fl_inject_tx_full) return false; return s_npend < 8; }
/* NOTE: production code only uses spi_is_readable() in the pattern
 *   while (spi_is_readable(spi)) (void)spi_get_hw(spi)->dr;
 * A plain struct read cannot be intercepted on the host, so this fake pops the entry here. */
bool spi_is_readable(const spi_inst_t *s) { (void)s; if (fl_inject_rx_never) return false; if (!s_nrx && s_npend) shift_one(); return s_nrx > 0; }
/* register seam (src/hal/spi_regs.h, CHIPLER_HOST_SPI_FAKE): DR write = TX FIFO push, DR read = RX FIFO pop */
void fake_spi_dr_write(uint32_t v) { push_tx((uint16_t)v, 0); }
uint32_t fake_spi_dr_read(void) { return s_nrx ? pop_rx() : 0u; }
spi_hw_t *spi_get_hw(spi_inst_t *s) { (void)s; return &s_hw; }
unsigned spi_get_dreq(spi_inst_t *s, bool is_tx) { (void)s; return is_tx ? 16u : 17u; }

/* pico-sdk 2.1.1 spi.c:107-130 */
int spi_write_blocking(spi_inst_t *s, const uint8_t *src, size_t len) {
    (void)s;
    for (size_t i = 0; i < len; ++i) push_tx(src[i], 0);
    while (s_nrx) pop_rx();
    shift_all();
    while (s_nrx) pop_rx();
    s_hw.icr = SPI_SSPICR_RORIC_BITS;
    return (int)len;
}
/* pico-sdk 2.1.1 spi.c:84-104. "spi_is_readable" inside the loop is evaluated against the model:
 * if nothing is readable yet the CPU would spin, i.e. time passes and the oldest pending frame shifts. */
int spi_write_read_blocking(spi_inst_t *s, const uint8_t *src, uint8_t *dst, size_t len) {
    (void)s;
    const size_t fifo_depth = 8;
    size_t rx_remaining = len, tx_remaining = len;
    while (rx_remaining || tx_remaining) {
        if (tx_remaining && s_npend < 8 && rx_remaining < tx_remaining + fifo_depth) { push_tx(*src++, 0); --tx_remaining; }
        if (rx_remaining) {
            if (!s_nrx) shift_one();
            if (s_nrx) { *dst++ = (uint8_t)pop_rx(); --rx_remaining; }
        }
    }
    return (int)len;
}

/* ---- dma ---- */
int dma_claim_unused_channel(bool required) { (void)required; s_dma_claimed = 5; return 5; }
dma_channel_config dma_channel_get_default_config(uint ch) { (void)ch; dma_channel_config c = {DMA_SIZE_32, 0x3F, true, false}; return c; }
void channel_config_set_transfer_data_size(dma_channel_config *c, enum dma_channel_transfer_size sz) { c->size = sz; }
void channel_config_set_dreq(dma_channel_config *c, uint dreq) { c->dreq = dreq; }
void channel_config_set_read_increment(dma_channel_config *c, bool inc) { c->rinc = inc; }
void channel_config_set_write_increment(dma_channel_config *c, bool inc) { c->winc = inc; }
void dma_channel_configure(uint ch, const dma_channel_config *c, volatile void *write_addr,
                           const volatile void *read_addr, uint count, bool trigger) {
    if ((int)ch != s_dma_claimed) fl_dma_errors++;
    if (write_addr != (volatile void *)&s_hw.dr) fl_dma_errors++;
    if (c->dreq != spi_get_dreq(spi0, true) || !c->rinc || c->winc) fl_dma_errors++;
    if (!((c->size == DMA_SIZE_16 && s_bits == 16) || (c->size == DMA_SIZE_8 && s_bits == 8))) fl_dma_errors++;
    if (!trigger) return;
    const volatile uint16_t *p16 = (const volatile uint16_t *)read_addr;
    const volatile uint8_t *p8 = (const volatile uint8_t *)read_addr;
    for (uint i = 0; i < count; i++) push_tx(c->size == DMA_SIZE_16 ? p16[i] : p8[i], 1);
    if (fl_inject_dma_stuck) s_dma_pending_stuck = 1;
    /* DMA completes when the last element is in the TX FIFO; up to 8 frames are still shifting */
}
void dma_channel_wait_for_finish_blocking(uint ch) { (void)ch; }
bool dma_channel_is_busy(uint ch) { (void)ch; return fl_inject_dma_stuck && s_dma_pending_stuck; }
void dma_channel_abort(uint ch) { (void)ch; fl_dma_aborts++; s_dma_pending_stuck = 0; }
/* register seam (src/hal/spi_regs.h): E5 sequence = clear EN, request abort, poll BUSY */
void fake_dma_clear_en(unsigned ch) { (void)ch; s_en_cleared = 1; }
void fake_dma_abort_req(unsigned ch) { (void)ch; if (!s_en_cleared) fl_abort_without_en_clear++; fl_dma_aborts++; if (!fl_inject_abort_stuck) s_dma_pending_stuck = 0; }
bool fake_dma_busy(unsigned ch) { (void)ch; return s_dma_pending_stuck && (fl_inject_dma_stuck || fl_inject_abort_stuck); }
void fake_spi_disable(void) { fl_sse_disabled++; }
static dma_channel_hw_t s_dma_hw;
dma_channel_hw_t *dma_channel_hw_addr(uint ch) { (void)ch; s_dma_hw.transfer_count = s_dma_pending_stuck ? 1234u : 0u; return &s_dma_hw; }
