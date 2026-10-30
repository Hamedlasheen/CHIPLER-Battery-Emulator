/* Single-producer / single-consumer command ring: UI core (touch) -> control core.
 * Lock-free, fixed size, drops (and counts) when full instead of blocking. Producer = core 1 only,
 * consumer = core 0 only. Commands carry the screen they were issued on so a command that arrives after
 * the screen changed is discarded by the consumer (no "tap landed on a different button").
 */
#ifndef CHIPLER_UI_CMD_QUEUE_H
#define CHIPLER_UI_CMD_QUEUE_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define UICQ_N 8u   /* power of two */

typedef enum {
    UIC_NONE = 0,
    UIC_SELECT,        /* idx */
    UIC_BACK,
    UIC_STOP,
    UIC_PAGE_UP,
    UIC_PAGE_DOWN,
    UIC_CAL_DONE,      /* calibration result is in the shared slot */
    UIC_AUTOCAL        /* touch seen while uncalibrated: offer calibration */
} uic_kind_t;

typedef struct { uint8_t kind; uint8_t screen; int16_t idx; } uic_t;

typedef struct {
    uic_t slot[UICQ_N];
    atomic_uint head, tail;
    uint32_t dropped;
    atomic_uint stop_req;      /* priority flag for STOP REPLIES: not part of the ring, so it can never be dropped */
} uicq_t;

static inline void uicq_init(uicq_t *q) { atomic_init(&q->head, 0); atomic_init(&q->tail, 0); atomic_init(&q->stop_req, 0); q->dropped = 0; }
static inline void uicq_request_stop(uicq_t *q) { atomic_store_explicit(&q->stop_req, 1, memory_order_release); }
static inline bool uicq_take_stop(uicq_t *q) { return atomic_exchange_explicit(&q->stop_req, 0, memory_order_acq_rel) != 0; }

static inline bool uicq_push(uicq_t *q, uic_t c) {
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (h - t >= UICQ_N) { q->dropped++; return false; }
    q->slot[h & (UICQ_N - 1)] = c;
    atomic_store_explicit(&q->head, h + 1, memory_order_release);
    return true;
}

static inline bool uicq_pop(uicq_t *q, uic_t *out) {
    unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&q->head, memory_order_acquire);
    if (t == h) return false;
    *out = q->slot[t & (UICQ_N - 1)];
    atomic_store_explicit(&q->tail, t + 1, memory_order_release);
    return true;
}
#endif
