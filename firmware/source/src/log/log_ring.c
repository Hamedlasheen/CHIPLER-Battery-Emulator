#include "log_ring.h"
#include <stdio.h>
#include <stdarg.h>

void lr_init(log_ring_t *r, char *mem, size_t cap) {
    r->buf = mem; r->cap = cap; r->head = r->tail = 0; r->dropped = r->pushed = 0;
}
size_t lr_used(const log_ring_t *r) { return r->head - r->tail; }

bool lr_push(log_ring_t *r, const char *s, size_t n) {
    if (n == 0) return true;
    if (n > r->cap || r->cap - lr_used(r) < n) { r->dropped++; return false; }
    for (size_t i = 0; i < n; i++) r->buf[(r->head + i) % r->cap] = s[i];
    r->head += n; r->pushed++;
    return true;
}

size_t lr_pop(log_ring_t *r, char *out, size_t max) {
    size_t n = lr_used(r);
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = r->buf[(r->tail + i) % r->cap];
    r->tail += n;
    return n;
}

bool lr_printf(log_ring_t *r, uint32_t ms, const char *tag, const char *fmt, ...) {
    char line[LOG_LINE_MAX];
    int k = snprintf(line, sizeof line, "[%8lu] %-5s ", (unsigned long)ms, tag);
    if (k < 0) k = 0;
    if (k > (int)sizeof line - 2) k = (int)sizeof line - 2;
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(line + k, sizeof line - (size_t)k - 1, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    size_t len = (size_t)k + (size_t)m;
    if (len > sizeof line - 2) len = sizeof line - 2;     /* truncated */
    line[len++] = '\n';
    return lr_push(r, line, len);
}
