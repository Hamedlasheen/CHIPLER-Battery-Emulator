#ifndef CHIPLER_BOARD_IO_H
#define CHIPLER_BOARD_IO_H
#include <stdbool.h>

void board_io_init(void);          /* OUT_EN forced low first */
bool btn_up_pressed(void);
bool btn_down_pressed(void);
bool btn_ok_pressed(void);
void out_enable(bool on);          /* drives eFuse EN */
bool out_enabled(void);
bool pm_alert_active(void);
#endif
