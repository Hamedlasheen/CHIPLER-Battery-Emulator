/* Core-1 progress / stall diagnostics (P1_r10). INSTRUMENTATION ONLY: it records where core 1 is, it never changes
 * what core 1 does.
 *
 * Single writer (core 1), any reader (core 0 serial 'g' and the stall monitor). Every field is a relaxed atomic word and
 * the whole block is protected by a seqlock, so a reader can never see a half-updated block and never blocks, even when
 * core 1 is stuck forever inside a wait (the reader copies whatever is there; if the writer died between the two
 * sequence increments the reader gives up after a bounded number of tries and reports torn=1). The reader takes no lock
 * that core 1 could own. Nothing here reads a peripheral data register.
 */
#ifndef CHIPLER_CORE1_DIAG_H
#define CHIPLER_CORE1_DIAG_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    C1S_BOOT = 0, C1S_LCD_INIT, C1S_GFX_INIT, C1S_TOUCH_INIT, C1S_TEST_PATTERN, C1S_BOOT_DELAY,
    C1S_LOOP_TOP, C1S_LOCK_WAIT, C1S_MODEL_COPY, C1S_TOUCH_POLL, C1S_TOUCH_XFER, C1S_UI_LOGIC,
    C1S_RENDER, C1S_PUSH_HASH, C1S_LCD_CMD, C1S_DMA_START, C1S_DMA_WAIT, C1S_CPU_PUSH,
    C1S_SPI_IDLE, C1S_SPI_DRAIN, C1S_SPI_FORMAT, C1S_RECOVER, C1S_SLEEP, C1S__COUNT
} c1_stage_t;

typedef enum {
    C1F_NONE = 0, C1F_DMA_TIMEOUT, C1F_SPI_TIMEOUT, C1F_TOUCH_TIMEOUT, C1F_CPU_PUSH_TIMEOUT, C1F_WRITE_TIMEOUT,
    C1F_BUS_FAILED, C1F_ABORT_TIMEOUT, C1F__COUNT
} c1_fault_t;

typedef struct {
    uint32_t now_us;                 /* reader's clock when the snapshot was taken (time_us_32) */
    uint32_t stage;                  /* c1_stage_t */
    uint32_t stage_since_us;         /* time_us_32 value when the current stage was entered */
    uint32_t stage_age_us;           /* now_us - stage_since_us */
    uint32_t stage_serial;           /* increments on every stage change: proves core 1 is moving */
    uint32_t loops, polls, renders, pushes, rows_total;
    uint32_t last_loop_us, max_loop_us, last_render_us, max_render_us, last_push_us, max_push_us, last_touch_us, max_touch_us;
    uint32_t touch_ok, touch_fail;
    uint32_t tmo_dma, tmo_spi, tmo_touch, tmo_cpu, tmo_write, recoveries;
    uint32_t fault_kind, fault_stage, fault_at_us;
    uint32_t bus_failed;
    bool torn;                       /* reader gave up (writer stopped inside an update) */
} c1d_snapshot_t;

void c1d_init(void);                                   /* core 1 or boot; resets counters */
void c1d_stage(c1_stage_t s);                          /* writer: where core 1 is now */
void c1d_note_loop(uint32_t us);
void c1d_note_poll(uint32_t us, bool ok);              /* one touch poll incl. SPI */
void c1d_note_render(uint32_t us);
void c1d_note_push(uint32_t us, uint32_t rows);        /* one lcd_push_frame_dirty() */
void c1d_fault(c1_fault_t kind);                       /* records kind + the stage active right now */
void c1d_note_recovery(void);
void c1d_set_bus_failed(bool failed);
bool c1d_read(c1d_snapshot_t *out);                    /* any core; never blocks; false only if the block was never written */
const char *c1d_stage_name(uint32_t stage);
const char *c1d_fault_name(uint32_t kind);
#endif
