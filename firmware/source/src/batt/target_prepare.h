#ifndef CHIPLER_TARGET_PREPARE_H
#define CHIPLER_TARGET_PREPARE_H
#include "target_engine.h"
typedef enum { TP_OK, TP_BAD_PROFILE, TP_BAD_ADDRESS, TP_NO_RESPONSE, TP_BUILD_FAILED } tp_result_t;
/* Pure preparation; no peripheral/IRQ/bus enable. Caller must stop/isolate first. */
tp_result_t target_prepare(target_engine_t *e,const batt_profile_t *p,int soc);
#endif
