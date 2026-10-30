/* Shared SPI0 arbitration for the ILI9341 display and the touch controller (P1_r1).
 *
 * Rules (enforced + counted, host-tested in tests/host/test_spi_bus.c):
 *   - exactly ONE core owns the bus (core 1, the UI core). A select from another core is refused
 *     and counted; neither driver touches the bus when refused.
 *   - both chip selects are inactive (high) before the baud rate / frame format changes and before
 *     any other device is selected.
 *   - the display DMA transfer is complete and the SPI FIFO drained before spi_bus_release().
 *   - nothing here is called from an interrupt; the battery I2C IRQ never waits on this bus.
 */
#ifndef CHIPLER_SPI_BUS_H
#define CHIPLER_SPI_BUS_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef enum { SPI_DEV_NONE = 0, SPI_DEV_LCD, SPI_DEV_TOUCH } spi_dev_t;
/* P1_r10: every wait on this bus is bounded. On a timeout the recovery order is: abort the DMA channel, wait (bounded) for
 * the SPI shifter to go idle, drain RX + clear overrun, and only THEN may the caller raise a chip select. If the shifter
 * never goes idle the bus is latched FAILED (spi_bus_select() refuses, the failure is logged by core 0), never a silent hang. */
typedef enum { SPI_WAIT_OK = 0, SPI_WAIT_DMA_TIMEOUT, SPI_WAIT_SPI_TIMEOUT, SPI_WAIT_FAILED } spi_wait_t;
typedef struct { bool dma_busy; uint32_t dma_remaining; uint32_t sr; } spi_bus_hw_t;
spi_wait_t spi_bus_wait_idle(uint32_t budget_us);        /* DMA idle, then SPI idle, within budget_us */
uint32_t spi_xfer_budget_us(uint32_t bits, uint32_t hz); /* 2x the wire time + 5 ms margin */
bool spi_bus_write(const uint8_t *d, size_t n);
bool spi_bus_xfer3(const uint8_t tx[3], uint8_t rx[3]);  /* bounded 24-clock touch exchange; false = timed out and RECOVERED (T_CS still low until the caller raises it) */
spi_wait_t spi_bus_recover(unsigned fault_kind);         /* c1_fault_t: abort DMA, wait idle (bounded), drain; never touches a chip select */          /* bounded replacement for spi_write_blocking (RX drained) */
bool spi_bus_failed(void);                               /* latched hard failure */
uint32_t spi_bus_timeouts(void);
uint32_t spi_bus_recoveries(void);
void spi_bus_hw_status(spi_bus_hw_t *o);                 /* side-effect-free register reads, safe from any core */
void spi_bus_init(void);
bool spi_bus_select(spi_dev_t d);   /* false = refused (wrong core / bus busy) */
void spi_bus_release(void);
void spi_bus_set_dma_channel(int ch);   /* select() also waits for this DMA channel to be idle (DEEPSEEK DS-TIMING-04) */
spi_dev_t spi_bus_current(void);
uint32_t spi_bus_violations(void);
#endif
