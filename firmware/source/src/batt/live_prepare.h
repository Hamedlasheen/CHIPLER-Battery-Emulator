#ifndef CHIPLER_LIVE_PREPARE_H
#define CHIPLER_LIVE_PREPARE_H
#include "smbus_target.h"
#include "profile.h"
bool live_prepare(st_target_t *,const batt_profile_t *,int soc);
bool live_publish_soc(st_target_t *,uint8_t cmd,const uint8_t *,uint8_t len);
#endif
