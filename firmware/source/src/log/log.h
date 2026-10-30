/* USB-CDC serial log (V02-02). LOG() never blocks: it formats into a bounded RAM ring
 * (whole line dropped + counted when full). log_pump() is called once per main-loop pass and
 * writes at most what the USB CDC FIFO can take *now* (no waiting for a host that is not
 * reading). Call LOG only from core 0. No handset identifiers are ever logged. */
#ifndef CHIPLER_LOG_H
#define CHIPLER_LOG_H
#include "pico/time.h"
#include "log_ring.h"

#define LOG_RING_BYTES   8192u
#define LOG_PUMP_BUDGET  256u     /* max bytes handed to USB per main-loop pass */

extern log_ring_t g_log;
void log_init(void);
void log_pump(void);
uint32_t log_dropped(void);
#define LOG(tag, fmt, ...) lr_printf(&g_log, (uint32_t)to_ms_since_boot(get_absolute_time()), tag, fmt, ##__VA_ARGS__)
#endif
