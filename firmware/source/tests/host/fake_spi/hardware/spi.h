#pragma once
#include <stdbool.h>
typedef struct { int dummy; } spi_inst_t;
extern spi_inst_t fake_spi0;
#define spi0 (&fake_spi0)
#define SPI_CPOL_0 0
#define SPI_CPHA_0 0
#define SPI_MSB_FIRST 0
unsigned spi_init(spi_inst_t *, unsigned);
unsigned spi_set_baudrate(spi_inst_t *, unsigned);
void spi_set_format(spi_inst_t *, unsigned, int, int, int);
bool spi_is_busy(spi_inst_t *);
#include <stdint.h>
#include <stddef.h>
typedef struct { volatile uint32_t dr, sr, icr; } spi_hw_t;
spi_hw_t *spi_get_hw(spi_inst_t *);
bool spi_is_readable(spi_inst_t *);
bool spi_is_writable(spi_inst_t *);
unsigned spi_get_baudrate(spi_inst_t *);
#define SPI_SSPICR_RORIC_BITS 1u
