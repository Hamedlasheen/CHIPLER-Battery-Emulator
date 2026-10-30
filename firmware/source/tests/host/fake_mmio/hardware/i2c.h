/* Register layout subset of the RP2350 DW_apb_i2c block (member names as in pico-sdk hardware/structs/i2c.h). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t con, enable, intr_stat, intr_mask, rx_tl, tx_tl, rxflr;
    uint32_t data_cmd, clr_intr, clr_rd_req, clr_rx_done, clr_stop_det, clr_start_det, clr_restart_det;
    uint32_t fs_spklen;
} i2c_hw_t;
typedef struct { unsigned index; i2c_hw_t hw; } i2c_inst_t;
extern i2c_inst_t fake_i2c0, fake_i2c1;
#define i2c0 (&fake_i2c0)
#define i2c1 (&fake_i2c1)
static inline i2c_hw_t *i2c_get_hw(i2c_inst_t *i) { return &i->hw; }
static inline unsigned i2c_hw_index(i2c_inst_t *i) { return i->index; }
void i2c_init(i2c_inst_t *, unsigned);
void i2c_deinit(i2c_inst_t *);
void i2c_set_slave_mode(i2c_inst_t *, bool, uint8_t);
