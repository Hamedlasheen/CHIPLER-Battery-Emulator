/* I2C target protocol engine (pure logic; called from the I2C IRQ glue).
 * Model: first byte written after START = register pointer; further written bytes go to
 * reg, reg+1, ...; reads return bytes from reg upward (auto-increment). Repeated START
 * keeps the pointer (write-pointer-then-read pattern). Unknown registers read 0xFF and
 * are counted, never fabricated. Every transaction is logged into a lock-free ring. */
#ifndef CHIPLER_TARGET_ENGINE_H
#define CHIPLER_TARGET_ENGINE_H
#include <stdint.h>
#include <stdbool.h>
#include "profile.h"

typedef struct {
    uint32_t t_us;
    uint8_t  reg;        /* pointer at start of transaction (0xFFFF.. if none written) */
    uint8_t  nw, nr;     /* data bytes written (excl. pointer), bytes read */
    uint8_t  flags;      /* bit0 pointer written, bit1 unknown read, bit2 write ignored */
    uint8_t  first[4];   /* first data bytes written (or read) for the log */
} txn_rec_t;

#define TXN_PTR       0x01
#define TXN_UNKNOWN   0x02
#define TXN_WIGNORED  0x04
#define TXN_RING      64   /* power of two */
#define TE_SNAP_MAX   32

/* Double-buffered protocol snapshot (CR-003 atomic SOC update). The writer fills the
 * unpublished buffer and flips `pub`; the IRQ latches `pub` at the START of every transaction
 * and serves the whole transaction from that buffer, so one multi-byte read can never mix old
 * and new values. A snapshot region overrides static register blocks in its address range. */
typedef struct {
    uint8_t data[TE_SNAP_MAX];
    uint8_t base, len;      /* len 0 = no snapshot */
    uint32_t seq;
} te_snapshot_t;

typedef struct {
    const batt_profile_t *prof;
    uint8_t ptr;
    bool    ptr_set;
    bool    in_txn;
    txn_rec_t cur;
    /* counters */
    volatile uint32_t txns, rx_bytes, tx_bytes, unknown_reads, ignored_writes, overflows;
    volatile int last_reg;
    /* ring */
    txn_rec_t ring[TXN_RING];
    volatile uint32_t head, tail;
    /* snapshot */
    te_snapshot_t snap[2];
    volatile uint8_t pub;   /* published buffer index */
    uint8_t lat;            /* buffer latched by the transaction in progress */
} target_engine_t;

void te_init(target_engine_t *e, const batt_profile_t *p);
void te_on_receive(target_engine_t *e, uint8_t byte, uint32_t t_us);
uint8_t te_on_request(target_engine_t *e, uint32_t t_us);
void te_on_finish(target_engine_t *e);
bool te_pop(target_engine_t *e, txn_rec_t *out);     /* consumer (main loop) */
/* V02-02: pop at most `budget` records per call; returns the number handled */
int te_drain(target_engine_t *e, int budget, void (*cb)(void *ctx, const txn_rec_t *r), void *ctx);
void te_clear_counters(target_engine_t *e);
/* Publish a new snapshot (main loop / lower-priority context). Returns false (try again next
 * cycle) if the only free buffer is still latched by a transaction in progress. */
bool te_publish(target_engine_t *e, uint8_t base, const uint8_t *data, uint8_t len);
uint32_t te_snapshot_seq(const target_engine_t *e);
#endif
