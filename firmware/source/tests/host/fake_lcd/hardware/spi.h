/* Fake PL022 SPI. Models: frame width (8/16), an 8-deep RX FIFO that is filled by EVERY shifted frame
 * (also DMA-fed TX frames, exactly like the PL022), lazily-completing TX frames, and the pico-sdk
 * spi_write_blocking / spi_write_read_blocking algorithms (copied in behaviour from pico-sdk 2.1.1
 * src/rp2_common/hardware_spi/spi.c lines 84-130). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct { int dummy; } spi_inst_t;
typedef struct { volatile uint32_t cr0, cr1, dr, sr, cpsr, imsc, ris, mis, icr, dmacr; } spi_hw_t;
extern spi_inst_t fake_spi0;
#define spi0 (&fake_spi0)
typedef enum { SPI_CPOL_0 = 0, SPI_CPOL_1 = 1 } spi_cpol_t;
typedef enum { SPI_CPHA_0 = 0, SPI_CPHA_1 = 1 } spi_cpha_t;
typedef enum { SPI_LSB_FIRST = 0, SPI_MSB_FIRST = 1 } spi_order_t;
#define SPI_SSPICR_RORIC_BITS 0x1u
unsigned spi_init(spi_inst_t *s, unsigned hz);
unsigned spi_set_baudrate(spi_inst_t *s, unsigned hz);
unsigned spi_get_baudrate(const spi_inst_t *s);
void spi_set_format(spi_inst_t *s, unsigned bits, spi_cpol_t cpol, spi_cpha_t cpha, spi_order_t order);
bool spi_is_busy(const spi_inst_t *s);
bool spi_is_readable(const spi_inst_t *s);
bool spi_is_writable(const spi_inst_t *s);
spi_hw_t *spi_get_hw(spi_inst_t *s);
unsigned spi_get_dreq(spi_inst_t *s, bool is_tx);
int spi_write_blocking(spi_inst_t *s, const uint8_t *src, size_t len);
int spi_write_read_blocking(spi_inst_t *s, const uint8_t *src, uint8_t *dst, size_t len);
