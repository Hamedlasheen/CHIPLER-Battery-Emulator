/*
 * PM-ID-001-r2 bench tool (Claude, 2026-10-17): INA226 identity + raw readings. NO PHONE.
 * Owner decision PM-SELECT-001 = INA226 (prototype). Wiring: WIRING-INA226-r2
 *   INA226 SDA -> GP8, SCL -> GP9, ALERT -> GP10 (each with external pull-up to 3V3),
 *   A0 = A1 = GND (0x40), VS = 3V3, shunt (nominal 10 mOhm, unverified) IN+ PSU side, IN- load side.
 * Writes ONLY the CONFIG reset bit (RST). Never writes CALIBRATION, MASK/ENABLE or ALERT LIMIT.
 * r2 changes (Codex CODEX_PM-ID-001_REVIEW): strict identity AND; transport errors reported per
 * register with rc; GPIO idle level is a sample only; readings only when lines + identity pass;
 * waits (bounded, 30 s) for the serial logger and repeats the summary every 10 s.
 * Main firmware unchanged. GP5/GP6/GP7 (battery bus) untouched. OUT_EN forced low.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "board_config.h"
#include "hal/board_io.h"
#include "ina226_id.h"

#define ADDR 0x40
#define SHUNT_NOMINAL_OHMS 0.010f   /* marked "010"; value is a hypothesis until measured */
#define REV "PM-ID-001-r2"

static int last_rc;
static bool rd16(uint8_t reg, uint16_t *v) {
    uint8_t b[2];
    last_rc = i2c_write_timeout_us(PM_I2C_INSTANCE, ADDR, &reg, 1, true, 5000);
    if (last_rc != 1) return false;
    last_rc = i2c_read_timeout_us(PM_I2C_INSTANCE, ADDR, b, 2, false, 5000);
    if (last_rc != 2) return false;
    *v = (uint16_t)(b[0] << 8 | b[1]);
    return true;
}
static bool wr16(uint8_t reg, uint16_t v) {
    uint8_t b[3] = {reg, (uint8_t)(v >> 8), (uint8_t)v};
    last_rc = i2c_write_timeout_us(PM_I2C_INSTANCE, ADDR, b, 3, false, 5000);
    return last_rc == 3;
}

static const char *vtext(ina226_id_verdict_t v) {
    switch (v) {
    case ID_PASS:            return "PASS: CONFIG=4127h, FEh=5449h, FFh=2260h/2261h (registers match INA226)";
    case ID_TRANSPORT:       return "FAIL: I2C transport error (NAK/timeout/short) - identity NOT evaluated";
    case ID_CONFIG_MISMATCH: return "FAIL: CONFIG after reset != 4127h (cause unresolved: reset/read/transport/part)";
    case ID_MFR_MISMATCH:    return "FAIL: Manufacturer ID (FEh) != 5449h";
    default:                 return "FAIL: Die ID (FFh) not 2260h/2261h";
    }
}

int main(void) {
    board_io_init();                       /* OUT_EN low: phone output OFF */
    stdio_init_all();
    /* wait for the serial logger (DTR) up to 30 s, so RESULT is not printed into the void */
    for (int i = 0; i < 300 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(500);
    printf("\n[PMID] CHIPLER %s INA226 identity test (bench only; no phone)\n", REV);
    printf("[PMID] serial %s\n", stdio_usb_connected() ? "connected" : "NOT detected after 30 s (output may be lost)");

    /* 1. idle level sample with internal pulls disabled (a sample only; not proof of a pull-up value) */
    const uint pins[3] = {PIN_PM_SDA, PIN_PM_SCL, PIN_PM_ALERT};
    const char *names[3] = {"SDA GP8", "SCL GP9", "ALERT GP10"};
    int low = 0;
    for (int i = 0; i < 3; i++) { gpio_init(pins[i]); gpio_set_dir(pins[i], GPIO_IN); gpio_disable_pulls(pins[i]); }
    sleep_ms(5);
    for (int i = 0; i < 3; i++) {
        bool h = gpio_get(pins[i]);
        printf("[PMID] idle %-10s = %s\n", names[i], h ? "HIGH (idle sample)" : "LOW  (pull-up missing, short, or line held low)");
        if (!h) low++;
    }

    /* 2. bus scan at 100 kHz (skipped if SDA/SCL are low) */
    bool lines_ok = gpio_get(PIN_PM_SDA) && gpio_get(PIN_PM_SCL);
    i2c_init(PM_I2C_INSTANCE, 100000);
    gpio_set_function(PIN_PM_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_PM_SCL, GPIO_FUNC_I2C);
    int found = 0; bool at40 = false;
    if (lines_ok) {
        printf("[PMID] scan 0x08..0x77:");
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            uint8_t z;
            if (i2c_read_timeout_us(PM_I2C_INSTANCE, a, &z, 1, false, 3000) >= 0) {
                printf(" 0x%02X", a); found++; if (a == ADDR) at40 = true;
            }
        }
        printf("%s\n", found ? "" : " (none)");
        if (found > 1) printf("[PMID] WARNING: more than one device on I2C0 (Gate 2 loopback wires still fitted?)\n");
    } else {
        printf("[PMID] scan skipped: SDA/SCL not high\n");
    }

    /* 3. identity: every step must transfer; first failing step reported with rc */
    uint16_t cfg = 0, fe = 0, ff = 0;
    const char *fail_step = NULL;
    if (!at40) fail_step = "no ACK at 0x40";
    else if (!wr16(0x00, 0x8000)) fail_step = "write CONFIG RST";
    else {
        sleep_ms(2);
        if (!rd16(0x00, &cfg)) fail_step = "read CONFIG";
        else if (!rd16(0xFE, &fe)) fail_step = "read FEh";
        else if (!rd16(0xFF, &ff)) fail_step = "read FFh";
    }
    if (fail_step) printf("[PMID] transport: FAILED at '%s' (rc=%d)\n", fail_step, at40 ? last_rc : 0);
    else {
        printf("[PMID] CONFIG after reset = 0x%04X (expect 0x4127)\n", cfg);
        printf("[PMID] MFR ID  (FEh)      = 0x%04X (expect 0x5449)\n", fe);
        printf("[PMID] DIE ID  (FFh)      = 0x%04X (expect 0x2260 or 0x2261)\n", ff);
    }
    ina226_id_verdict_t v = ina226_id_verdict(fail_step == NULL, cfg, fe, ff);
    int bad = low + (v != ID_PASS);
    printf("[PMID] identity: %s\n", vtext(v));
    printf("[PMID] note: register match is strong evidence, not proof of a genuine/undamaged part\n");
    printf("[PMID] RESULT: %s (%d errors)\n", bad ? "FAIL" : "PASS", bad);

    /* 4. raw readings every 1 s (default config: continuous, 1.1 ms, no averaging). Read-only.
     * Only when RESULT is PASS; summary repeated every 10 s for a late logger. */
    bool measure = (bad == 0);
    printf("[PMID] live raw readings: %s\n", measure ?
           "ON (I = Vshunt / 0.010 ohm NOMINAL; uncalibrated, shunt value unverified)" : "OFF (RESULT is not PASS)");
    for (uint32_t n = 1;; n++) {
        if (measure) {
            uint16_t sh = 0, bus = 0;
            if (!rd16(0x01, &sh))       printf("[PMID] #%lu read FAILED reg 01h (rc=%d) - no sample\n", (unsigned long)n, last_rc);
            else if (!rd16(0x02, &bus)) printf("[PMID] #%lu read FAILED reg 02h (rc=%d) - no sample\n", (unsigned long)n, last_rc);
            else {
                float uv = ina226_shunt_uv(sh), mv = ina226_bus_mv(bus);
                printf("[PMID] #%lu Vshunt=%+.1f uV  Vbus=%.3f V  I~%+.4f A  ALERT=%s\n", (unsigned long)n,
                       (double)uv, (double)(mv / 1000.f), (double)(uv / 1e6f / SHUNT_NOMINAL_OHMS),
                       gpio_get(PIN_PM_ALERT) ? "high" : "LOW");
            }
        }
        if (n % 10 == 0)
            printf("[PMID] summary %s: identity=%s RESULT=%s CONFIG=%04X FE=%04X FF=%04X idle_low=%d\n", REV,
                   v == ID_PASS ? "PASS" : "FAIL", bad ? "FAIL" : "PASS", cfg, fe, ff, low);
        sleep_ms(1000);
    }
}
