#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct { int dummy; } spi_inst_t;
extern spi_inst_t fake_spi0;
#define spi0 (&fake_spi0)
int spi_write_read_blocking(spi_inst_t *spi, const uint8_t *src, uint8_t *dst, size_t len);
