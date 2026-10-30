#include "ina226_pico.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "board_config.h"

#define INA226_ADDR 0x40   /* A0 = A1 = GND (WIRING-INA226-r2) */
static int s_rc;
static bool p_wr(void *ctx, const uint8_t *b, size_t n) {
    (void)ctx;
    s_rc = i2c_write_timeout_us(PM_I2C_INSTANCE, INA226_ADDR, b, n, false, 5000);
    return s_rc == (int)n;
}
static bool p_rd(void *ctx, uint8_t reg, uint8_t *b, size_t n) {
    (void)ctx;
    s_rc = i2c_write_timeout_us(PM_I2C_INSTANCE, INA226_ADDR, &reg, 1, true, 5000);
    if (s_rc != 1) return false;
    s_rc = i2c_read_timeout_us(PM_I2C_INSTANCE, INA226_ADDR, b, n, false, 5000);
    return s_rc == (int)n;
}
static void p_delay(void *ctx, uint32_t ms) { (void)ctx; sleep_ms(ms); }
static uint32_t p_now(void *ctx) { (void)ctx; return to_ms_since_boot(get_absolute_time()); }
static const ina226_bus_t s_bus = {p_wr, p_rd, p_delay, 0, p_now};

const ina226_bus_t *ina226_pico_bus_init(uint32_t hz) {
    i2c_init(PM_I2C_INSTANCE, hz);
    gpio_set_function(PIN_PM_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_PM_SCL, GPIO_FUNC_I2C);
    gpio_init(PIN_PM_ALERT); gpio_set_dir(PIN_PM_ALERT, GPIO_IN); gpio_disable_pulls(PIN_PM_ALERT);
    return &s_bus;
}
bool ina226_pico_alert_low(void) { return !gpio_get(PIN_PM_ALERT); }
int ina226_pico_last_rc(void) { return s_rc; }
