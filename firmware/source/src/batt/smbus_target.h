/* BETA1: hardened descendant of DeepSeek's live SMBus framing engine.
 * Same-core main producer + IRQ consumer only. No phone semantics inferred.
 * Main setters guard the entire mutation; IRQ reads copy one whole wire frame.
 * Bus writes are queued as opaque STOP-validated records, never mutate responses.
 */
#ifndef CHIPLER_SMBUS_TARGET_H
#define CHIPLER_SMBUS_TARGET_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
/* Functions reachable from the I2C IRQ run from SRAM on target (see CMakeLists TE_RAMFUNC);
 * on host the macro is empty. tools/CLAUDE_check_isr_ram.py proves it on the linked ELF. */
#ifndef TE_RAMFUNC
#define TE_RAMFUNC
#endif
/* IRQ-path byte helpers: always inlined into the (RAM) caller. libc memcpy/memset live in XIP flash on
 * this target, so the IRQ chain must not call them (built with -fno-tree-loop-distribute-patterns so
 * the compiler does not turn these loops back into libc calls). */
static inline __attribute__((always_inline)) void st_zero(void *d, unsigned n) { volatile uint8_t *p = (volatile uint8_t *)d; while (n--) *p++ = 0; }
static inline __attribute__((always_inline)) void st_copy(void *d, const void *s, unsigned n) {
    uint8_t *p = (uint8_t *)d; const uint8_t *q = (const uint8_t *)s; while (n--) *p++ = *q++;
}
#include "ds12p_config.h"
/* DEEPSEEK AUTH-GATE 082: wire-path hooks. The main loop installs them once at prepare
 * time; they are called from the IRQ path, so they only touch small state (no libc). */
typedef struct {
    bool (*allow_read)(void *ctx, uint8_t cmd);                          /* false => refuse read */
    bool (*is_page_target)(void *ctx, uint8_t cmd);                      /* selector-page target */
    bool (*page_read)(void *ctx, uint8_t cmd, const uint8_t **data, uint8_t *count);
    bool (*page_select)(void *ctx, uint8_t cmd, const uint8_t *body, uint8_t len);
} st_hooks_t;
#if CHIPLER_12P_REFERENCE
/* 44 recorded read rows + 7 write shapes = 51 slots (page targets are served by the
 * selector hook, not registered statically). +19 slots x sizeof(st_reg_t) ~ +0.8 KiB. */
#define ST_MAX_REGS 56u
#else
#define ST_MAX_REGS 32u
#endif
#define ST_MAX_BLOCK 32u
#define ST_MAX_WRITE 35u
/* Captured evidence: longest run of consecutive valid battery writes in the 8323 decoded frames of the six
 * original captures = 3 (tools/CLAUDE_capture_burst.py). 4 left one slot of margin; 8 gives 2.7x. Overflow still
 * faults deterministically (ST_QUEUE_FULL) and is counted, never silently dropped. */
#define ST_WRITE_QUEUE 8u
#ifndef CHIPLER_AUTO_LEARN
#define CHIPLER_AUTO_LEARN 0
#endif
/* AUTO_LEARN_P1_RC1: requests observed at this target (one packed word per transaction, auto_core.h AUTO_OBS_PACK),
 * produced in the STOP path (IRQ, RAM), consumed by the main loop. Bounded; a full ring counts obs_dropped. */
#define ST_OBS_RING 64u
typedef enum { ST_SHAPE_WRITE, ST_SHAPE_WORD, ST_SHAPE_BLOCK } st_shape_t;
typedef enum { ST_OK, ST_SHORT_A8, ST_BAD_LENGTH, ST_BAD_PEC, ST_UNSUPPORTED,
               ST_ABORTED, ST_OVERREAD, ST_QUEUE_FULL } st_result_t;
typedef struct { uint8_t cmd,len,data[32]; st_shape_t shape; bool ready; } st_reg_t;
typedef struct { uint8_t cmd,len,body[33]; uint32_t sequence; } st_write_t;
typedef uint32_t (*st_guard_enter_fn)(void *);
typedef void (*st_guard_exit_fn)(void *,uint32_t);
typedef struct {
    uint8_t addr7,n_regs; st_reg_t regs[ST_MAX_REGS];
    st_guard_enter_fn enter; st_guard_exit_fn leave; void *guard_ctx;
    bool sealed,active,reading,nacked,overread,invalid;
    bool idle_fill;   /* OWNER build: read of an unknown/refused command answered with released-bus 0xFF, no isolation */
    /* CR-P1 R3 command pointer: retained ONLY after (a) a command-only STOP naming a known readable command or
     * (b) a completed read of that command. Cleared by any write, error, abort, unknown command, profile change
     * or reset. A cold bus (no pointer) never gets an invented default such as 0x86. */
    bool ptr_valid;uint8_t ptr;
    uint8_t last_cmd;bool have_last_cmd; /* diagnostic only (beta2); never used to serve a read - see ptr above */
    uint8_t w[ST_MAX_WRITE],nw,wire[34],wire_len,tx_off;
    uint32_t generation,latched_generation;
    st_result_t last;
    uint32_t txns,reads_done,reads_short,writes_ok,writes_bad,unknown_reads,
             bad_len,overreads,aborts,queue_dropped,rx_bytes,tx_bytes;
    st_write_t writes[ST_WRITE_QUEUE]; atomic_uint write_head,write_tail;
    st_hooks_t hooks; void *hooks_ctx;   /* installed once before activate */
#if CHIPLER_AUTO_LEARN
    volatile bool listen;    /* AUTO observe: every read refused (0xFF released-bus fill), writes/selectors accepted */
    uint32_t obs[ST_OBS_RING]; atomic_uint obs_head, obs_tail; volatile uint32_t obs_dropped;
#endif
} st_target_t;
void st_init(st_target_t *,uint8_t addr7);
bool st_set_guard(st_target_t *,st_guard_enter_fn,st_guard_exit_fn,void *);
void st_set_hooks(st_target_t *,const st_hooks_t *,void *ctx);
bool st_add(st_target_t *,uint8_t cmd,st_shape_t,uint8_t len);
bool st_set_word(st_target_t *,uint8_t cmd,uint16_t value);
/* Current value of a ready WORD register (review 102: replaces direct regs[] walks outside the engine).
 * Not guarded: the caller holds the engine guard when the target is live. */
bool st_get_word(const st_target_t *,uint8_t cmd,uint16_t *value);
bool st_set_words(st_target_t *,uint8_t n,const uint8_t *cmds,const uint16_t *vals); /* atomic multi-WORD update */
bool st_set_block(st_target_t *,uint8_t cmd,const uint8_t *,uint8_t len);
bool st_seal(st_target_t *); /* descriptors frozen, every read response ready */
bool st_live_ready(const st_target_t *); /* seal + actual IRQ guard required */
bool st_on_rx(st_target_t *,uint8_t); /* first byte after STOP is always command */
bool st_begin_read(st_target_t *);   /* requires command-only repeated START */
bool st_on_tx(st_target_t *,uint8_t *);
void st_on_nack(st_target_t *);
st_result_t st_on_stop(st_target_t *);
void st_on_restart_write(st_target_t *); /* discard prior non-STOP write */
bool st_resume_pointer(st_target_t *); /* idle engine + RD_REQ without command: use the retained known pointer, else false */
void st_abort(st_target_t *);        /* drops ALL transaction context */
bool st_pop_write(st_target_t *,st_write_t *); /* main consumer only */
#if CHIPLER_AUTO_LEARN
void st_set_listen(st_target_t *,bool on);       /* guarded; takes effect at the next read */
bool st_pop_obs(st_target_t *,uint32_t *out);    /* main consumer only */
#endif
uint8_t st_pec_read(uint8_t,uint8_t,const uint8_t *,size_t);
uint8_t st_pec_write(uint8_t,uint8_t,const uint8_t *,size_t);
#endif
