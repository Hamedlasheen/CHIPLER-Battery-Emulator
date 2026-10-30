/* P1_r1 shared SPI0 arbiter (production spi_bus.c, fake SDK): ordering, one owner core, CS discipline. */
#include <stdio.h>
#include <string.h>
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "pico/stdlib.h"
#include "board_config.h"
#include "../../src/hal/spi_bus.h"

enum { E_GPIO_PUT, E_BAUD, E_FORMAT, E_DMA_POLL, E_SPI_POLL, E_INIT, E_ABORT, E_SSE, E_ENCLR };
typedef struct { int kind; unsigned a, b; } ev_t;
static ev_t ev[512]; static int nev;
static void rec(int k, unsigned a, unsigned b) { if (nev < 512) { ev[nev].kind = k; ev[nev].a = a; ev[nev].b = b; nev++; } }
static unsigned core = 0; static int dma_busy_polls, spi_busy_polls;
spi_inst_t fake_spi0;
unsigned get_core_num(void) { return core; }
void tight_loop_contents(void) {}
void gpio_init(unsigned p) { (void)p; }
void gpio_set_dir(unsigned p, bool o) { (void)p; (void)o; }
void gpio_put(unsigned p, bool v) { rec(E_GPIO_PUT, p, v); }
void gpio_pull_up(unsigned p) { (void)p; }
void gpio_set_function(unsigned p, unsigned f) { (void)p; (void)f; }
unsigned spi_init(spi_inst_t *s, unsigned hz) { (void)s; rec(E_INIT, hz, 0); return hz; }
unsigned spi_set_baudrate(spi_inst_t *s, unsigned hz) { (void)s; rec(E_BAUD, hz, 0); return hz; }
void spi_set_format(spi_inst_t *s, unsigned b, int x, int y, int z) { (void)s; (void)x; (void)y; (void)z; rec(E_FORMAT, b, 0); }
bool spi_is_busy(spi_inst_t *s) { (void)s; rec(E_SPI_POLL, 0, 0); return spi_busy_polls-- > 0; }
bool dma_channel_is_busy(unsigned c) { (void)c; rec(E_DMA_POLL, 0, 0); return dma_busy_polls-- > 0; }
static uint32_t now_us;
uint32_t time_us_32(void) { now_us += 40; return now_us; }          /* every poll costs 40 us so a budget always expires */
void dma_channel_abort(unsigned c) { (void)c; rec(E_ABORT, 0, 0); dma_busy_polls = 0; }
static int en_cleared, abort_stuck, sse_off;
void fake_spi_dr_write(uint32_t v) { (void)v; }
uint32_t fake_spi_dr_read(void) { return 0; }
void fake_spi_disable(void) { sse_off++; rec(E_SSE, 0, 0); }
void fake_dma_clear_en(unsigned c) { (void)c; en_cleared++; rec(E_ENCLR, 0, 0); }
void fake_dma_abort_req(unsigned c) { (void)c; rec(E_ABORT, 0, 0); if (!abort_stuck) dma_busy_polls = 0; }
bool fake_dma_busy(unsigned c) { (void)c; rec(E_DMA_POLL, 0, 0); return dma_busy_polls-- > 0; }
static dma_channel_hw_t dhw; dma_channel_hw_t *dma_channel_hw_addr(unsigned c) { (void)c; return &dhw; }
static spi_hw_t shw; spi_hw_t *spi_get_hw(spi_inst_t *s) { (void)s; return &shw; }
bool spi_is_readable(spi_inst_t *s) { (void)s; return false; }
bool spi_is_writable(spi_inst_t *s) { (void)s; return true; }
unsigned spi_get_baudrate(spi_inst_t *s) { (void)s; return 9375000u; }

static unsigned fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static int last_of(int kind, int from, int to) { int r = -1; for (int i = from; i < to && i < nev; i++) if (ev[i].kind == kind) r = i; return r; }
static int first_cs_put(int from) { for (int i = from; i < nev; i++) if (ev[i].kind == E_GPIO_PUT && (ev[i].a == PIN_LCD_CS || ev[i].a == PIN_TOUCH_CS)) return i; return -1; }

int main(void) {
    core = 1;
    spi_bus_init();
    /* both chip selects driven inactive (high) before the SPI block is configured */
    { int cs = 0, init = -1; for (int i = 0; i < nev; i++) { if (ev[i].kind == E_INIT) init = i; }
      for (int i = 0; i < init; i++) if (ev[i].kind == E_GPIO_PUT && ev[i].b == 1 && (ev[i].a == PIN_LCD_CS || ev[i].a == PIN_TOUCH_CS)) cs++;
      CHECK(cs == 2 && init > 0); }

    /* LCD -> TOUCH switch with a DMA tail and a busy FIFO still pending */
    spi_bus_set_dma_channel(3);
    CHECK(spi_bus_select(SPI_DEV_LCD) && spi_bus_current() == SPI_DEV_LCD);
    nev = 0; dma_busy_polls = 4; spi_busy_polls = 3;
    CHECK(spi_bus_select(SPI_DEV_TOUCH) && spi_bus_current() == SPI_DEV_TOUCH);
    int baud = last_of(E_BAUD, 0, nev), lastdma = last_of(E_DMA_POLL, 0, nev), lastspi = last_of(E_SPI_POLL, 0, nev), cs0 = first_cs_put(0);
    CHECK(baud >= 0 && lastdma >= 0 && lastspi >= 0 && cs0 >= 0);
    CHECK(ev[baud].a == LCD_SPI_HZ_TOUCH);
    CHECK(lastdma < cs0 && lastspi < cs0);                 /* idle (DMA + FIFO) BEFORE any chip select moves */
    { int highs = 0; for (int i = cs0; i < baud; i++) if (ev[i].kind == E_GPIO_PUT && ev[i].b == 1) highs++;
      CHECK(highs == 2); }                                  /* both CS high before the baud/format change */
    CHECK(last_of(E_FORMAT, 0, nev) > baud);

    /* same device again: no reconfiguration, CS still forced inactive */
    nev = 0; dma_busy_polls = spi_busy_polls = 0;
    CHECK(spi_bus_select(SPI_DEV_TOUCH));
    CHECK(last_of(E_BAUD, 0, nev) < 0 && last_of(E_FORMAT, 0, nev) < 0 && first_cs_put(0) >= 0);

    /* release waits for idle, then CS high */
    nev = 0; dma_busy_polls = 2; spi_busy_polls = 2;
    spi_bus_release();
    CHECK(first_cs_put(0) > last_of(E_DMA_POLL, 0, nev) && first_cs_put(0) > last_of(E_SPI_POLL, 0, nev));

    /* the other core is refused, counted, and never touches the bus */
    nev = 0; core = 0;
    CHECK(!spi_bus_select(SPI_DEV_LCD) && !spi_bus_select(SPI_DEV_TOUCH));
    CHECK(spi_bus_violations() == 2 && nev == 0 && spi_bus_current() == SPI_DEV_TOUCH);
    core = 1;
    CHECK(spi_bus_select(SPI_DEV_LCD) && ev[last_of(E_BAUD, 0, nev)].a == LCD_SPI_HZ);

    /* P1_r10: bounded waits. A DMA channel that never finishes is aborted FIRST; chip selects move only afterwards. */
    spi_bus_init(); core = 1; spi_bus_set_dma_channel(3);
    nev = 0; dma_busy_polls = 1000000000; spi_busy_polls = 0;
    CHECK(spi_bus_select(SPI_DEV_LCD));
    { int ab = last_of(E_ABORT, 0, nev), cs = first_cs_put(0), en = last_of(E_ENCLR, 0, nev);
      CHECK(en >= 0 && ab > en);                          /* RP2350-E5: EN cleared before the abort request */
      CHECK(ab >= 0 && cs > ab);
      CHECK(spi_bus_timeouts() == 1 && spi_bus_recoveries() == 1 && !spi_bus_failed()); }
    /* the shifter never goes idle even after the abort: latched FAILED, select refused, no hang, release still ends with CS high */
    nev = 0; dma_busy_polls = 0; spi_busy_polls = 1000000000;
    CHECK(!spi_bus_select(SPI_DEV_TOUCH) && spi_bus_failed());
    nev = 0; spi_bus_release();
    CHECK(first_cs_put(0) >= 0);
    CHECK(spi_xfer_budget_us(76800u * 16u, 9375000u) > 2u * 131072u && spi_xfer_budget_us(8u, 9375000u) < 6000u);
    spi_bus_init(); CHECK(!spi_bus_failed());                  /* only an explicit re-init clears the latch */

    /* the aborted channel never leaves BUSY: bounded, TERMINAL, SPI block disabled BEFORE any chip select is released */
    spi_bus_init(); core = 1; spi_bus_set_dma_channel(3);
    nev = 0; dma_busy_polls = 1000000000; spi_busy_polls = 0; abort_stuck = 1;
    CHECK(!spi_bus_select(SPI_DEV_LCD) && spi_bus_failed());
    { int en = last_of(E_ENCLR, 0, nev), ab = last_of(E_ABORT, 0, nev), sse = last_of(E_SSE, 0, nev), cs = first_cs_put(0);
      CHECK(en >= 0 && ab > en && sse > ab);
      CHECK(cs < 0 || cs > sse); }
    nev = 0; spi_bus_release();
    CHECK(first_cs_put(0) >= 0 && spi_bus_current() != SPI_DEV_NONE + 99);
    abort_stuck = 0; dma_busy_polls = 0;
    spi_bus_init(); CHECK(!spi_bus_failed());

    printf("spi_bus arbiter: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
