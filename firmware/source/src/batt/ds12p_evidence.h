/* DEEPSEEK AUTH-GATE 081 - evidence table types (generated header, do not edit).
 * These are plain data types with no firmware dependency so the host unit tests can
 * include the generated tables directly. */
#ifndef CHIPLER_DS12P_EVIDENCE_H
#define CHIPLER_DS12P_EVIDENCE_H
#include <stdint.h>

typedef enum { DS_EV_WORD = 0, DS_EV_BLOCK = 1 } ds_ev_shape_t;
typedef enum { DS_EV_STATIC = 0, DS_EV_DYNAMIC = 1 } ds_ev_class_t;

typedef struct {
    uint8_t cmd;
    ds_ev_shape_t shape;
    uint8_t count;                 /* data bytes (WORD: 2, BLOCK: count byte value) */
    uint8_t data[32];
    uint8_t pec;                   /* captured PEC; recomputed by the generator */
    ds_ev_class_t cls;
    const char *row_id;            /* provenance: capture/tx/time */
    uint32_t n_valid;              /* PEC-valid instances in the capture */
    uint32_t n_distinct;           /* distinct payloads in the capture */
} ds_ev_row_t;

typedef struct {
    uint8_t cmd;
    ds_ev_shape_t shape;
    uint8_t len;                   /* WORD: 2; BLOCK: count byte value */
    const char *row_id;
} ds_ev_write_t;

typedef struct {
    uint8_t sel_cmd;               /* selector write command (0x65 or 0x73) */
    uint8_t page;                  /* selector page value (first body byte) */
    uint8_t target_cmd;            /* block read command served for this page */
    uint8_t count;
    uint8_t data[32];
    uint8_t pec;
    ds_ev_class_t cls;
    const char *row_id;
    uint32_t n_reads;              /* recorded reads of this page */
} ds_page_row_t;
#endif
