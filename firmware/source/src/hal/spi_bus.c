#include "spi_bus.h"
#include "board_config.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "diag/core1_diag.h"
#include "spi_regs.h"

#define SPI_DEFAULT_BUDGET_US 5000u     /* command-sized transfers; frame pushes pass their own budget */
#define SPI_RECOVER_IDLE_US   2000u     /* after a DMA abort the shifter must empty within this */
#define DMA_ABORT_BUDGET_US   2000u     /* the abort of one channel must retire its in-flight transfer within this */
#define DRAIN_MAX             32u       /* RX FIFO is 8 deep; never loop on hardware progress */

static spi_dev_t s_cur;
static int s_owner_core = -1;
static uint32_t s_viol;
static int s_dma = -1;
static bool s_failed;
static uint32_t s_tmo, s_rec;
void spi_bus_set_dma_channel(int ch) { s_dma = ch; }

static inline bool expired(uint32_t deadline) { return (int32_t)(time_us_32() - deadline) >= 0; }

uint32_t spi_xfer_budget_us(uint32_t bits, uint32_t hz) {
    if (!hz) hz = 1000000u;
    uint64_t t = ((uint64_t)bits * 1000000u + hz - 1u) / hz;
    return (uint32_t)(t * 2u + 5000u);
}

static void drain_rx(void) {
    for (unsigned n = 0; n < DRAIN_MAX && spi_is_readable(LCD_SPI_INSTANCE); n++) (void)SPI_DR_READ();
    spi_get_hw(LCD_SPI_INSTANCE)->icr = SPI_SSPICR_RORIC_BITS;
}

/* RP2350-E5 abort sequence with a deadline (the SDK's dma_channel_abort() waits for BUSY forever). The channel is disabled
 * (EN cleared) BEFORE the abort request so it cannot re-trigger; lcd.c configures no chained channel (chain_to = itself). */
static bool dma_abort_bounded(uint ch) {
    c1d_stage(C1S_RECOVER);
    DMA_CLEAR_EN(ch);
    DMA_ABORT_REQ(ch);
    uint32_t dl = time_us_32() + DMA_ABORT_BUDGET_US;
    while (DMA_CH_BUSY(ch)) {
        if (expired(dl)) return false;
        tight_loop_contents();
    }
    return true;
}

/* TERMINAL state: the producer or the shifter did not stop. The bus is latched FAILED and is NEVER described as idle. The SPI
 * block is disabled (SSE=0) so it can no longer clock anything; only after that may chip selects be released by the caller. */
static spi_wait_t terminal_failed(c1_fault_t cause) {
    c1d_fault(cause);
    SPI_DISABLE();
    s_failed = true;
    c1d_set_bus_failed(true);
    c1d_fault(C1F_BUS_FAILED);
    return SPI_WAIT_FAILED;
}

/* Timeout path. Order (Codex 2026-10-21): stop the producer first (bounded, E5 sequence), wait (bounded) for the shifter,
 * drain, and leave every chip select exactly as the caller had it; the caller raises CS only after this returns. If the
 * producer or the shifter cannot be stopped the bus goes TERMINAL (see above). */
static spi_wait_t recover(c1_fault_t kind) {
    s_tmo++;
    c1d_fault(kind);
    c1d_stage(C1S_RECOVER);
    if (s_dma >= 0 && !dma_abort_bounded((uint)s_dma)) return terminal_failed(C1F_ABORT_TIMEOUT);
    c1d_stage(C1S_SPI_IDLE);
    uint32_t dl = time_us_32() + SPI_RECOVER_IDLE_US;
    while (spi_is_busy(LCD_SPI_INSTANCE)) {
        if (expired(dl)) return terminal_failed(C1F_SPI_TIMEOUT);
        tight_loop_contents();
    }
    c1d_stage(C1S_SPI_DRAIN);
    drain_rx();
    s_rec++;
    c1d_note_recovery();
    return kind == C1F_DMA_TIMEOUT ? SPI_WAIT_DMA_TIMEOUT : SPI_WAIT_SPI_TIMEOUT;
}

spi_wait_t spi_bus_wait_idle(uint32_t budget_us) {
    if (s_failed) return SPI_WAIT_FAILED;
    uint32_t dl = time_us_32() + budget_us;
    if (s_dma >= 0) {
        c1d_stage(C1S_DMA_WAIT);
        while (DMA_CH_BUSY((uint)s_dma)) {
            if (expired(dl)) return recover(C1F_DMA_TIMEOUT);
            tight_loop_contents();
        }
    }
    c1d_stage(C1S_SPI_IDLE);
    while (spi_is_busy(LCD_SPI_INSTANCE)) {
        if (expired(dl)) return recover(C1F_SPI_TIMEOUT);
        tight_loop_contents();
    }
    return SPI_WAIT_OK;
}

bool spi_bus_write(const uint8_t *d, size_t n) {
    if (s_failed) return false;
    c1d_stage(C1S_LCD_CMD);
    uint32_t dl = time_us_32() + spi_xfer_budget_us((uint32_t)n * 8u, spi_get_baudrate(LCD_SPI_INSTANCE));
    for (size_t i = 0; i < n; i++) {
        while (!spi_is_writable(LCD_SPI_INSTANCE)) {
            if (spi_is_readable(LCD_SPI_INSTANCE)) (void)SPI_DR_READ();   /* keep RX from piling up */
            if (expired(dl)) { recover(C1F_WRITE_TIMEOUT); return false; }
            tight_loop_contents();
        }
        SPI_DR_WRITE(d[i]);
    }
    c1d_stage(C1S_SPI_IDLE);
    while (spi_is_busy(LCD_SPI_INSTANCE)) {
        if (spi_is_readable(LCD_SPI_INSTANCE)) (void)SPI_DR_READ();
        if (expired(dl)) { recover(C1F_WRITE_TIMEOUT); return false; }
        tight_loop_contents();
    }
    drain_rx();
    return true;
}

spi_wait_t spi_bus_recover(unsigned fault_kind) { return recover((c1_fault_t)fault_kind); }

bool spi_bus_xfer3(const uint8_t tx[3], uint8_t rx[3]) {
    if (s_failed) return false;
    c1d_stage(C1S_TOUCH_XFER);
    uint32_t dl = time_us_32() + spi_xfer_budget_us(24u, spi_get_baudrate(LCD_SPI_INSTANCE));
    drain_rx();                       /* stale bytes must never be mistaken for the answer */
    for (int i = 0; i < 3; i++) {
        while (!spi_is_writable(LCD_SPI_INSTANCE)) {
            if (expired(dl)) { recover(C1F_TOUCH_TIMEOUT); return false; }
            tight_loop_contents();
        }
        SPI_DR_WRITE(tx[i]);
    }
    for (int i = 0; i < 3; i++) {
        while (!spi_is_readable(LCD_SPI_INSTANCE)) {
            if (expired(dl)) { recover(C1F_TOUCH_TIMEOUT); return false; }
            tight_loop_contents();
        }
        rx[i] = (uint8_t)SPI_DR_READ();
    }
    return true;
}

void spi_bus_init(void) {
    gpio_init(PIN_LCD_CS);   gpio_set_dir(PIN_LCD_CS, GPIO_OUT);   gpio_put(PIN_LCD_CS, 1);
#if TOUCH_BACKEND
    gpio_init(PIN_TOUCH_CS); gpio_set_dir(PIN_TOUCH_CS, GPIO_OUT); gpio_put(PIN_TOUCH_CS, 1);
    gpio_init(PIN_TOUCH_IRQ); gpio_set_dir(PIN_TOUCH_IRQ, GPIO_IN); gpio_pull_up(PIN_TOUCH_IRQ);
#endif
    spi_init(LCD_SPI_INSTANCE, LCD_SPI_HZ_TOUCH);
    gpio_set_function(PIN_LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_LCD_MOSI, GPIO_FUNC_SPI);
#if TOUCH_BACKEND
    gpio_set_function(PIN_SPI_MISO, GPIO_FUNC_SPI);
#endif
    s_cur = SPI_DEV_NONE;
    s_owner_core = (int)get_core_num();
    s_failed = false; s_tmo = s_rec = 0;
}

bool spi_bus_select(spi_dev_t d) {
    if ((int)get_core_num() != s_owner_core) { s_viol++; return false; }
    if (spi_bus_wait_idle(SPI_DEFAULT_BUDGET_US) == SPI_WAIT_FAILED) return false;
    gpio_put(PIN_LCD_CS, 1);
#if TOUCH_BACKEND
    gpio_put(PIN_TOUCH_CS, 1);
#endif
    if (d == s_cur) return true;
    c1d_stage(C1S_SPI_FORMAT);
    if (d == SPI_DEV_LCD) spi_set_baudrate(LCD_SPI_INSTANCE, LCD_SPI_HZ);
    else if (d == SPI_DEV_TOUCH) spi_set_baudrate(LCD_SPI_INSTANCE, LCD_SPI_HZ_TOUCH);
    spi_set_format(LCD_SPI_INSTANCE, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    s_cur = d;
    return true;
}

void spi_bus_release(void) {
    spi_bus_wait_idle(SPI_DEFAULT_BUDGET_US);      /* a recovered/failed bus still gets its chip selects released, after the wait */
    gpio_put(PIN_LCD_CS, 1);
#if TOUCH_BACKEND
    gpio_put(PIN_TOUCH_CS, 1);
#endif
}

spi_dev_t spi_bus_current(void) { return s_cur; }
uint32_t spi_bus_violations(void) { return s_viol; }
bool spi_bus_failed(void) { return s_failed; }
uint32_t spi_bus_timeouts(void) { return s_tmo; }
uint32_t spi_bus_recoveries(void) { return s_rec; }

void spi_bus_hw_status(spi_bus_hw_t *o) {
    int ch = s_dma;
    o->dma_busy = ch >= 0 && dma_channel_is_busy((uint)ch);
    o->dma_remaining = ch >= 0 ? (dma_channel_hw_addr((uint)ch)->transfer_count & 0x0FFFFFFFu) : 0u;
    o->sr = spi_get_hw(LCD_SPI_INSTANCE)->sr;
}
