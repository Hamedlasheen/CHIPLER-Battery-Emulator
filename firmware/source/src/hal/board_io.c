#include "board_io.h"
#include "board_config.h"
#include "pico/stdlib.h"

static bool s_out;

static void btn_pin(unsigned pin) {
    gpio_init(pin); gpio_set_dir(pin, GPIO_IN); gpio_pull_up(pin);
}

void board_io_init(void) {
    /* FIRST: phone power path off (external 100k pull-down also holds it off during reset) */
    gpio_init(PIN_OUT_EN); gpio_put(PIN_OUT_EN, 0); gpio_set_dir(PIN_OUT_EN, GPIO_OUT);
    s_out = false;
#if PIN_BTN_UP >= 0
    btn_pin(PIN_BTN_UP);
#endif
#if PIN_BTN_DOWN >= 0
    btn_pin(PIN_BTN_DOWN);
#endif
#if PIN_BTN_OK >= 0
    btn_pin(PIN_BTN_OK);
#else
    /* touch-only variant: GP29 is an unconnected reserve, plain input without pulls */
    gpio_init(PIN_RESERVED_GP29); gpio_set_dir(PIN_RESERVED_GP29, GPIO_IN); gpio_disable_pulls(PIN_RESERVED_GP29);
#endif
}

__attribute__((unused)) static bool rd(unsigned pin) { return BTN_ACTIVE_LOW ? !gpio_get(pin) : gpio_get(pin); }
#if PIN_BTN_UP >= 0
bool btn_up_pressed(void)   { return rd(PIN_BTN_UP); }
#else
bool btn_up_pressed(void)   { return false; }   /* P1: key not fitted, navigation is by touch */
#endif
#if PIN_BTN_DOWN >= 0
bool btn_down_pressed(void) { return rd(PIN_BTN_DOWN); }
#else
bool btn_down_pressed(void) { return false; }
#endif
#if PIN_BTN_OK >= 0
bool btn_ok_pressed(void)   { return rd(PIN_BTN_OK); }
#else
bool btn_ok_pressed(void)   { return false; }
#endif

void out_enable(bool on) {
    s_out = on && PROTECTION_HARDWARE_FITTED;
    gpio_put(PIN_OUT_EN, s_out ? 1 : 0);
}
bool out_enabled(void) { return s_out; }

bool pm_alert_active(void) { return !gpio_get(PIN_PM_ALERT); }  /* open-drain, active low */
