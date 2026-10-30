/* Touch backend for an XPT2046 / ADS7846-family resistive controller (command set only).
 * The owner's module has an HR2046 at U1 (photo-confirmed marking); this XPT2046-family driver ran on it at bench r7 (owner-reported: touch tracking + calibration OK, no capture). This code is therefore EXPERIMENTAL: it sends standard
 * 8-bit control words on the dedicated T_CS and interprets the 12-bit answers. If the HR2046 behaves differently
 * the values are meaningless; TOUCH DIAG shows them raw so that is visible at bring-up.
 *
 * Runs only on the UI core, through the SPI0 arbiter (spi_bus.c). Polled, no interrupt. It never
 * touches I2C, the battery bus or any power pin.
 */
#include "touch_hw.h"
#include "board_config.h"
#if TOUCH_BACKEND
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "../hal/spi_bus.h"

#define CTL_Z1  0xB3u   /* start | A=011 | 12 bit | differential | PD=11 (ADC + reference on) */
#define CTL_Z2  0xC3u
#define CTL_X   0xD3u
#define CTL_Y   0x93u
#define CTL_OFF 0x90u   /* Y with PD=00: power down between polls */
#define NS 5
#ifndef TOUCH_Z_MIN
#define TOUCH_Z_MIN 300u
#endif

static uint32_t s_pending_err;

/* One 24-clock conversion through the bounded bus exchange (P1_r10; the SDK's spi_write_read_blocking() has no timeout).
 * T_CS is released only after the exchange finished or was recovered (DMA aborted, shifter idle, RX drained). A failed
 * exchange returns false and the whole poll is abandoned. */
static bool conv(uint8_t ctl, uint16_t *out) {
    const uint8_t tx[3] = {ctl, 0, 0};
    uint8_t rx[3] = {0, 0, 0};
    gpio_put(PIN_TOUCH_CS, 0);
    bool ok = spi_bus_xfer3(tx, rx);
    gpio_put(PIN_TOUCH_CS, 1);
    if (!ok) return false;
    *out = (uint16_t)((((uint16_t)rx[1] << 8) | rx[2]) >> 3) & 0x0FFF;
    return true;
}

static uint16_t median5(uint16_t *v) {
    for (int i = 1; i < NS; i++) { uint16_t k = v[i]; int j = i - 1; while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; } v[j + 1] = k; }
    return v[NS / 2];
}

void touch_hw_init(void) {
    /* pins are configured by spi_bus_init() */
    s_pending_err = 0;
}

bool touch_hw_irq_active(void) {
    bool lvl = gpio_get(PIN_TOUCH_IRQ);
    return TOUCH_IRQ_ACTIVE_LOW ? !lvl : lvl;
}

bool touch_hw_read(touch_raw_t *r) {
    r->pen = false; r->x = r->y = r->z = 0;
    if (!spi_bus_select(SPI_DEV_TOUCH)) { s_pending_err++; return false; }
    uint16_t z1, z2, d, xs[NS], ys[NS];
    #define TH_CONV(c, p) do { if (!conv((c), (p))) { spi_bus_release(); s_pending_err++; return false; } } while (0)
    TH_CONV(CTL_Z1, &d);       /* first conversion after idle is discarded */
    TH_CONV(CTL_Z1, &z1);
    TH_CONV(CTL_Z2, &z2);
    uint32_t z = (z1 > 0) ? (uint32_t)z1 + 4095u - z2 : 0;
    bool pressed = z1 > 0 && z >= TOUCH_Z_MIN;
#if TOUCH_IRQ_GATE
    pressed = pressed && touch_hw_irq_active();
#endif
    if (pressed) {
        TH_CONV(CTL_X, &d);
        for (int i = 0; i < NS; i++) TH_CONV(CTL_X, &xs[i]);
        TH_CONV(CTL_Y, &d);
        for (int i = 0; i < NS; i++) TH_CONV(CTL_Y, &ys[i]);
        r->x = median5(xs); r->y = median5(ys); r->z = (uint16_t)(z > 0xFFFF ? 0xFFFF : z); r->pen = true;
    } else {
        r->z = (uint16_t)(z > 0xFFFF ? 0xFFFF : z);
    }
    TH_CONV(CTL_OFF, &d);      /* power down the ADC/reference */
    #undef TH_CONV
    spi_bus_release();
    return true;
}
#else
void touch_hw_init(void) {}
bool touch_hw_irq_active(void) { return false; }
bool touch_hw_read(touch_raw_t *r) { r->pen = false; r->x = r->y = r->z = 0; return false; }
#endif
