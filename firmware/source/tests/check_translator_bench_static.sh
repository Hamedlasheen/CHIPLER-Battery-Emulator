#!/usr/bin/env bash
# Translator pulse bench: static proof that the image can only pull LOW / release on GP6/GP7 and touches no other pin.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/bench/translator_pulse_bench.c"; H="$ROOT/bench/translator_pulse_plan.h"
fail=0; say() { printf '%s\n' "$*"; }
# code only (strip comments) so documentation words do not count
code="$(python -I "$ROOT/tests/strip_c_comments.py" "$B")"
chk() { # name, regex-that-must-NOT-match
  if printf '%s\n' "$code" | grep -nE "$2" >/dev/null; then say "FAIL $1:"; printf '%s\n' "$code" | grep -nE "$2"; fail=1; else say "PASS $1"; fi; }
chk "no gpio_put (no GPIO output value is ever written)"        'gpio_put|gpio_set_dir_out|gpio_set_outover|gpio_set_oeover'
chk "no GPIO_OUT direction (direction is only PIO pindirs)"      'GPIO_OUT|gpio_set_dir'
chk "no SIO output register access"                              'sio_hw|gpio_oe_set|gpio_out_set|gpio_set_mask|gpio_xor|gpio_put_masked'
chk "no PIO write of a 1 / OUT / SET PINS instruction"           'pio_encode_set\(pio_pins|pio_encode_out|pio_sm_put|pio_sm_exec'
chk "no internal pull-ups (external pull-ups define HIGH)"       'gpio_pull_up|gpio_pull_down|gpio_set_pulls'
chk "no other pin numbers (display/INA226/battery IRQ pins)"     'PIN_LCD|PIN_PM|PIN_BAT|PIN_TOUCH|spi_|i2c_|adc_|GP(8|9|10|26|27|28|29)\b'
# the only latch write must be the literal value 0
n=$(printf '%s\n' "$code" | grep -c 'pio_sm_set_pins_with_mask')
z=$(printf '%s\n' "$code" | grep -c 'pio_sm_set_pins_with_mask(s_pio, (uint)s_sm, 0u,')
if [ "$n" = "1" ] && [ "$z" = "1" ]; then say "PASS the only output-latch write is the literal 0"; else say "FAIL latch write count=$n literal0=$z"; fail=1; fi
# plan header: pins 6 and 7 only
if grep -qE 'TP_PIN_CH0 6u' "$H" && grep -qE 'TP_PIN_CH1 7u' "$H"; then say "PASS channel pins are GP6 and GP7"; else say "FAIL channel pins"; fail=1; fi
exit $fail
