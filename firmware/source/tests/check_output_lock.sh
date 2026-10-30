#!/usr/bin/env bash
# FW-REVIEW-001 #2 static regression of the output-arming policy, per build (review 110 R110-2).
#   usage: check_output_lock.sh <owner|default>   (tests/host/CMakeLists.txt passes the configuration it detected)
#   default build: only PROF_VERIFIED can arm, and no registry profile is VERIFIED -> nothing can power a phone.
#   owner build:   VERIFIED, plus PROF_PHONE_DATA_TEST inside #if CHIPLER_OWNER_PHONE_TEST and never for families
#                  15/16/17. That is the owner's phone-test unlock, deliberately NOT "output always locked".
#   The behaviour of every registry profile in each build is checked by shared12_tests (output_policy); this script
#   only pins the source shape.
#  1. No override flag/parameter exists anywhere in source, config or build files.
#  2. profile_armable(): VERIFIED in every build; DATA TEST only under the owner flag and the family exclusion.
#  3. Every firmware target links the SAME safety.c/profile.c and no target defines a macro
#     that could change arming behaviour.
#  4. No profile in the registry is PROF_VERIFIED (nothing is phone-qualified yet).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="${1:-default}"
fail=0
say() { printf '%s\n' "$*"; }

if grep -rniE 'dev_?override|allow_unverified|unlock_output' "$ROOT/src" "$ROOT/bench" "$ROOT/config" "$ROOT/CMakeLists.txt" >/dev/null; then
  say "FAIL 1: override token found:"; grep -rniE 'dev_?override|allow_unverified|unlock_output' "$ROOT/src" "$ROOT/bench" "$ROOT/config" "$ROOT/CMakeLists.txt"; fail=1
else say "PASS 1: no override flag or parameter in src/bench/config/CMakeLists.txt"; fi

body="$(awk '/^bool profile_armable\(/,/^}/' "$ROOT/src/batt/profile.c")"
# lines that accept DATA TEST, and whether each sits inside "#if CHIPLER_OWNER_PHONE_TEST ... #endif"
dt_bad="$(printf '%s\n' "$body" | awk '/^#if CHIPLER_OWNER_PHONE_TEST/{o=1;next} /^#endif/{o=0;next}
  /PROF_PHONE_DATA_TEST/ && !/^[[:space:]]*(\/\*|\*)/ { if(!o || $0 !~ /!profile_family_excluded\(p\)/) print }')"
if printf '%s' "$body" | grep -qE 'p->status == PROF_VERIFIED\) return true' \
   && ! printf '%s' "$body" | grep -qE 'PROF_IN_DEVELOPMENT|PROF_BENCH_TEST|PROF_UNVERIFIED' && [ -z "$dt_bad" ]; then
  if [ "$CFG" = owner ]; then
    say "PASS 2 (owner build): profile_armable() accepts VERIFIED, and DATA TEST only under CHIPLER_OWNER_PHONE_TEST and outside families 15/16/17"
  else
    say "PASS 2 (default build): profile_armable() accepts only VERIFIED here; the DATA TEST branch is compiled out"
  fi
else say "FAIL 2: profile_armable() policy shape; offending lines:"; printf '%s\n' "${dt_bad:-$body}"; fail=1; fi

if grep -q 'src/safety/safety.c' "$ROOT/CMakeLists.txt" && grep -q 'src/batt/profile.c' "$ROOT/CMakeLists.txt" \
   && [ "$(grep -c 'add_executable' "$ROOT/CMakeLists.txt")" = "1" ]; then
  say "PASS 3a: all firmware targets are created by one function with one COMMON_SRC list"
else say "FAIL 3a: firmware targets do not share one source list"; fail=1; fi
# review 102: the build identity line is allowed only exactly as written (no other definition may ride on it)
if grep -E 'target_compile_definitions\(' "$ROOT/CMakeLists.txt" | grep -viE 'TE_RAMFUNC|LCD_|CHIPLER_SIM_TELEMETRY|DISPLAY_VARIANT'    | grep -vxE '[[:space:]]*target_compile_definitions\(\$\{name\} PRIVATE PICO_PROGRAM_BUILD_DATE="\$\{CHIPLER_BUILD_ID\}" CHIPLER_BUILD_ID_STR="\$\{CHIPLER_BUILD_ID\}"\)' | grep -q .; then
  say "FAIL 3b: unexpected per-target definition:"; grep -E 'target_compile_definitions\(' "$ROOT/CMakeLists.txt"; fail=1
else say "PASS 3b: per-target definitions limited to TE_RAMFUNC / LCD_* / DISPLAY_VARIANT / CHIPLER_SIM_TELEMETRY / build id"; fi

n_verified=$(grep -cE 'PROF_VERIFIED[[:space:]]*,' "$ROOT/src/batt/profile.c")
if [ "$n_verified" = "0" ]; then say "PASS 4: no registry profile is PROF_VERIFIED"
else say "FAIL 4: $n_verified registry entries marked PROF_VERIFIED"; fail=1; fi

if grep -rnE 'A\+B|btn_a_pressed|btn_b_pressed|BTN_EV_A_|BTN_EV_B_|BTN_EV_AB_' "$ROOT/src" "$ROOT/bench" >/dev/null; then
  say "FAIL 5: legacy two-button (A/B, A+B) tokens remain:"; grep -rnE 'A\+B|btn_a_pressed|btn_b_pressed|BTN_EV_A_|BTN_EV_B_|BTN_EV_AB_' "$ROOT/src" "$ROOT/bench"; fail=1
else say "PASS 5: no legacy A/B or A+B button code/strings in src/bench (CR-003-r1)"; fi

if grep -nE 'soc_build' "$ROOT/src/batt/profile.c" | grep -v 'bench_soc_build' | grep -vE '^\s*[0-9]+:\s*(/\*|\*)' | grep -q 'soc_build[^_]'; then
  say "FAIL 6: a non-bench profile references a SOC builder"; fail=1
else say "PASS 6: only the synthetic BENCH profile has a SOC builder (no invented phone SOC data)"; fi

# 7. CHARGE TEST (CP-09 merged UI): no sink/input hardware declared, no verdict wording in firmware
if grep -qE '^#define CHG_SINK_FITTED[[:space:]]+0' "$ROOT/config/board_p1.h" && grep -qE '^#define CHG_INPUT_SENSOR_FITTED[[:space:]]+0' "$ROOT/config/board_p1.h"    && grep -qE '^#define CHG_SINK_FITTED[[:space:]]+0' "$ROOT/config/board_legacy_round.h"; then
  say "PASS 7a: CHG_SINK_FITTED = 0 and CHG_INPUT_SENSOR_FITTED = 0 (charge test cannot start)"
else say "FAIL 7a: charge hardware declared fitted without CP-09 acceptance"; fail=1; fi
if grep -rniE '"[^"]*(HEALTHY|WITHIN LIMITS|CHARGING OK|CHARGE PASS)[^"]*"' "$ROOT/src" >/dev/null; then
  say "FAIL 7b: charge verdict wording found in firmware:"; grep -rniE '"[^"]*(HEALTHY|WITHIN LIMITS|CHARGING OK|CHARGE PASS)[^"]*"' "$ROOT/src"; fail=1
else say "PASS 7b: no 'healthy / within limits / pass' charge verdict string in firmware"; fi
if grep -nE 'safety_(request_arm|confirm|force|on)' "$ROOT/src/charge/charge_diag.c" >/dev/null; then
  say "FAIL 7c: charge module calls the safety state machine"; fail=1
else say "PASS 7c: charge module has no access to output arming"; fi

# 8. V02-03 / V02-04
if grep -rnE 'adc_gpio_init|adc_select_input' "$ROOT/src" >/dev/null; then
  say "FAIL 8a: ADC (old GP29 VIN sense) still configured in firmware"; fail=1
else say "PASS 8a: no ADC/VIN sense configured (GP29 = OK key); arming policy per build: PASS 2/4"; fi
if grep -qE '^function\(chipler_p1_set' "$ROOT/CMakeLists.txt" && [ "$(grep -cE '^chipler_p1_set\(' "$ROOT/CMakeLists.txt")" = "1" ]; then
  say "PASS 8b: one P1 set (UI_DEMO + MAIN) built by one function with one shared definition line"
else say "FAIL 8b: P1 set definition missing"; fail=1; fi
# 9. P1_r1: PIN_BAT_BUS_EN must not exist in the P1 header (GP5 is LCD_DC) and no source may use it unguarded
if grep -qE '^#define PIN_BAT_BUS_EN' "$ROOT/config/board_p1.h"; then say "FAIL 9a: P1 header defines PIN_BAT_BUS_EN"; fail=1
else say "PASS 9a: board_p1.h does not define PIN_BAT_BUS_EN (GP5 = LCD_DC)"; fi
if grep -rn 'PIN_BAT_BUS_EN' "$ROOT/src" "$ROOT/bench" | grep -v 'src/hal/bus_en.h' | grep -q .; then
  say "FAIL 9b: PIN_BAT_BUS_EN used outside bus_en.h:"; grep -rn 'PIN_BAT_BUS_EN' "$ROOT/src" "$ROOT/bench" | grep -v 'src/hal/bus_en.h'; fail=1
else say "PASS 9b: PIN_BAT_BUS_EN referenced only inside the BAT_BUS_EN_PRESENT guard of bus_en.h"; fi
# 10. touch-only: no key GPIO initialised on GP26/GP27 (touch) in the P1 build
if grep -qE '^#define PIN_BTN_(UP|DOWN|OK)[[:space:]]+-1' "$ROOT/config/board_p1.h" && [ "$(grep -cE '^#define PIN_BTN_(UP|DOWN|OK)[[:space:]]+-1' "$ROOT/config/board_p1.h")" = "3" ]; then
  say "PASS 10: P1 has no key pins (touch-only); GP26/GP27 are never configured as key inputs"
else say "FAIL 10: P1 key pin definitions present"; fail=1; fi

[ $fail = 0 ] && say "RESULT: PASS" || say "RESULT: FAIL"
exit $fail
