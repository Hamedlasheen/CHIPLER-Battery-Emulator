/* CHIPLER P1 - BENCH ONLY: I2C level-translator EDGE-SPEED test on GP6 / GP7 (the 3.3 V side of the 1.8 V<->3.3 V translator).
 * Standalone image: no display, no touch, no INA226 (GP8/GP9/GP10 are NOT touched), no I2C, no SMBus engine. Phone DISCONNECTED.
 *
 * What it does: one channel at a time, 100 kHz then 400 kHz, GP6 then GP7. It generates OPEN-DRAIN pulses with a PIO state
 * machine: pull the line LOW, release it (high impedance), repeat at 50 % duty. It never drives HIGH: the pad output latch is
 * written once with 0 and the PIO program only executes `set pindirs` (see translator_pulse_plan.h and the host test that
 * decodes the program). The high level and its rise time come from the external pull-ups / translator - that is exactly what
 * is being measured (with a scope or logic analyser on the translator's 3.3 V side and 1.8 V side).
 *
 * Serial (USB CDC, 115200 irrelevant): every step prints the channel and the frequency.
 *   boot: 5 s countdown, then automatic sequence GP6@100k, GP6@400k, GP7@100k, GP7@400k, 10 s each, 1 s released gap.
 *   keys: a = (re)start auto sequence   1..4 = hold one step until another key   x = release both pins now   h = help
 * HARDWARE NOT RUN. This image proves nothing about the phone; it only generates stimulus for the owner's measurement.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "translator_pulse_plan.h"

static const pio_program_t k_prog = { .instructions = tp_prog, .length = 2, .origin = -1
#if PICO_PIO_VERSION > 0
    , .pio_version = 0, .used_gpio_ranges = 0x1      /* GP0..GP15 */
#endif
};
static PIO s_pio;
static int s_sm = -1;
static uint s_off;
static int s_pin = -1;                               /* pin currently pulsed, -1 = both released */

static uint pin_of(uint8_t ch) { return ch == 0 ? TP_PIN_CH0 : TP_PIN_CH1; }

/* Both lines hi-Z, SIO input, no internal pulls (the external pull-ups define the high level). */
static void release_all(void) {
    if (s_sm >= 0) {
        pio_sm_set_enabled(s_pio, (uint)s_sm, false);
        pio_sm_set_pindirs_with_mask(s_pio, (uint)s_sm, 0u, (1u << TP_PIN_CH0) | (1u << TP_PIN_CH1));   /* output enable off */
    }
    for (uint8_t ch = 0; ch < 2; ch++) {
        uint p = pin_of(ch);
        gpio_init(p);                                 /* SIO function, input, output latch 0 */
        gpio_disable_pulls(p);
    }
    s_pin = -1;
}

static void start_pulses(uint8_t ch, uint32_t hz) {
    release_all();
    uint pin = pin_of(ch);
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t q8 = tp_div_q8(sys, hz);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, s_off, s_off + 1);
    sm_config_set_set_pins(&c, pin, 1);
    sm_config_set_clkdiv_int_frac(&c, (uint16_t)(q8 >> 8), (uint8_t)(q8 & 0xFFu));
    pio_sm_init(s_pio, (uint)s_sm, s_off, &c);
    pio_sm_set_pindirs_with_mask(s_pio, (uint)s_sm, 0u, 1u << pin);        /* input: pad NOT driven */
    pio_sm_set_pins_with_mask(s_pio, (uint)s_sm, 0u, 1u << pin);           /* output latch = 0, forever; no instruction ever writes 1 */
    pio_gpio_init(s_pio, pin);
    pio_sm_set_enabled(s_pio, (uint)s_sm, true);
    s_pin = (int)pin;
    printf("[TRANS] ch=GP%u f=%luHz START open-drain LOW pulses, line released between (sys=%luHz pio_div=%lu.%02lu actual=%luHz)\n",
           pin, (unsigned long)hz, (unsigned long)sys, (unsigned long)(q8 >> 8), (unsigned long)(((q8 & 0xFF) * 100u) / 256u),
           (unsigned long)tp_actual_hz(sys, q8));
}

static void help(void) {
    printf("[TRANS] keys: a=auto sequence  1=GP6@100k 2=GP6@400k 3=GP7@100k 4=GP7@400k (hold)  x=release both  h=help. Phone must be DISCONNECTED. Open-drain only: never drives HIGH.\n");
}

typedef enum { M_COUNTDOWN, M_STEP, M_GAP, M_HOLD, M_IDLE } mode_t_;

int main(void) {
    stdio_init_all();
    s_pio = pio0;
    s_sm = pio_claim_unused_sm(s_pio, true);
    s_off = (uint)pio_add_program(s_pio, &k_prog);
    release_all();                                    /* before anything else: both lines released */
    sleep_ms(1500);
    printf("\n[TRANS] CHIPLER P1 translator edge-speed bench (open-drain pulses on GP6/GP7). HARDWARE NOT RUN. Display/INA226 pins untouched. Phone DISCONNECTED.\n");
    help();
    mode_t_ mode = M_COUNTDOWN;
    int step = 0;
    uint32_t t0 = to_ms_since_boot(get_absolute_time()), last_print = t0;
    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        int c = getchar_timeout_us(0);
        if (c == 'x') { release_all(); mode = M_IDLE; printf("[TRANS] RELEASED both GP6 and GP7 (hi-Z)\n"); }
        else if (c == 'a') { release_all(); mode = M_COUNTDOWN; step = 0; t0 = now; last_print = now; printf("[TRANS] auto sequence restarts in %us\n", TP_START_DELAY_SECONDS); }
        else if (c >= '1' && c <= '4') { step = c - '1'; start_pulses(tp_seq[step].ch, tp_seq[step].hz); mode = M_HOLD; t0 = now; last_print = now; }
        else if (c == 'h') help();

        uint32_t el = now - t0;
        switch (mode) {
        case M_COUNTDOWN:
            if (now - last_print >= 1000u) { last_print = now; printf("[TRANS] start in %lus (lines released)\n", (unsigned long)(TP_START_DELAY_SECONDS - (el / 1000u > TP_START_DELAY_SECONDS ? TP_START_DELAY_SECONDS : el / 1000u))); }
            if (el >= TP_START_DELAY_SECONDS * 1000u) { start_pulses(tp_seq[step].ch, tp_seq[step].hz); mode = M_STEP; t0 = now; last_print = now; }
            break;
        case M_STEP:
            if (now - last_print >= 1000u) { last_print = now;
                printf("[TRANS] ch=GP%u f=%luHz running %lu/%us\n", pin_of(tp_seq[step].ch), (unsigned long)tp_seq[step].hz, (unsigned long)(el / 1000u), TP_STEP_SECONDS); }
            if (el >= TP_STEP_SECONDS * 1000u) {
                release_all();
                printf("[TRANS] ch=GP%u f=%luHz DONE, lines released\n", pin_of(tp_seq[step].ch), (unsigned long)tp_seq[step].hz);
                step++; mode = step >= 4 ? M_IDLE : M_GAP; t0 = now; last_print = now;
                if (mode == M_IDLE) printf("[TRANS] SEQUENCE DONE (GP6 100k/400k, GP7 100k/400k). Send 'a' to repeat, 1..4 to hold one step.\n");
            }
            break;
        case M_GAP:
            if (el >= TP_GAP_SECONDS * 1000u) { start_pulses(tp_seq[step].ch, tp_seq[step].hz); mode = M_STEP; t0 = now; last_print = now; }
            break;
        case M_HOLD:
            if (now - last_print >= 1000u) { last_print = now;
                printf("[TRANS] ch=GP%u f=%luHz HOLD %lus (x = release)\n", pin_of(tp_seq[step].ch), (unsigned long)tp_seq[step].hz, (unsigned long)(el / 1000u)); }
            break;
        case M_IDLE: break;
        }
        sleep_ms(5);
    }
}
