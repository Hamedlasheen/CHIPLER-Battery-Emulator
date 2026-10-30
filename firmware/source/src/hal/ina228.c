/* Pico glue for the INA228 core (V02-01): real I2C0 bus with timeouts. All logic incl. checked
 * writes / read-back / readiness lives in ina228_core.c (host-tested with fault injection). */
#include "ina228.h"
#include "board_config.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"

static bool bus_wr(void *ctx, const uint8_t *buf, size_t n) {
    (void)ctx;
    return i2c_write_timeout_us(PM_I2C_INSTANCE, INA228_ADDR, buf, n, false, 3000) == (int)n;
}
static bool bus_rd(void *ctx, uint8_t reg, uint8_t *buf, size_t n) {
    (void)ctx;
    if (i2c_write_timeout_us(PM_I2C_INSTANCE, INA228_ADDR, &reg, 1, true, 2000) != 1) return false;
    return i2c_read_timeout_us(PM_I2C_INSTANCE, INA228_ADDR, buf, n, false, 3000) == (int)n;
}
static void bus_delay(void *ctx, uint32_t ms) { (void)ctx; sleep_ms(ms); }

static const ina228_bus_t s_bus = {bus_wr, bus_rd, bus_delay, 0};
static ina228_dev_t s_dev;

bool ina228_init(float r_shunt, float i_max) {
    i2c_init(PM_I2C_INSTANCE, PM_I2C_HZ);
    gpio_set_function(PIN_PM_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_PM_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_PM_SDA); gpio_pull_up(PIN_PM_SCL);   /* weak; external 4.7k required */
    gpio_init(PIN_PM_ALERT); gpio_set_dir(PIN_PM_ALERT, GPIO_IN); gpio_pull_up(PIN_PM_ALERT);
    return ina228_dev_init(&s_dev, &s_bus, r_shunt, i_max) == INA_OK;
}

bool ina228_present(void) { return s_dev.ready; }
uint16_t ina228_device_id(void) { return s_dev.devid; }
ina_status_t ina228_status(void) { return s_dev.status; }
bool ina228_read(ina228_reading_t *r) { return ina228_dev_read(&s_dev, r); }
bool ina228_set_bus_ov_alert(float volts) { return ina228_dev_set_ov(&s_dev, volts) == INA_OK; }
