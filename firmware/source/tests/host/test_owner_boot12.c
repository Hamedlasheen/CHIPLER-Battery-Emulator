/* OWNER PHONE-TEST: every iPhone-12-family DATA TEST profile must serve the recorded 12 Pro boot commands, and a
 * byte-count probe of any BLOCK register (read count, NACK, STOP) must not fault the target - the iPhone does it
 * before 0x7E/0x21/0x90/0x91/0x95/0x76/0x9E/0xA8 at boot (first_burst_4.13s.txt). */
#include "../../src/batt/smbus_target.h"
#include "../../src/batt/live_prepare.h"
#include "../../src/batt/live_wire.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, fails, masked;
#define C(x) do{checks++;if(!(x)){fails++;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
static uint32_t guard(void *p){(void)p;unsigned o=masked;masked=1;return o;}
static void unguard(void *p,uint32_t v){(void)p;masked=v;}
static uint8_t crc8(const uint8_t *p, unsigned n) {
    uint8_t c = 0;
    for (unsigned i = 0; i < n; i++) { c ^= p[i]; for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1)); }
    return c;
}
/* full WORD read with PEC check */
static bool rd_word(st_target_t *t, uint8_t cmd) {
    uint8_t d[2], pec, pre[5] = {0x16, cmd, 0x17};
    if (!st_on_rx(t, cmd) || !st_begin_read(t)) return false;
    if (!st_on_tx(t, &d[0]) || !st_on_tx(t, &d[1]) || !st_on_tx(t, &pec)) return false;
    st_on_nack(t); if (st_on_stop(t) != ST_OK) return false;
    pre[3] = d[0]; pre[4] = d[1];
    return pec == crc8(pre, 5);
}
/* count-only probe: returns the stop result */
static st_result_t probe_block(st_target_t *t, uint8_t cmd, uint8_t *count) {
    if (!st_on_rx(t, cmd) || !st_begin_read(t)) return ST_UNSUPPORTED;
    if (!st_on_tx(t, count)) return ST_UNSUPPORTED;
    st_on_nack(t); return st_on_stop(t);
}
/* full BLOCK read with PEC check */
static bool rd_block(st_target_t *t, uint8_t cmd, uint8_t expect_count) {
    uint8_t pre[3 + 1 + 32], n = 0, b, cnt, pec;
    pre[n++] = 0x16; pre[n++] = cmd; pre[n++] = 0x17;
    if (!st_on_rx(t, cmd) || !st_begin_read(t)) return false;
    if (!st_on_tx(t, &cnt) || cnt != expect_count) return false;
    pre[n++] = cnt;
    for (unsigned k = 0; k < cnt; k++) { if (!st_on_tx(t, &b)) return false; pre[n++] = b; }
    if (!st_on_tx(t, &pec)) return false;
    st_on_nack(t); if (st_on_stop(t) != ST_OK) return false;
    return pec == crc8(pre, n);
}
int main(void) {
    static const char *ids[] = {"P12m-B2", "P12-B2", "P12P-B2", "P12PM-B2", "P12P-C1"};
    static const uint8_t words[] = {0x09, 0x08, 0x0D, 0x0F, 0x10, 0x18, 0x7A, 0x7B, 0x1B, 0x17};
    static const uint8_t blocks[] = {0x7E, 0x21, 0x90, 0x91, 0x95, 0x76, 0x9E, 0xA8, 0xAF, 0x4C};
    for (unsigned m = 0; m < sizeof ids / sizeof ids[0]; m++) {
        const batt_profile_t *p = profile_get(profile_find(ids[m]));
        C(p && profile_12p_capture_state(p) == 1);
        if (!p) continue;
        st_target_t t; st_init(&t, 0x0B); C(st_set_guard(&t, guard, unguard, 0));
        C(live_prepare(&t, p, 50) && st_live_ready(&t));
        unsigned ok_w = 0, ok_b = 0;
        for (unsigned k = 0; k < sizeof words; k++) { bool r = rd_word(&t, words[k]); C(r); ok_w += r; }
        for (unsigned k = 0; k < sizeof blocks; k++) {
            uint8_t cnt = 0;
            C(probe_block(&t, blocks[k], &cnt) == ST_SHORT_A8);   /* count probe accepted, target not faulted */
            bool r = rd_block(&t, blocks[k], cnt); C(r); ok_b += r;
        }
        printf("%-9s words %u/%zu blocks(probe+full) %u/%zu\n", ids[m], ok_w, sizeof words, ok_b, sizeof blocks);
    }
    /* a truncated read that is NOT a count probe is still rejected */
    {
        const batt_profile_t *p = profile_get(profile_find("P12P-C1"));
        st_target_t t; st_init(&t, 0x0B); C(st_set_guard(&t, guard, unguard, 0)); C(live_prepare(&t, p, 50));
        uint8_t b;
        C(st_on_rx(&t, 0x7E) && st_begin_read(&t) && st_on_tx(&t, &b) && st_on_tx(&t, &b));
        st_on_nack(&t); C(st_on_stop(&t) == ST_BAD_LENGTH);
    }
    /* r5: unknown read (0x60, seen x29 on the bench) answers 0xFF on every byte and ends OK; the next read still works */
    {
        const batt_profile_t *p = profile_get(profile_find("P12P-C1"));
        st_target_t t; st_init(&t, 0x0B); C(st_set_guard(&t, guard, unguard, 0)); C(live_prepare(&t, p, 50));
        uint8_t b = 0; unsigned ff = 0;
        C(st_on_rx(&t, 0x60) && st_begin_read(&t));
        for (int k = 0; k < 40; k++) { C(st_on_tx(&t, &b)); ff += b == 0xFF; }   /* longer than any block: no overread */
        C(ff == 40);
        st_on_nack(&t); C(st_on_stop(&t) == ST_OK); C(t.unknown_reads == 1);
        C(rd_word(&t, 0x09));
        /* unknown write (0x7A seen x4) with valid PEC: dropped, not fatal */
        uint8_t body[2] = {0x01, 0x00};
        uint8_t pec = st_pec_write(0x0B, 0x7A, body, 2);
        C(st_on_rx(&t, 0x7A) && st_on_rx(&t, body[0]) && st_on_rx(&t, body[1]) && st_on_rx(&t, pec));
        C(st_on_stop(&t) == ST_OK); C(t.writes_bad == 1);
        /* write queue full under a burst of 0x65 page selects: still OK on the wire */
        unsigned okw = 0;
        for (int k = 0; k < 40; k++) {
            uint8_t sel[2] = {(uint8_t)(k % 16), 0x00}; uint8_t sp = st_pec_write(0x0B, 0x65, sel, 2);
            if (st_on_rx(&t, 0x65) && st_on_rx(&t, sel[0]) && st_on_rx(&t, sel[1]) && st_on_rx(&t, sp) && st_on_stop(&t) == ST_OK) okw++;
        }
        C(okw == 40); C(t.queue_dropped > 0);
        C(rd_word(&t, 0x08));
    }
    /* r5: idle expire must not fire when an IRQ stamped last_us after the caller sampled `now` */
    {
        const batt_profile_t *p = profile_get(profile_find("P12P-C1"));
        st_target_t t; st_init(&t, 0x0B); C(st_set_guard(&t, guard, unguard, 0)); C(live_prepare(&t, p, 50));
        live_wire_t w; live_wire_init(&w, &t, 1000);
        C(st_on_rx(&t, 0x09));                       /* engine active (mid-transaction) */
        w.last_us = 5000;                            /* IRQ stamped after now=4990 was sampled */
        C(!live_wire_expire(&w, 4990, 100000) && !w.blocked);
        C(!live_wire_expire(&w, 5000 + 100000, 100000));
        C(live_wire_expire(&w, 5000 + 100001, 100000) && w.blocked && w.site == 8);
    }
    printf("owner_boot12: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
