/*
 * CHIPLER P1_r1 - BENCH ONLY: live SMBus target exerciser (replaces DeepSeek's smbus_live_bench which used the
 * old engine that returned 0xFF for unknown reads).
 *
 * Runs the PRODUCTION path (battery_bus -> live_port -> live_wire -> smbus_target) on I2C1 GP6/GP7 at 0x0B with the
 * selected bench profile, and prints counters once per second. Connect an ISOLATED bench master (logic-analyzer
 * driven or a second board), NOT a phone, NOT a charger.
 *   profile SMB-BENCH   : synthetic block 0x20 (4 bytes), SOC publish test
 *   profile CAP12PM-B1  : captured T03 responses (22 reads + opaque writes)
 *   profile P12PM-B2    : captured T06 responses (PHONE DATA TEST; data only)
 * Serial keys: 1/2/3 select profile and (re)start, x stop, c clear counters, h help.
 * Output stays locked (GP28 low); this image never arms power and never claims phone acceptance.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "board_config.h"
#include "hal/board_io.h"
#include "hal/battery_bus.h"
#include "batt/profile.h"

static const char *k_ids[3] = {"SMB-BENCH", "CAP12PM-B1", "P12PM-B2"};

static void start(int k) {
    const batt_profile_t *p = profile_get(profile_find(k_ids[k]));
    battery_bus_stop(); battery_bus_reset_fault();
    bool ok = p && battery_bus_prepare(p, 50) && battery_bus_activate();
    printf("[BENCH] profile %s -> %s\n", k_ids[k], ok ? "RUNNING" : "NOT STARTED");
}

int main(void) {
    board_io_init();
    battery_bus_init();
    stdio_init_all();
    sleep_ms(1500);
    printf("[BENCH] smbus_live_bench P1_r1: keys 1=SMB-BENCH 2=CAP12PM-B1 3=P12PM-B2 x=stop c=clear h=help\n");
    uint32_t last = 0;
    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        battery_bus_service(time_us_32());
        st_write_t w;
        while (battery_bus_pop_write(&w)) printf("[BENCH] opaque write cmd=%02X len=%u seq=%lu\n", w.cmd, w.len, (unsigned long)w.sequence);
        int c = getchar_timeout_us(0);
        if (c == '1') start(0); else if (c == '2') start(1); else if (c == '3') start(2);
        else if (c == 'x') { battery_bus_stop(); printf("[BENCH] stopped (pins released, no isolation claim)\n"); }
        else if (c == 'c') battery_bus_clear_counters();
        else if (c == 'h') printf("[BENCH] 1/2/3 start profile, x stop, c clear counters\n");
        if (now - last >= 1000) {
            last = now;
            target_engine_t *e = battery_bus_engine();
            battery_bus_diag_t d;
            printf("[BENCH] run=%d fault=%d txns=%lu rx=%lu tx=%lu unk=%lu ign=%lu err=%lu",
                   battery_bus_running(), battery_bus_fault(), (unsigned long)e->txns, (unsigned long)e->rx_bytes,
                   (unsigned long)e->tx_bytes, (unsigned long)e->unknown_reads, (unsigned long)e->ignored_writes, (unsigned long)e->overflows);
            if (battery_bus_diagnostic(&d)) printf(" reads=%lu short=%lu lastCmd=%02X reason=%d irq=%08lX abort=%08lX",
                   (unsigned long)d.reads, (unsigned long)d.short_reads, d.command, d.fault, (unsigned long)d.irq, (unsigned long)d.abort_source);
            printf("\n");
        }
        sleep_us(500);
    }
}
