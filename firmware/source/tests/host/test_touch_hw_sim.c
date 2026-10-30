/* AG-TOUCH: production touch_hw_read() against a datasheet-model XPT2046 slave (host, fake SDK).
 * NOT a hardware result. The slave decodes the control byte per XPT2046 datasheet (Table 6/7/8, Fig 12):
 *   S=bit7, A2-A0=bits6-4 (001 Y, 011 Z1, 100 Z2, 101 X, differential), MODE=bit3 (0 = 12 bit),
 *   SER/DFR=bit2 (0 = differential), PD1-PD0=bits1-0 (PD0=0 -> PENIRQ enabled / power down),
 * and answers in the 24-clock frame: byte0 = 0, byte1 = BUSY-null + D11..D5, byte2 = D4..D0 + 000.
 * Intended home: SOURCE/tests/host/test_touch_hw_sim.c (+ fake_touch/ next to it).
 * Build: gcc -I fake_touch -I <SOURCE>/config -DTOUCH_BACKEND=1 -DCHIPLER_P1_ILI9341_TOUCH=1
 *        test_touch_hw_sim.c <SOURCE>/src/input/touch_hw.c <SOURCE>/src/input/touch_core.c -lm
 * -DAG_TOUCH_PATCHED expects the diagnostic-visibility patch (patch_touch_core_diag_raw.diff) applied.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "board_config.h"
#include "src/hal/spi_bus.h"
#include "src/input/touch_hw.h"
#include "src/input/touch_core.h"

typedef enum { M_XPT, M_CONST, M_BYTES, M_RANDOM } slave_mode_t;
static uint32_t lcg = 12345u;
static uint8_t rnd8(void) { lcg = lcg * 1664525u + 1013904223u; return (uint8_t)(lcg >> 24); }
static struct {
    slave_mode_t mode;
    bool touched; unsigned x, y, z1, z2;       /* M_XPT */
    unsigned const12;                          /* M_CONST : every conversion returns this */
    uint8_t  const_byte;                       /* M_BYTES : MISO is a constant byte */
    unsigned spike_mask; unsigned spike_val;   /* corrupt X sample k when bit (xr % 6) set */
    uint8_t log[64]; int nlog;
    int violations;                            /* protocol the real chip would not accept */
    int xy_reads, xr;
    bool cs_low; int last_pd0;
    int bus_refused;
    int rel_calls, sel_calls;
    int xfer_fail, xfer_fail_calls;
} S;
spi_inst_t fake_spi0;
static void sim_reset(void) { memset(&S, 0, sizeof S); }

void gpio_put(unsigned pin, bool v) { if (pin == PIN_TOUCH_CS) S.cs_low = !v; }
bool gpio_get(unsigned pin) {
    if (pin != PIN_TOUCH_IRQ) return true;
    /* PENIRQ low when touched and last control byte had PD0=0 (datasheet "PENIRQ Output") */
    if (S.mode == M_XPT && S.touched && S.last_pd0 == 0) return false;
    return true;
}
bool spi_bus_select(spi_dev_t d) { S.sel_calls++; (void)d; return !S.bus_refused; }
void spi_bus_release(void) { S.rel_calls++; }
void spi_bus_init(void) {}
void spi_bus_set_dma_channel(int ch) { (void)ch; }
spi_dev_t spi_bus_current(void) { return SPI_DEV_NONE; }
uint32_t spi_bus_violations(void) { return 0; }

/* production touch_hw.c now calls the bounded 24-clock exchange (spi_bus_xfer3). The register-level behaviour of that function
 * (timeouts, recovery order) is tested in test_lcd_sequence.c G7 against the production spi_bus.c. */
bool spi_bus_xfer3(const uint8_t tx[3], uint8_t rx[3]) {
    const size_t len = 3;
    if (S.xfer_fail) { S.xfer_fail_calls++; return false; }
    if (!S.cs_low || len != 3) S.violations++;
    if (S.nlog < 64) S.log[S.nlog++] = tx[0];
    rx[0] = rx[1] = rx[2] = 0;
    if (S.mode == M_RANDOM) { rx[0] = rnd8(); rx[1] = rnd8(); rx[2] = rnd8(); return true; }
    if (S.mode == M_BYTES) { rx[0] = rx[1] = rx[2] = S.const_byte; return true; }
    uint8_t c = tx[0];
    if (!(c & 0x80)) { S.violations++; return true; }
    unsigned a = (c >> 4) & 7, mode = (c >> 3) & 1, ser = (c >> 2) & 1, pd = c & 3;
    S.last_pd0 = (int)(pd & 1);
    unsigned v = 0x555;
    bool ch_ok = (a == 1 || a == 3 || a == 4 || a == 5);
    if (!ch_ok) S.violations++;
    if (mode != 0) S.violations++;
    if (ser != 0 && ch_ok) S.violations++;
    if (S.mode == M_CONST) v = S.const12;
    else if (ch_ok && mode == 0 && ser == 0) {
        if (a == 3) v = S.touched ? S.z1 : 0;
        else if (a == 4) v = S.touched ? S.z2 : 4095;
        else {
            S.xy_reads++;
            v = S.touched ? (a == 5 ? S.x : S.y) : 0x2AA;
            if (a == 5) { unsigned k = (unsigned)(S.xr % 6); S.xr++; if (S.spike_mask & (1u << k)) v = S.spike_val; }
        }
    }
    v &= 0xFFF;
    rx[1] = (uint8_t)((v >> 5) & 0x7F);
    rx[2] = (uint8_t)((v & 0x1F) << 3);
    return true;
}

static unsigned fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { unsigned down, up, tap, longp; } evc_t;
static evc_t run_polls(touch_core_t *tc, unsigned n, uint32_t *now) {
    evc_t e = {0, 0, 0, 0};
    for (unsigned i = 0; i < n; i++) {
        touch_raw_t r; bool ok = touch_hw_read(&r);
        tch_event_t ev = touch_core_feed(tc, *now, ok ? &r : NULL);
        switch (ev.kind) { case TCH_EV_DOWN: e.down++; break; case TCH_EV_UP: e.up++; break;
            case TCH_EV_TAP: e.tap++; break; case TCH_EV_LONG: e.longp++; break; default: break; }
        *now += 10;
    }
    return e;
}
static void xpt(bool touched, unsigned x, unsigned y, unsigned z1, unsigned z2) {
    S.mode = M_XPT; S.touched = touched; S.x = x; S.y = y; S.z1 = z1; S.z2 = z2;
}
static void clear_obs(void) { S.nlog = 0; S.violations = 0; S.xy_reads = 0; S.xr = 0; }

int main(void) {
    touch_raw_t r; touch_core_t tc; uint32_t now = 1000;
    touch_hw_init();

    /* 1. command bytes sent by production code are the datasheet-decodable ones */
    sim_reset(); xpt(true, 2000, 1500, 2000, 2300); clear_obs();
    CHECK(touch_hw_read(&r));
    { static const uint8_t exp[] = {0xB3,0xB3,0xC3, 0xD3,0xD3,0xD3,0xD3,0xD3,0xD3, 0x93,0x93,0x93,0x93,0x93,0x93, 0x90};
      CHECK(S.nlog == (int)sizeof exp && memcmp(S.log, exp, sizeof exp) == 0); }
    CHECK(S.violations == 0);
    CHECK(S.last_pd0 == 0);
    CHECK(touch_hw_irq_active());
    CHECK(S.sel_calls == 1 && S.rel_calls == 1);

    /* 2. firm touch: exact decode of x, y, z = z1 + 4095 - z2 */
    sim_reset(); xpt(true, 2000, 1500, 2000, 2300); clear_obs();
    CHECK(touch_hw_read(&r) && r.pen && r.x == 2000 && r.y == 1500 && r.z == 2000 + 4095 - 2300);
    { bool all = true;
      for (unsigned v = 41; v <= 4054; v += 13) { xpt(true, v, 4095 - v, 2000, 2300);
          if (!(touch_hw_read(&r) && r.pen && r.x == v && r.y == 4095 - v)) all = false; }
      CHECK(all); }

    /* 3. no touch */
    sim_reset(); xpt(false, 0, 0, 0, 4095); clear_obs();
    CHECK(touch_hw_read(&r) && !r.pen && r.z == 0 && r.x == 0 && r.y == 0);
    CHECK(S.xy_reads == 1 && S.nlog == 4 && S.log[3] == 0x90 && !touch_hw_irq_active());

    /* 4. light touch / threshold (TOUCH_Z_MIN 300; the z formula is UNVERIFIED vs datasheet) */
    sim_reset(); xpt(true, 1000, 3000, 300, 3900);
    CHECK(touch_hw_read(&r) && r.pen && r.z == 495 && r.x == 1000 && r.y == 3000);
    xpt(true, 1000, 3000, 205, 4000);
    CHECK(touch_hw_read(&r) && r.pen && r.z == 300);
    xpt(true, 1000, 3000, 204, 4000);
    CHECK(touch_hw_read(&r) && !r.pen && r.z == 299 && r.x == 0 && r.y == 0);
    xpt(true, 1000, 3000, 150, 4000);
    CHECK(touch_hw_read(&r) && !r.pen && r.z == 245);

    /* 5. median-of-5 rejects up to 2 corrupted X samples */
    sim_reset(); xpt(true, 2000, 1500, 2000, 2300); S.spike_val = 4000; S.spike_mask = 1u << 3;
    CHECK(touch_hw_read(&r) && r.pen && r.x == 2000);
    S.xr = 0; S.spike_mask = (1u << 2) | (1u << 4);
    CHECK(touch_hw_read(&r) && r.pen && r.x == 2000);

    /* 6. bus refused */
    sim_reset(); xpt(true, 2000, 1500, 2000, 2300); S.bus_refused = 1; clear_obs();
    CHECK(!touch_hw_read(&r) && !r.pen && S.nlog == 0);

    /* 7. through touch_core: press then release -> exactly one TAP */
    sim_reset(); touch_core_init(&tc, 320, 240); now = 1000;
    xpt(true, 2000, 1500, 2000, 2300);
    { evc_t e = run_polls(&tc, 10, &now); CHECK(e.down == 1 && e.tap == 0); }
    xpt(false, 0, 0, 0, 4095);
    { evc_t e = run_polls(&tc, 10, &now); CHECK(e.tap == 1 && e.up == 0); }
    CHECK(tc.taps == 1 && tc.presses == 1 && tc.press_rx == 2000 && tc.press_ry == 1500);
    CHECK(tc.last_rx == 2000 && tc.last_ry == 1500 && tc.last_rz == 2000 + 4095 - 2300);
    sim_reset(); touch_core_init(&tc, 320, 240); now = 1000;
    xpt(true, TCH_RAW_MIN, TCH_RAW_MAX, 2000, 2300); run_polls(&tc, 10, &now);
    xpt(false, 0, 0, 0, 4095); { evc_t e = run_polls(&tc, 10, &now); CHECK(e.tap == 1); }

    /* 8. rail values with pressure: hw says pen, core rejects -> no press, no TAP */
    { const unsigned rx[3] = {0, 4095, 0}, ry[3] = {0, 4095, 4095};
      for (int k = 0; k < 3; k++) {
        sim_reset(); touch_core_init(&tc, 320, 240); now = 1000;
        xpt(true, rx[k], ry[k], 2000, 2300);
        CHECK(touch_hw_read(&r) && r.pen && r.x == rx[k] && r.y == ry[k]);
        evc_t e = run_polls(&tc, 100, &now);
        CHECK(e.down == 0 && e.up == 0 && e.tap == 0 && e.longp == 0);
        CHECK(tc.presses == 0 && tc.taps == 0 && tc.rejected_raw >= 100 && !tc.ever_pen);
      } }

    /* 9. WRONG CHIP A: constant 0 */
    sim_reset(); S.mode = M_CONST; S.const12 = 0; clear_obs();
    CHECK(touch_hw_read(&r) && !r.pen && r.z == 0 && r.x == 0 && r.y == 0 && S.xy_reads == 0);
    sim_reset(); S.mode = M_BYTES; S.const_byte = 0x00; touch_core_init(&tc, 320, 240); now = 1000;
    { evc_t e = run_polls(&tc, 200, &now); CHECK(e.down == 0 && e.tap == 0 && e.up == 0);
      CHECK(tc.presses == 0 && tc.taps == 0 && tc.rejected_raw == 0 && !tc.ever_pen);
      CHECK(tc.last_rx == 0 && tc.last_ry == 0 && tc.last_rz == 0 && tc.polls == 200); }

    /* 10. WRONG CHIP B: constant 0xFFF / MISO stuck high */
    sim_reset(); S.mode = M_CONST; S.const12 = 0xFFF; touch_core_init(&tc, 320, 240); now = 1000;
    CHECK(touch_hw_read(&r) && r.pen && r.x == 4095 && r.y == 4095 && r.z == 4095);
    { evc_t e = run_polls(&tc, 200, &now); CHECK(e.down == 0 && e.up == 0 && e.tap == 0 && e.longp == 0);
      CHECK(tc.presses == 0 && tc.taps == 0 && tc.rejected_raw >= 200 && tc.stuck == 0); }
    sim_reset(); S.mode = M_BYTES; S.const_byte = 0xFF; touch_core_init(&tc, 320, 240); now = 1000;
    CHECK(touch_hw_read(&r) && r.pen && r.x == 4095 && r.y == 4095);
    { evc_t e = run_polls(&tc, 200, &now); CHECK(e.down == 0 && e.tap == 0 && tc.rejected_raw >= 200 && tc.presses == 0); }
    /* DIAG visibility: unpatched code never shows rail data (RAW stays 0/0/0) */
#ifndef AG_TOUCH_PATCHED
    CHECK(tc.last_rx == 0 && tc.last_ry == 0 && tc.last_rz == 0);
#else
    CHECK(tc.last_rx == 4095 && tc.last_ry == 4095 && tc.last_rz == 4095);
#endif

    /* 11. WRONG CHIP C: constant mid-scale -> permanent fake touch; never a TAP; stuck guard releases as UP */
    sim_reset(); S.mode = M_CONST; S.const12 = 0x800; touch_core_init(&tc, 320, 240); now = 1000;
    { evc_t e = run_polls(&tc, 1500, &now);
      CHECK(e.tap == 0 && e.down == 1 && e.up == 1 && tc.stuck == 1 && tc.taps == 0); }

    /* 12. WRONG CHIP D: floating MISO (T_DO unconnected / wrong pin) = random bits. Informational: random data
     *     is not constant, so the rail/stuck filters do not apply; count what reaches the UI. */
    sim_reset(); S.mode = M_RANDOM; touch_core_init(&tc, 320, 240); now = 1000;
    { evc_t e = run_polls(&tc, 100000, &now);
      printf("INFO random-MISO 100000 polls: presses=%lu taps=%lu down=%u up=%u rail_rej=%lu outliers=%lu\n",
             (unsigned long)tc.presses, (unsigned long)tc.taps, e.down, e.up, (unsigned long)tc.rejected_raw, (unsigned long)tc.outliers);
#ifdef AG_TOUCH_ASSERT_RANDOM_NO_TAP
      CHECK(e.tap == 0);
#endif
    }

    /* P1_r10: a failed (timed-out) exchange abandons the poll: false, no result, bus released, error counted, no TAP */
    sim_reset(); xpt(true, 2000, 1500, 2000, 2300); clear_obs(); S.xfer_fail = 1;
    r.pen = true; r.x = 7;
    CHECK(!touch_hw_read(&r) && !r.pen && r.x == 0 && S.xfer_fail_calls == 1 && S.rel_calls == 1 && S.nlog == 0);
    S.xfer_fail = 0; clear_obs();
    CHECK(touch_hw_read(&r) && r.pen);                                   /* recovers on the next poll */
    printf("touch_hw_sim: %u checks, %u failures\n", checks, fails);
    return fails ? 1 : 0;
}
