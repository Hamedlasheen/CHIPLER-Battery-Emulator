#include "core1_diag.h"
#include <stdatomic.h>
#include "pico/stdlib.h"

enum {
    F_STAGE, F_SINCE, F_SERIAL, F_LOOPS, F_POLLS, F_RENDERS, F_PUSHES, F_ROWS,
    F_LAST_LOOP, F_MAX_LOOP, F_LAST_RENDER, F_MAX_RENDER, F_LAST_PUSH, F_MAX_PUSH, F_LAST_TOUCH, F_MAX_TOUCH,
    F_TOUCH_OK, F_TOUCH_FAIL, F_TMO_DMA, F_TMO_SPI, F_TMO_TOUCH, F_TMO_CPU, F_TMO_WRITE, F_RECOV,
    F_FAULT_KIND, F_FAULT_STAGE, F_FAULT_AT, F_BUS_FAILED, F_N
};

static atomic_uint s_f[F_N];
static atomic_uint s_seq;          /* even = stable, odd = writer inside an update */
static atomic_uint s_written;

static inline uint32_t ld(int i) { return atomic_load_explicit(&s_f[i], memory_order_relaxed); }
static inline void st(int i, uint32_t v) { atomic_store_explicit(&s_f[i], v, memory_order_relaxed); }

static inline void begin(void) {
    unsigned s = atomic_load_explicit(&s_seq, memory_order_relaxed);
    atomic_store_explicit(&s_seq, s + 1, memory_order_relaxed);   /* odd */
    atomic_thread_fence(memory_order_release);
}
static inline void end(void) {
    atomic_thread_fence(memory_order_release);
    unsigned s = atomic_load_explicit(&s_seq, memory_order_relaxed);
    atomic_store_explicit(&s_seq, s + 1, memory_order_release);   /* even */
    atomic_store_explicit(&s_written, 1, memory_order_relaxed);
}
static inline void mx(int i, uint32_t v) { if (v > ld(i)) st(i, v); }

void c1d_init(void) {
    begin();
    for (int i = 0; i < F_N; i++) st(i, 0);
    st(F_SINCE, time_us_32());
    end();
}
void c1d_stage(c1_stage_t s) {
    uint32_t now = time_us_32();
    begin(); st(F_STAGE, (uint32_t)s); st(F_SINCE, now); st(F_SERIAL, ld(F_SERIAL) + 1); end();
}
void c1d_note_loop(uint32_t us) { begin(); st(F_LOOPS, ld(F_LOOPS) + 1); st(F_LAST_LOOP, us); mx(F_MAX_LOOP, us); end(); }
void c1d_note_poll(uint32_t us, bool ok) {
    begin(); st(F_POLLS, ld(F_POLLS) + 1); st(F_LAST_TOUCH, us); mx(F_MAX_TOUCH, us);
    st(ok ? F_TOUCH_OK : F_TOUCH_FAIL, ld(ok ? F_TOUCH_OK : F_TOUCH_FAIL) + 1); end();
}
void c1d_note_render(uint32_t us) { begin(); st(F_RENDERS, ld(F_RENDERS) + 1); st(F_LAST_RENDER, us); mx(F_MAX_RENDER, us); end(); }
void c1d_note_push(uint32_t us, uint32_t rows) {
    begin(); st(F_PUSHES, ld(F_PUSHES) + 1); st(F_ROWS, ld(F_ROWS) + rows); st(F_LAST_PUSH, us); mx(F_MAX_PUSH, us); end();
}
void c1d_fault(c1_fault_t kind) {
    uint32_t now = time_us_32();
    begin();
    switch (kind) {
    case C1F_DMA_TIMEOUT: st(F_TMO_DMA, ld(F_TMO_DMA) + 1); break;
    case C1F_SPI_TIMEOUT: st(F_TMO_SPI, ld(F_TMO_SPI) + 1); break;
    case C1F_TOUCH_TIMEOUT: st(F_TMO_TOUCH, ld(F_TMO_TOUCH) + 1); break;
    case C1F_CPU_PUSH_TIMEOUT: st(F_TMO_CPU, ld(F_TMO_CPU) + 1); break;
    case C1F_WRITE_TIMEOUT: st(F_TMO_WRITE, ld(F_TMO_WRITE) + 1); break;
    default: break;
    }
    st(F_FAULT_KIND, (uint32_t)kind); st(F_FAULT_STAGE, ld(F_STAGE)); st(F_FAULT_AT, now);
    end();
}
void c1d_note_recovery(void) { begin(); st(F_RECOV, ld(F_RECOV) + 1); end(); }
void c1d_set_bus_failed(bool failed) { begin(); st(F_BUS_FAILED, failed ? 1u : 0u); end(); }

bool c1d_read(c1d_snapshot_t *o) {
    uint32_t v[F_N];
    bool torn = true;
    for (int tries = 0; tries < 64; tries++) {
        unsigned s1 = atomic_load_explicit(&s_seq, memory_order_acquire);
        if (s1 & 1u) continue;
        for (int i = 0; i < F_N; i++) v[i] = ld(i);
        atomic_thread_fence(memory_order_acquire);
        unsigned s2 = atomic_load_explicit(&s_seq, memory_order_relaxed);
        if (s1 == s2) { torn = false; break; }
    }
    if (torn) for (int i = 0; i < F_N; i++) v[i] = ld(i);         /* best effort, flagged */
    o->now_us = time_us_32();
    o->stage = v[F_STAGE]; o->stage_since_us = v[F_SINCE]; o->stage_age_us = o->now_us - v[F_SINCE]; o->stage_serial = v[F_SERIAL];
    o->loops = v[F_LOOPS]; o->polls = v[F_POLLS]; o->renders = v[F_RENDERS]; o->pushes = v[F_PUSHES]; o->rows_total = v[F_ROWS];
    o->last_loop_us = v[F_LAST_LOOP]; o->max_loop_us = v[F_MAX_LOOP]; o->last_render_us = v[F_LAST_RENDER]; o->max_render_us = v[F_MAX_RENDER];
    o->last_push_us = v[F_LAST_PUSH]; o->max_push_us = v[F_MAX_PUSH]; o->last_touch_us = v[F_LAST_TOUCH]; o->max_touch_us = v[F_MAX_TOUCH];
    o->touch_ok = v[F_TOUCH_OK]; o->touch_fail = v[F_TOUCH_FAIL];
    o->tmo_dma = v[F_TMO_DMA]; o->tmo_spi = v[F_TMO_SPI]; o->tmo_touch = v[F_TMO_TOUCH]; o->tmo_cpu = v[F_TMO_CPU]; o->tmo_write = v[F_TMO_WRITE];
    o->recoveries = v[F_RECOV]; o->fault_kind = v[F_FAULT_KIND]; o->fault_stage = v[F_FAULT_STAGE]; o->fault_at_us = v[F_FAULT_AT];
    o->bus_failed = v[F_BUS_FAILED]; o->torn = torn;
    return atomic_load_explicit(&s_written, memory_order_relaxed) != 0;
}

static const char *const k_stage[C1S__COUNT] = {
    "BOOT", "LCD_INIT", "GFX_INIT", "TOUCH_INIT", "TEST_PATTERN", "BOOT_DELAY", "LOOP_TOP", "LOCK_WAIT", "MODEL_COPY",
    "TOUCH_POLL", "TOUCH_XFER", "UI_LOGIC", "RENDER", "PUSH_HASH", "LCD_CMD", "DMA_START", "DMA_WAIT", "CPU_PUSH",
    "SPI_IDLE", "SPI_DRAIN", "SPI_FORMAT", "RECOVER", "SLEEP"
};
static const char *const k_fault[C1F__COUNT] = { "NONE", "DMA_TIMEOUT", "SPI_TIMEOUT", "TOUCH_TIMEOUT", "CPU_PUSH_TIMEOUT", "WRITE_TIMEOUT", "BUS_FAILED", "ABORT_TIMEOUT" };
const char *c1d_stage_name(uint32_t s) { return s < C1S__COUNT ? k_stage[s] : "?"; }
const char *c1d_fault_name(uint32_t k) { return k < C1F__COUNT ? k_fault[k] : "?"; }
