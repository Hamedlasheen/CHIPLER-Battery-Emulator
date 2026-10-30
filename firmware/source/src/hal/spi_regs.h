/* SPI0 / DMA register access seam (P1_r11). On the target these are plain register accesses. Host tests that model the PL022
 * FIFOs and the DMA channel define CHIPLER_HOST_SPI_FAKE and provide the fake_* functions: a plain struct access cannot be
 * intercepted on the host, so this header is the only place where the tested and the shipped build differ. */
#ifndef CHIPLER_SPI_REGS_H
#define CHIPLER_SPI_REGS_H
#include <stdint.h>
#include <stdbool.h>
#ifdef CHIPLER_HOST_SPI_FAKE
void fake_spi_dr_write(uint32_t v);
uint32_t fake_spi_dr_read(void);
void fake_spi_disable(void);                 /* SSE cleared: the shifter stops clocking */
void fake_dma_clear_en(unsigned ch);         /* RP2350-E5: clear EN of the channel BEFORE the abort request */
void fake_dma_abort_req(unsigned ch);
bool fake_dma_busy(unsigned ch);
#define SPI_DR_WRITE(v) fake_spi_dr_write((uint32_t)(v))
#define SPI_DR_READ()   fake_spi_dr_read()
#define SPI_DISABLE()   fake_spi_disable()
#define DMA_CLEAR_EN(ch)  fake_dma_clear_en((unsigned)(ch))
#define DMA_ABORT_REQ(ch) fake_dma_abort_req((unsigned)(ch))
#define DMA_CH_BUSY(ch)   fake_dma_busy((unsigned)(ch))
#else
#define SPI_DR_WRITE(v) (spi_get_hw(LCD_SPI_INSTANCE)->dr = (uint32_t)(v))
#define SPI_DR_READ()   (spi_get_hw(LCD_SPI_INSTANCE)->dr)
#define SPI_DISABLE()   hw_clear_bits(&spi_get_hw(LCD_SPI_INSTANCE)->cr1, SPI_SSPCR1_SSE_BITS)
#define DMA_CLEAR_EN(n_)  hw_clear_bits(&dma_hw->ch[(n_)].ctrl_trig, DMA_CH0_CTRL_TRIG_EN_BITS)
#define DMA_ABORT_REQ(n_) (dma_hw->abort = 1u << (n_))
#define DMA_CH_BUSY(n_)   ((dma_hw->ch[(n_)].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0)
#endif
#endif
