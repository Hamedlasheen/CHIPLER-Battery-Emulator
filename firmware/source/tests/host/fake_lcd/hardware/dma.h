#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef unsigned int uint;
enum dma_channel_transfer_size { DMA_SIZE_8 = 0, DMA_SIZE_16 = 1, DMA_SIZE_32 = 2 };
typedef struct { uint32_t size, dreq; bool rinc, winc; } dma_channel_config;
int dma_claim_unused_channel(bool required);
dma_channel_config dma_channel_get_default_config(uint ch);
void channel_config_set_transfer_data_size(dma_channel_config *c, enum dma_channel_transfer_size s);
void channel_config_set_dreq(dma_channel_config *c, uint dreq);
void channel_config_set_read_increment(dma_channel_config *c, bool inc);
void channel_config_set_write_increment(dma_channel_config *c, bool inc);
void dma_channel_configure(uint ch, const dma_channel_config *c, volatile void *write_addr,
                           const volatile void *read_addr, uint transfer_count, bool trigger);
void dma_channel_wait_for_finish_blocking(uint ch);
bool dma_channel_is_busy(uint ch);
void dma_channel_abort(uint ch);
typedef struct { uint32_t transfer_count; } dma_channel_hw_t;
dma_channel_hw_t *dma_channel_hw_addr(uint ch);
