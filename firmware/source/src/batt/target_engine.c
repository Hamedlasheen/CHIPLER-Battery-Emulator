#include "target_engine.h"
#include <string.h>

#ifndef TE_RAMFUNC
#define TE_RAMFUNC
#endif

void te_init(target_engine_t *e, const batt_profile_t *p) {
    memset(e, 0, sizeof *e);
    e->prof = p;
    e->last_reg = -1;
}

void te_clear_counters(target_engine_t *e) {
    e->txns = e->rx_bytes = e->tx_bytes = e->unknown_reads = e->ignored_writes = e->overflows = 0;
    e->last_reg = -1;
}

TE_RAMFUNC static void begin(target_engine_t *e, uint32_t t_us) {
    if (e->in_txn) return;
    e->in_txn = true;
    memset(&e->cur, 0, sizeof e->cur);
    e->cur.t_us = t_us;
    e->cur.reg = e->ptr;
    e->lat = e->pub;          /* whole transaction is served from this snapshot */
}

/* value of register `reg` for the current transaction; false if unknown */
TE_RAMFUNC static bool read_reg(target_engine_t *e, uint8_t reg, uint8_t *v, bool *writable_static) {
    const te_snapshot_t *s = &e->snap[e->lat];
    *writable_static = false;
    if (s->len && reg >= s->base && reg < (uint16_t)s->base + s->len) { *v = s->data[reg - s->base]; return true; }
    return false;
}

TE_RAMFUNC static const reg_block_t *find_block(const batt_profile_t *p, uint8_t reg, uint8_t *idx) {
    if (!p || !p->regs) return 0;
    for (int i = 0; i < p->n_regs; i++) {
        const reg_block_t *b = &p->regs[i];
        if (reg >= b->reg && reg < (uint16_t)b->reg + b->len) { *idx = (uint8_t)(reg - b->reg); return b; }
    }
    return 0;
}

TE_RAMFUNC void te_on_receive(target_engine_t *e, uint8_t byte, uint32_t t_us) {
    begin(e, t_us);
    e->rx_bytes++;
    if (!(e->cur.flags & TXN_PTR)) {
        e->ptr = byte; e->ptr_set = true;
        e->cur.reg = byte; e->cur.flags |= TXN_PTR;
        e->last_reg = byte;
        return;
    }
    if (e->cur.nw < 4) e->cur.first[e->cur.nw] = byte;
    if (e->cur.nw < 255) e->cur.nw++;
    uint8_t i, sv; bool ws;
    const reg_block_t *b = read_reg(e, e->ptr, &sv, &ws) ? 0 : find_block(e->prof, e->ptr, &i);
    if (b && b->writable) ((uint8_t *)b->data)[i] = byte;
    else { e->cur.flags |= TXN_WIGNORED; e->ignored_writes++; }
    e->ptr++;
}

TE_RAMFUNC uint8_t te_on_request(target_engine_t *e, uint32_t t_us) {
    begin(e, t_us);
    uint8_t v = 0xFF, i; bool ws;
    if (!read_reg(e, e->ptr, &v, &ws)) {
        const reg_block_t *b = find_block(e->prof, e->ptr, &i);
        if (b) v = b->data[i];
        else { v = 0xFF; e->cur.flags |= TXN_UNKNOWN; e->unknown_reads++; }
    }
    if (e->cur.nw == 0 && e->cur.nr < 4) e->cur.first[e->cur.nr] = v;
    if (e->cur.nr < 255) e->cur.nr++;
    e->tx_bytes++;
    e->ptr++;
    return v;
}

TE_RAMFUNC void te_on_finish(target_engine_t *e) {
    if (!e->in_txn) return;
    e->in_txn = false;
    e->txns++;
    uint32_t h = e->head;
    if (h - e->tail >= TXN_RING) { e->overflows++; return; }
    e->ring[h & (TXN_RING - 1)] = e->cur;
    __asm volatile("" ::: "memory");
    e->head = h + 1;
}

bool te_pop(target_engine_t *e, txn_rec_t *out) {
    uint32_t t = e->tail;
    if (t == e->head) return false;
    *out = e->ring[t & (TXN_RING - 1)];
    __asm volatile("" ::: "memory");
    e->tail = t + 1;
    return true;
}

int te_drain(target_engine_t *e, int budget, void (*cb)(void *ctx, const txn_rec_t *r), void *ctx) {
    int n = 0;
    txn_rec_t r;
    while (n < budget && te_pop(e, &r)) { cb(ctx, &r); n++; }
    return n;
}

bool te_publish(target_engine_t *e, uint8_t base, const uint8_t *data, uint8_t len) {
    if (len > TE_SNAP_MAX) return false;
    uint8_t tgt = e->pub ^ 1;
    if (e->in_txn && e->lat == tgt) return false;   /* still being read by an older transaction */
    te_snapshot_t *s = &e->snap[tgt];
    for (uint8_t k = 0; k < len; k++) s->data[k] = data[k];
    s->base = base; s->len = len;
    s->seq = e->snap[e->pub].seq + 1;
    __asm volatile("" ::: "memory");
    e->pub = tgt;
    return true;
}

uint32_t te_snapshot_seq(const target_engine_t *e) { return e->snap[e->pub].seq; }
