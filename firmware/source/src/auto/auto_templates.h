#ifndef CHIPLER_AUTO_TEMPLATES_H
#define CHIPLER_AUTO_TEMPLATES_H
#include "auto_core.h"
#define AUTO_T_12P_POWER 1u     /* stored in profiles: never reuse this id for another map */
#define AUTO_MAX_TEMPLATES 4
int auto_templates_build(auto_template_t *out, int max);
#endif
