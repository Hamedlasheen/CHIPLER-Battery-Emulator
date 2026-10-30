#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {unsigned index;} i2c_inst_t;
extern i2c_inst_t fake_i2c0,fake_i2c1;
#define i2c0 (&fake_i2c0)
#define i2c1 (&fake_i2c1)
void i2c_init(i2c_inst_t *,unsigned);void i2c_deinit(i2c_inst_t *);
unsigned i2c_get_read_available(i2c_inst_t *);uint8_t i2c_read_byte_raw(i2c_inst_t *);
void i2c_write_byte_raw(i2c_inst_t *,uint8_t);unsigned i2c_hw_index(i2c_inst_t *);
