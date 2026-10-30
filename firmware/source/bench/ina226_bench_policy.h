#ifndef CHIPLER_INA226_BENCH_POLICY_H
#define CHIPLER_INA226_BENCH_POLICY_H
#include <stdbool.h>
#include <stdint.h>
#define I226_POLL_MS 20u
/* Wrap-safe for deadlines less than 2^31 ms apart. Skip missed slots, never burst. */
static inline bool i226_poll_due(uint32_t now, uint32_t *next, uint32_t *missed) {
    if ((int32_t)(now - *next) < 0) return false;
    uint32_t late_slots = (now - *next) / I226_POLL_MS;
    *missed += late_slots;
    *next += (late_slots + 1u) * I226_POLL_MS;
    return true;
}
#endif
