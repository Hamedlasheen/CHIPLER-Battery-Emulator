#pragma once
#include <stdbool.h>
#include <stdint.h>
bool dma_channel_is_busy(unsigned);
void dma_channel_abort(unsigned);
typedef struct { uint32_t transfer_count; } dma_channel_hw_t;
dma_channel_hw_t *dma_channel_hw_addr(unsigned);
