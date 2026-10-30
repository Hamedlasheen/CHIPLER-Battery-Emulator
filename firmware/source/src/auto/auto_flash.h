/* CHIPLER AUTO store, device half: the sector of the r8..r13 owner state (PICO_FLASH_SIZE - 2 sectors), shared with
 * those records (recognised as foreign, never rewritten). One page programmed per record (~1 ms, flash_safe_execute);
 * erase only for CLEAR, with replies off. Every write is read back and the record must scan as written. */
#ifndef CHIPLER_AUTO_FLASH_H
#define CHIPLER_AUTO_FLASH_H
#include "auto_core.h"
#include "auto_ctl.h"
#ifdef PICO_ON_DEVICE
bool auto_flash_scan(auto_store_t *s);                       /* false: region overlaps the program image */
auto_result_t auto_flash_save_profile(const auto_profile_t *p);
auto_result_t auto_flash_save_control(bool auto_enabled, uint32_t last_fp);
auto_result_t auto_flash_clear(void);
#endif
#endif
