#!/usr/bin/env bash
# 063 R1: main.c must use the atomic splash gate, initialise it before launching core 1, and keep no plain volatile flag for it.
set -e
M="${1:?path to src/main.c}"
grep -q 'splash_gate_t s_splash_gate' "$M"
! grep -q 'volatile bool s_splash' "$M"
a=$(grep -n 'splash_gate_init(&s_splash_gate)' "$M" | head -1 | cut -d: -f1)
b=$(grep -n 'multicore_launch_core1_with_stack' "$M" | head -1 | cut -d: -f1)
[ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]
grep -q 'splash_gate_on_push(&s_splash_gate' "$M"
grep -q 'splash_gate_leave(&s_splash_gate' "$M"
grep -q 'memory_order_release' src/ui/splash_gate.h 2>/dev/null || grep -q 'memory_order_release' "$(dirname "$M")/ui/splash_gate.h"
grep -q 'memory_order_acquire' "$(dirname "$M")/ui/splash_gate.h"
echo "splash gate static check PASS"
