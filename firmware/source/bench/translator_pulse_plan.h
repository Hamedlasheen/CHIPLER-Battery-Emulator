/* Translator pulse bench: the parts that are pure data/arithmetic, shared by the bench image and the host test
 * (tests/host/test_translator_bench_plan.c). No SDK headers here.
 *
 * OPEN-DRAIN BY CONSTRUCTION: the PIO output latch of the pin is written ONCE with 0 and the program only ever executes
 * `set pindirs, 1` (output enable = pull LOW) and `set pindirs, 0` (release = high impedance). There is no instruction that
 * writes a 1 to the pin, so the pad can never drive HIGH. The external pull-ups make the high level.
 */
#ifndef CHIPLER_TRANSLATOR_PULSE_PLAN_H
#define CHIPLER_TRANSLATOR_PULSE_PLAN_H
#include <stdint.h>

/* PIO SET instruction: 111 | delay[4:0] | dest[2:0] | data[4:0]; dest 100 = PINDIRS */
#define TP_SET(delay, dest, data) ((uint16_t)(0xE000u | ((uint32_t)(delay) << 8) | ((uint32_t)(dest) << 5) | (uint32_t)(data)))
#define TP_DEST_PINS    0u
#define TP_DEST_PINDIRS 4u
static const uint16_t tp_prog[2] = {
    TP_SET(31, TP_DEST_PINDIRS, 1),     /* drive LOW (output enabled, latch = 0), 32 PIO cycles */
    TP_SET(31, TP_DEST_PINDIRS, 0)      /* release (hi-Z), 32 PIO cycles */
};
#define TP_CYCLES_PER_PERIOD 64u        /* 2 instructions x 32 cycles; 50 % low / 50 % released */

typedef struct { uint8_t ch; uint32_t hz; } tp_step_t;
#define TP_PIN_CH0 6u                   /* GP6 = battery-bus SDA side (3.3 V side of the translator) */
#define TP_PIN_CH1 7u                   /* GP7 = battery-bus SCL side */
static const tp_step_t tp_seq[4] = { {0, 100000u}, {0, 400000u}, {1, 100000u}, {1, 400000u} };   /* one channel at a time */
#define TP_STEP_SECONDS 10u
#define TP_GAP_SECONDS 1u
#define TP_START_DELAY_SECONDS 5u

/* PIO clock divider in 16.8 fixed point (value = div * 256), rounded to nearest. */
static inline uint32_t tp_div_q8(uint32_t sys_hz, uint32_t f_hz) {
    uint64_t den = (uint64_t)f_hz * TP_CYCLES_PER_PERIOD;
    return (uint32_t)(((uint64_t)sys_hz * 256u + den / 2u) / den);
}
/* frequency the divider really produces */
static inline uint32_t tp_actual_hz(uint32_t sys_hz, uint32_t div_q8) {
    uint64_t den = (uint64_t)div_q8 * TP_CYCLES_PER_PERIOD;
    return (uint32_t)(((uint64_t)sys_hz * 256u + den / 2u) / den);
}
#endif
