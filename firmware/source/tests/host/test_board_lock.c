#include "../../src/hal/board_io.h"
#include "board_config.h"
#include <stdio.h>
static unsigned fails,writes;static bool pin;
void gpio_init(unsigned p){(void)p;}
void gpio_set_dir(unsigned p,bool v){(void)p;(void)v;}
void gpio_put(unsigned p,bool v){if(p==PIN_OUT_EN){writes++;pin=v;if(v)fails++;}}
void gpio_pull_up(unsigned p){(void)p;}
bool gpio_get(unsigned p){(void)p;return true;}
int main(void){
 board_io_init();if(pin||out_enabled()||PROTECTION_HARDWARE_FITTED)fails++;
 out_enable(true);if(pin||out_enabled())fails++;
 out_enable(false);if(pin||out_enabled()||writes!=3)fails++;
 printf("Production board_io output lock with absent protection: %s; GPIO writes=%u\n",fails?"FAIL":"PASS",writes);
 return fails?1:0;
}
