/* AG-LCD fake SDK recorder API (host only). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t v;          /* value shifted out on MOSI (8- or 16-bit frame) */
    uint8_t bits;        /* frame width at the time it was shifted */
    uint8_t dc;          /* PIN_LCD_DC level while shifting */
    uint8_t lcd_cs;      /* PIN_LCD_CS level while shifting (0 = selected) */
    uint8_t touch_cs;    /* PIN_TOUCH_CS level while shifting (0 = selected) */
    uint8_t via_dma;     /* 1 = fed by the DMA channel */
    uint64_t t_us;       /* virtual time stamp */
} fl_frame_t;

typedef struct { uint8_t level; uint64_t t_us; } fl_rst_t;

extern fl_frame_t *fl_frames;
extern size_t fl_nframes;
extern fl_rst_t fl_rst[16];
extern int fl_nrst;
extern unsigned fl_cs_while_busy;    /* a CS pin changed while frames were still shifting */
extern unsigned fl_fmt_while_busy;   /* spi_set_format while frames were still shifting */
extern unsigned fl_dma_errors;       /* DMA config not matching the SPI (size / target / dreq) */
extern unsigned fl_ror;              /* RX FIFO overrun events (frame received while RX FIFO full) */
extern unsigned fl_baud_actual;      /* last actual baud (pico-sdk 2.1.1 divider algorithm, clk_peri 150 MHz) */

/* fault injection (host only) */
extern int fl_inject_dma_stuck;      /* DMA channel stays busy until dma_channel_abort() */
extern int fl_inject_spi_stuck;      /* SPI shifter never goes idle (not even after an abort) */
extern int fl_inject_rx_never;       /* RX FIFO never becomes readable (dead MISO / disabled block) */
extern int fl_inject_tx_full;        /* TX FIFO never becomes writable */
extern int fl_inject_abort_stuck;    /* the aborted DMA channel never leaves BUSY (RP2350-E5 style hang) */
extern unsigned fl_sse_disabled;     /* SPI block disabled (SSE cleared) */
extern unsigned fl_abort_without_en_clear;  /* abort requested without clearing EN first (E5 violation) */
extern unsigned fl_dma_aborts;
extern unsigned fl_order_cs_before_abort;   /* a CS pin was raised while a stuck DMA had not been aborted yet */

void fl_reset_log(void);
uint64_t fl_now_us(void);
void fl_xpt_set(uint16_t z1, uint16_t z2, uint16_t x, uint16_t y);
unsigned fl_rx_level(void);
