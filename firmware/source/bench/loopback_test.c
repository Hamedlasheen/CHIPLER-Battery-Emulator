/*
 * Gate 2 bench test: isolated I2C master <-> RP2350 I2C target, SAME board, NO PHONE.
 * Wiring (only these, nothing else on the bus except optional INA228 @0x40):
 *     GP8 (I2C0 SDA, master)  <->  GP6 (I2C1 SDA, target)
 *     GP9 (I2C0 SCL, master)  <->  GP7 (I2C1 SCL, target)
 *     4.7k pull-up from each line to 3V3
 * Output (USB serial): per-test counts and a final PASS/FAIL line. Copy the log into the
 * bench test report. Uses the synthetic BENCH-r0 profile (not iPhone data).
 * v0.2.0-dev adds the CR-003 SOC snapshot test: a timer IRQ republishes 25/50/100 % synthetic
 * images every 250 us while the master reads the 4-byte block at 0x20; every read must be
 * internally consistent (units == soc*40, checksum ok) -> no torn old/new mix.
 *
 * GATE2_INIT-r2 = merge of Codex GATE2_INIT_r1 and Claude CR-005 (f13f382), bench only:
 *  Gate 2 runs 1-4 (2026-10-16) each reported exactly torn=1 of 3000. Both reviewers found the
 *  same cause: the first read finished before the timer's first publish, so 0x20 was unmapped
 *  (FF FF FF FF). Host-reproduced by test_snapshot_before_publish.
 *  From Codex r1: seed publish with checked soc_build/te_publish, timer-allocation check, abort if
 *  the target does not start, errors buffered and printed only after the timed loop, NAK vs
 *  TIMEOUT distinguished on the wrong-address probe, build_errors counter.
 *  From Claude CR-005: all-0xFF reads counted separately as NO_SNAPSHOT, pub/lat/ptr recorded
 *  per bad read, 10 records, 30000 reads, explicit note that 'ovf' is not a data error.
 *  Main firmware unchanged.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "board_config.h"
#include "hal/board_io.h"
#include "hal/battery_bus.h"
#include "batt/profile.h"
#include "batt/soc_ctrl.h"

static uint32_t rng = 0x12345678;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int run(uint32_t hz, int iters, uint8_t addr) {
    i2c_init(PM_I2C_INSTANCE, hz);
    gpio_set_function(PIN_PM_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_PM_SCL, GPIO_FUNC_I2C);
    int fail_ro = 0, fail_rw = 0, fail_nak = 0;
    uint64_t t0 = time_us_64();
    for (int k = 0; k < iters; k++) {
        /* read-only pattern: pointer write + repeated START + read 1..8 bytes */
        uint8_t reg = rnd() & 0x07, len = 1 + (rnd() % 8), buf[8];
        if (i2c_write_timeout_us(PM_I2C_INSTANCE, addr, &reg, 1, true, 5000) != 1) { fail_nak++; continue; }
        if (i2c_read_timeout_us(PM_I2C_INSTANCE, addr, buf, len, false, 5000) != len) { fail_nak++; continue; }
        for (int i = 0; i < len; i++) if (buf[i] != (uint8_t)((reg + i) ^ 0xA5)) { fail_ro++; break; }
        /* scratch write then read back */
        uint8_t w[5] = {(uint8_t)(0x80 + (rnd() & 7)), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd()};
        if (i2c_write_timeout_us(PM_I2C_INSTANCE, addr, w, 5, false, 5000) != 5) { fail_nak++; continue; }
        uint8_t rb[4];
        if (i2c_write_timeout_us(PM_I2C_INSTANCE, addr, w, 1, true, 5000) != 1 ||
            i2c_read_timeout_us(PM_I2C_INSTANCE, addr, rb, 4, false, 5000) != 4) { fail_nak++; continue; }
        if (memcmp(rb, w + 1, 4)) fail_rw++;
    }
    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000);
    target_engine_t *e = battery_bus_engine();
    printf("[BENCH] %lu Hz: iters=%d ro_mismatch=%d rw_mismatch=%d nak/timeout=%d time=%lu ms "
           "target: txns=%lu rx=%lu tx=%lu unknown=%lu ignored=%lu ovf=%lu\n",
           (unsigned long)hz, iters, fail_ro, fail_rw, fail_nak, (unsigned long)ms,
           (unsigned long)e->txns, (unsigned long)e->rx_bytes, (unsigned long)e->tx_bytes,
           (unsigned long)e->unknown_reads, (unsigned long)e->ignored_writes, (unsigned long)e->overflows);
    txn_rec_t r; while (te_pop(e, &r)) {}
    te_clear_counters(e);
    return fail_ro + fail_rw + fail_nak;
}

static volatile uint32_t s_pubs, s_pub_busy, s_build_errors;
static int s_soc_i;
static bool soc_timer_cb(repeating_timer_t *t) {
    const batt_profile_t *p = (const batt_profile_t *)t->user_data;
    uint8_t img[8], base, len;
    s_soc_i = (s_soc_i + 1) % SOC_N_PRESETS;
    if (!p->soc_build(k_soc_presets[s_soc_i], img, &base, &len)) {
        s_build_errors++;
        return true;
    }
    if (te_publish(battery_bus_engine(), base, img, len)) s_pubs++; else s_pub_busy++;
    return true;
}

static int soc_snapshot_test(uint8_t addr, const batt_profile_t *p, int iters) {
    /* The timer's first tick is in the future. Without this seed the first read
     * may see the unmapped 0x20 region (FF FF FF FF), not a torn snapshot. */
    target_engine_t *e = battery_bus_engine();
    uint8_t img[8], base, len;
    s_pubs = s_pub_busy = s_build_errors = 0;
    s_soc_i = 0;
    if (!p || !p->soc_build ||
        !p->soc_build(k_soc_presets[0], img, &base, &len) ||
        !te_publish(e, base, img, len)) {
        printf("[BENCH] FAIL: initial SOC snapshot not published; test not started\n");
        return 1;
    }
    printf("[BENCH] SOC seed: seq=%lu soc=%u (before timer/first read)\n",
           (unsigned long)te_snapshot_seq(e), (unsigned)img[0]);
    repeating_timer_t t;
    if (!add_repeating_timer_us(-250, soc_timer_cb, (void *)p, &t)) {
        printf("[BENCH] FAIL: SOC timer allocation failed; test not started\n");
        return 1;
    }
    /* Store a bounded sample; print only after the timed test to avoid changing
     * IRQ timing with USB output. Sequence is the published seq after the read,
     * not a claim about which snapshot the target latched. */
    struct { int iteration; uint8_t bytes[4]; uint32_t seq_after; uint8_t pub, lat, ptr; bool ff; } errors[10];
    int n_errors = 0;
    int torn = 0, nosnap = 0, nak = 0, seen[3] = {0};
    for (int k = 0; k < iters; k++) {
        uint8_t reg = 0x20, b[4];
        if (i2c_write_timeout_us(PM_I2C_INSTANCE, addr, &reg, 1, true, 5000) != 1 ||
            i2c_read_timeout_us(PM_I2C_INSTANCE, addr, b, 4, false, 5000) != 4) { nak++; continue; }
        uint16_t units = b[1] | (b[2] << 8);
        bool ok = soc_valid(b[0]) && units == b[0] * 40 && b[3] == (uint8_t)(b[0] + b[1] + b[2]);
        if (!ok) {
            bool ff = b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF && b[3] == 0xFF;
            if (ff) nosnap++; else torn++;
            if (n_errors < (int)(sizeof errors / sizeof errors[0])) {
                errors[n_errors].iteration = k;
                memcpy(errors[n_errors].bytes, b, sizeof b);
                errors[n_errors].seq_after = te_snapshot_seq(e);
                errors[n_errors].pub = e->pub; errors[n_errors].lat = e->lat; errors[n_errors].ptr = e->ptr;
                errors[n_errors].ff = ff;
                n_errors++;
            }
        }
        else seen[b[0] == 25 ? 0 : (b[0] == 50 ? 1 : 2)]++;
    }
    cancel_repeating_timer(&t);
    printf("[BENCH] SOC snapshot: reads=%d torn=%d no_snapshot=%d nak/timeout=%d seen25=%d seen50=%d seen100=%d "
           "publishes=%lu deferred=%lu build_errors=%lu\n", iters, torn, nosnap, nak, seen[0], seen[1], seen[2],
           (unsigned long)s_pubs, (unsigned long)s_pub_busy, (unsigned long)s_build_errors);
    for (int i = 0; i < n_errors; i++) {
        printf("[BENCH] SOC invalid: iteration=%d kind=%s bytes=%02X %02X %02X %02X published_seq_after=%lu pub=%u lat=%u ptr=0x%02X\n",
               errors[i].iteration, errors[i].ff ? "NO_SNAPSHOT" : "INCONSISTENT", errors[i].bytes[0], errors[i].bytes[1],
               errors[i].bytes[2], errors[i].bytes[3], (unsigned long)errors[i].seq_after,
               errors[i].pub, errors[i].lat, errors[i].ptr);
    }
    return torn + nosnap + nak + (s_build_errors ? 1 : 0) + (seen[0] && seen[1] && seen[2] ? 0 : 1);
}

int main(void) {
    board_io_init();            /* phone output forced OFF */
    battery_bus_init();
    stdio_init_all();
    sleep_ms(3000);             /* time to open the serial monitor */
    const batt_profile_t *p = profile_get(profile_find("BENCH-r0"));
    printf("\n[BENCH] CHIPLER Gate 2 INIT-r2 (Codex r1 + Claude CR-005 merged; bench only; no phone)\n");
    printf("[BENCH] note: 'ovf' = target log ring full (not drained during a run); NOT a data error, not counted\n");
    if (!p || !battery_bus_start(p)) {
        printf("[BENCH] RESULT: FAIL (target did not start)\n");
        while (true) { sleep_ms(1000); }
    }
    printf("[BENCH] Profile %s addr 0x%02X\n", p->id, p->i2c_addr);
    int bad = 0;
    bad += run(100000, 500, p->i2c_addr);
    bad += run(400000, 2000, p->i2c_addr);
    i2c_init(PM_I2C_INSTANCE, 400000);
    bad += soc_snapshot_test(p->i2c_addr, p, 30000);
    /* negative test: wrong address must NAK */
    uint8_t z = 0;
    int r = i2c_write_timeout_us(PM_I2C_INSTANCE, 0x56, &z, 1, false, 5000);
    printf("[BENCH] wrong-address probe (0x56): %s (rc=%d)\n",
           r == PICO_ERROR_GENERIC ? "NAK as expected" :
           (r == PICO_ERROR_TIMEOUT ? "TIMEOUT - not a confirmed NAK" : "UNEXPECTED RESPONSE"), r);
    if (r != PICO_ERROR_GENERIC) bad++;
    printf("[BENCH] RESULT: %s (%d errors)\n", bad ? "FAIL" : "PASS", bad);
    while (true) { sleep_ms(1000); }
}
