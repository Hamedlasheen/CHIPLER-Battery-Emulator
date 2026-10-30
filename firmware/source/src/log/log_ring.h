/* Bounded, non-blocking log ring (V02-02). Pure C, host-tested.
 * Producers (core 0 main loop only) format a line into the ring; if it does not fit, the WHOLE
 * line is dropped and counted - a producer never waits. A consumer drains at most `max` bytes
 * per call (the firmware only drains what the USB CDC FIFO can accept right now). */
#ifndef CHIPLER_LOG_RING_H
#define CHIPLER_LOG_RING_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define LOG_LINE_MAX 160

typedef struct {
    char *buf; size_t cap;
    size_t head, tail;           /* head = write index, tail = read index, used = head - tail */
    uint32_t dropped, pushed;
} log_ring_t;

void lr_init(log_ring_t *r, char *mem, size_t cap);
size_t lr_used(const log_ring_t *r);
bool lr_push(log_ring_t *r, const char *s, size_t n);      /* all-or-nothing */
size_t lr_pop(log_ring_t *r, char *out, size_t max);
/* "[ms] TAG message\n"; truncated to LOG_LINE_MAX; returns true if stored */
bool lr_printf(log_ring_t *r, uint32_t ms, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
#endif
