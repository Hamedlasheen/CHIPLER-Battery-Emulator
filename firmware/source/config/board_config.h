/*
 * CHIPLER board configuration dispatcher (P1_r1).
 *
 *   CHIPLER_P1_ILI9341_TOUCH defined  -> config/board_p1.h        (new prototype, 320x240 + touch)
 *   not defined                       -> config/board_legacy_round.h (historical 240x240 round panel;
 *                                       kept only so the imported beta1 host tests keep building)
 *
 * Every firmware target in CMakeLists.txt is built with CHIPLER_P1_ILI9341_TOUCH.
 */
#ifndef CHIPLER_BOARD_CONFIG_DISPATCH_H
#define CHIPLER_BOARD_CONFIG_DISPATCH_H
#ifdef CHIPLER_P1_ILI9341_TOUCH
#include "board_p1.h"
#else
#include "board_legacy_round.h"
#endif
/* Build-configuration defaults shared by every board header (review 102). */
#ifndef CHIPLER_OWNER_PHONE_TEST
#define CHIPLER_OWNER_PHONE_TEST 0
#endif
#ifndef CHIPLER_12P_REFERENCE
#define CHIPLER_12P_REFERENCE 0
#endif
/* INA226 shunt-over-limit ALERT (hardware latch -> FAULT "MONITOR ALERT"). The default build keeps the original 0.60 A.
 * Review 102 F-08: the owner build raised LIMIT_OC_A to 5 A for a booting iPhone (peaks above 0.6 A are normal) but this
 * trip stayed at 0.60 A, a likely source of the unexplained start-up "FAULT MONITOR ALERT". The owner build uses the
 * owner's OC limit. Range check: 5 A x 0.010 ohm = 50 mV < 81.9 mV INA226 full scale. */
#if CHIPLER_OWNER_PHONE_TEST
#define INA_SOL_TRIP_A    LIMIT_OC_A
#else
#define INA_SOL_TRIP_A    0.60f
#endif
#endif
