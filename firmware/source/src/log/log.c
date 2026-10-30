#include "log.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "tusb.h"

static char s_mem[LOG_RING_BYTES];
log_ring_t g_log;

void log_init(void) { lr_init(&g_log, s_mem, sizeof s_mem); }
uint32_t log_dropped(void) { return g_log.dropped; }

void log_pump(void) {
    if (!stdio_usb_connected()) return;                 /* keep lines until a host opens the port */
    uint32_t avail = tud_cdc_write_available();
    /* stdio translates '\n' to "\r\n": worst case doubles; only hand over what fits now */
    size_t n = avail / 2;
    if (n > LOG_PUMP_BUDGET) n = LOG_PUMP_BUDGET;
    if (n == 0) return;
    char tmp[LOG_PUMP_BUDGET];
    size_t k = lr_pop(&g_log, tmp, n);
    if (k) stdio_put_string(tmp, (int)k, false, true);
}
