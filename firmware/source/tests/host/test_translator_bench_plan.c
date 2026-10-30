/* Translator pulse bench (bench/translator_pulse_plan.h): the PIO program can only pull LOW / release, the dividers give the
 * requested frequencies, one channel per step in the requested order. Host model only; the pulses on the pins are NOT RUN. */
#include <stdio.h>
#include <stdint.h>
#include "../../bench/translator_pulse_plan.h"

static unsigned checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
    /* 1. the program: only SET instructions, only to PINDIRS, data 1 (drive LOW) then 0 (release); never a write to PINS */
    const unsigned n = sizeof tp_prog / sizeof tp_prog[0];
    CHECK(n == 2);
    for (unsigned i = 0; i < n; i++) {
        uint16_t w = tp_prog[i];
        CHECK((w >> 13) == 7u);                    /* 111 = SET */
        CHECK(((w >> 5) & 7u) == TP_DEST_PINDIRS); /* dest = PINDIRS, never PINS (000) */
        CHECK(((w >> 5) & 7u) != TP_DEST_PINS);
        CHECK((w & 0x1Fu) <= 1u);                  /* data 0/1 on pindirs only */
        CHECK(((w >> 8) & 0x1Fu) == 31u);          /* 32 cycles each: symmetric 50 % */
    }
    CHECK((tp_prog[0] & 0x1Fu) == 1u && (tp_prog[1] & 0x1Fu) == 0u);   /* LOW (output enable) first, then release */
    CHECK(tp_prog[0] == 0xFF81u && tp_prog[1] == 0xFF80u);

    /* 2. frequencies */
    const uint32_t sys[] = {150000000u, 125000000u, 133000000u, 200000000u};
    for (unsigned s = 0; s < 4; s++) {
        for (unsigned k = 0; k < 4; k++) {
            uint32_t f = tp_seq[k].hz, q = tp_div_q8(sys[s], f), a = tp_actual_hz(sys[s], q);
            double err = ((double)a - (double)f) / (double)f;
            if (s == 0) printf("  sys=%u target=%u div=%u.%02u actual=%u\n", sys[s], f, q >> 8, ((q & 255u) * 100u) / 256u, a);
            CHECK(q >= 256u && q < (65536u << 8));
            CHECK(err < 0.005 && err > -0.005);
        }
    }
    CHECK(tp_actual_hz(150000000u, tp_div_q8(150000000u, 100000u)) == 100000u);
    CHECK(tp_actual_hz(150000000u, tp_div_q8(150000000u, 400000u)) == 400000u);

    /* 3. plan: one channel at a time, 100 kHz then 400 kHz, GP6 then GP7 */
    CHECK(tp_seq[0].ch == 0 && tp_seq[0].hz == 100000u && tp_seq[1].ch == 0 && tp_seq[1].hz == 400000u);
    CHECK(tp_seq[2].ch == 1 && tp_seq[2].hz == 100000u && tp_seq[3].ch == 1 && tp_seq[3].hz == 400000u);
    CHECK(TP_PIN_CH0 == 6u && TP_PIN_CH1 == 7u);
    CHECK(TP_PIN_CH0 != 8u && TP_PIN_CH1 != 8u && TP_PIN_CH0 != 9u && TP_PIN_CH1 != 9u && TP_PIN_CH0 != 10u && TP_PIN_CH1 != 10u);   /* INA226 GP8/9/10 untouched */
    CHECK(TP_STEP_SECONDS >= 5u && TP_GAP_SECONDS >= 1u);

    printf("translator_bench_plan: %u checks, %u failures -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
