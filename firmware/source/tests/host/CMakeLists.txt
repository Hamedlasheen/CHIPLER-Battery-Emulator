cmake_minimum_required(VERSION 3.13)
project(chipler_host_tests C)
set(CMAKE_C_STANDARD 11)
set(SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
# Review 110 R110-3: which configuration is this? Asked of the C preprocessor with the flags the tests are built with
# (=1, =0x1, -D...=true-ish all agree with the firmware's #if), never by a text match on CMAKE_C_FLAGS.
include(CheckCSourceCompiles)
check_c_source_compiles("#ifndef CHIPLER_OWNER_PHONE_TEST\n#error off\n#elif !CHIPLER_OWNER_PHONE_TEST\n#error off\n#endif\nint main(void){return 0;}" CHIPLER_HOST_OWNER)
check_c_source_compiles("#ifndef CHIPLER_12P_REFERENCE\n#error off\n#elif !CHIPLER_12P_REFERENCE\n#error off\n#endif\nint main(void){return 0;}" CHIPLER_HOST_REF)
if(CHIPLER_HOST_OWNER)
  set(CHIPLER_HOST_CONFIG owner)
else()
  set(CHIPLER_HOST_CONFIG default)
endif()
message(STATUS "CHIPLER host test configuration: ${CHIPLER_HOST_CONFIG} (12P_REFERENCE=${CHIPLER_HOST_REF})")
add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2)

add_executable(unit_tests test_main.c
    ${SRC}/input/buttons.c ${SRC}/ui/ui_nav.c ${SRC}/safety/safety.c
    ${SRC}/batt/profile.c ${SRC}/batt/target_engine.c ${SRC}/batt/soc_ctrl.c
    ${SRC}/charge/charge_diag.c ${SRC}/hal/ina228_core.c ${SRC}/hal/ina226_core.c ${SRC}/batt/smbus_core.c ${SRC}/batt/capture_replay.c ${SRC}/batt/target_prepare.c ${SRC}/log/log_ring.c ${SRC}/ctrl/ctrl_cycle.c)
target_link_libraries(unit_tests m)

add_executable(render_screens render_screens.c
    ${SRC}/gfx/gfx.c ${SRC}/gfx/fonts_data.c ${SRC}/ui/ui_render.c
    ${SRC}/batt/profile.c ${SRC}/safety/safety.c ${SRC}/batt/soc_ctrl.c
    ${SRC}/charge/charge_diag.c)
target_link_libraries(render_screens m)

enable_testing()
add_executable(bus_order_tests test_bus_order.c
    ${SRC}/hal/battery_bus.c ${SRC}/batt/target_prepare.c ${SRC}/batt/target_engine.c ${SRC}/batt/profile.c
    ${SRC}/batt/smbus_target.c ${SRC}/batt/live_prepare.c ${SRC}/batt/live_wire.c
    ${SRC}/batt/auth_gate.c ${SRC}/batt/selector_pages.c ${SRC}/batt/live_setup.c ${SRC}/batt/owner_words.c)   # review 102 F-05: missing since the auth gate/selector split
target_include_directories(bus_order_tests PRIVATE fake_pico ../../config)
add_test(NAME bus_order_tests COMMAND bus_order_tests)
add_executable(power_adapter_tests test_power_adapter.c ${SRC}/hal/ina226_core.c ${SRC}/log/log_ring.c)
target_include_directories(power_adapter_tests PRIVATE fake_pico ../../config)
target_link_libraries(power_adapter_tests m)
add_test(NAME power_adapter_tests COMMAND power_adapter_tests)
add_executable(power_legacy_tests test_power_legacy.c)
target_include_directories(power_legacy_tests PRIVATE fake_pico ../../config)
add_test(NAME power_legacy_tests COMMAND power_legacy_tests)
add_test(NAME unit_tests COMMAND unit_tests)
add_executable(live_smbus_tests test_live_smbus.c ${SRC}/batt/smbus_target.c ${SRC}/batt/live_prepare.c ${SRC}/batt/live_wire.c ${SRC}/batt/profile.c)
add_test(NAME live_smbus_tests COMMAND live_smbus_tests)
add_executable(soc_phone_tests test_soc_phone.c ${SRC}/batt/smbus_target.c ${SRC}/batt/live_prepare.c ${SRC}/batt/soc_ctrl.c ${SRC}/batt/profile.c ${SRC}/batt/owner_words.c)
add_test(NAME soc_phone_tests COMMAND soc_phone_tests)
add_executable(splash_gate_tests test_splash_gate.c)
add_test(NAME splash_gate_tests COMMAND splash_gate_tests)
add_executable(auth_gate_tests auth_gate_tests.c ${SRC}/batt/auth_gate.c ${SRC}/batt/selector_pages.c)
target_include_directories(auth_gate_tests PRIVATE ${SRC}/batt ${SRC})
add_test(NAME auth_gate_tests COMMAND auth_gate_tests)
add_executable(protocol_irq_tests protocol_irq_tests.c ${SRC}/batt/smbus_target.c ${SRC}/batt/auth_gate.c ${SRC}/batt/selector_pages.c)
target_include_directories(protocol_irq_tests PRIVATE ${SRC}/batt ${SRC})
add_test(NAME protocol_irq_tests COMMAND protocol_irq_tests)
add_executable(live_capture_tests test_live_captures.c ${SRC}/batt/smbus_target.c)
add_test(NAME live_capture_tests COMMAND live_capture_tests)
# DS-REUSE-13: unported historical contract/edge cases (block snapshot, guard balance,
# command sweep, A8 negatives, write bounds, wire rejection matrix, pointer policy).
add_executable(live_contract_tests test_live_contract.c ${SRC}/batt/smbus_target.c ${SRC}/batt/live_wire.c ${SRC}/batt/live_prepare.c ${SRC}/batt/profile.c)
add_test(NAME live_contract_tests COMMAND live_contract_tests)
add_executable(board_lock_tests test_board_lock.c ${SRC}/hal/board_io.c)
target_include_directories(board_lock_tests PRIVATE fake_pico ../../config)
add_test(NAME board_lock_tests COMMAND board_lock_tests)

# ---- P1_r1 additions (landscape 320x240 touch UI, touch core, settings, MMIO harness) ----
add_executable(touch_core_tests test_touch_core.c ${SRC}/input/touch_core.c)
target_link_libraries(touch_core_tests m)
add_test(NAME touch_core_tests COMMAND touch_core_tests)
add_executable(settings_tests test_settings.c ${SRC}/settings/settings_store.c)
target_link_libraries(settings_tests m)
add_test(NAME settings_tests COMMAND settings_tests)
set(LS_SRC ${SRC}/gfx/gfx.c ${SRC}/gfx/fonts_data.c ${SRC}/ui/ui_ls.c ${SRC}/ui/ui_nav.c
    ${SRC}/batt/profile.c ${SRC}/safety/safety.c ${SRC}/batt/soc_ctrl.c ${SRC}/charge/charge_diag.c)
add_executable(ui_ls_tests test_ui_ls.c ${LS_SRC} ${SRC}/input/touch_core.c)
target_compile_definitions(ui_ls_tests PRIVATE GFX_W=320 GFX_H=240)
target_link_libraries(ui_ls_tests m)
add_test(NAME ui_ls_tests COMMAND ui_ls_tests)
add_executable(render_ls render_ls.c ${LS_SRC})
target_compile_definitions(render_ls PRIVATE GFX_W=320 GFX_H=240)
target_link_libraries(render_ls m)
add_executable(live_port_mmio_tests test_live_port_mmio.c ${SRC}/batt/smbus_target.c ${SRC}/batt/live_wire.c
    ${SRC}/batt/live_prepare.c ${SRC}/batt/profile.c)
target_include_directories(live_port_mmio_tests PRIVATE fake_mmio ../../config ${SRC}/hal)
target_compile_definitions(live_port_mmio_tests PRIVATE CHIPLER_P1_ILI9341_TOUCH=1)
add_test(NAME live_port_mmio_tests COMMAND live_port_mmio_tests)
add_executable(spi_bus_tests test_spi_bus.c ${SRC}/hal/spi_bus.c ${SRC}/diag/core1_diag.c)
target_include_directories(spi_bus_tests PRIVATE fake_spi ../../config ${SRC})
target_compile_definitions(spi_bus_tests PRIVATE CHIPLER_P1_ILI9341_TOUCH=1 CHIPLER_HOST_SPI_FAKE=1)
add_test(NAME spi_bus_tests COMMAND spi_bus_tests)
foreach(t ds_touch12_tests ds_touch12_delta_tests)
    add_executable(${t} deepseek_touch/${t}.c ${SRC}/input/touch_core.c ${SRC}/settings/settings_store.c)
    target_include_directories(${t} PRIVATE ${SRC}/input ${SRC}/settings)
    target_compile_definitions(${t} PRIVATE DS_PATCHED=1)
    target_link_libraries(${t} m)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
find_program(TEST_SHELL NAMES sh bash REQUIRED)
add_test(NAME output_lock_static COMMAND ${TEST_SHELL} ${CMAKE_CURRENT_LIST_DIR}/../check_output_lock.sh ${CHIPLER_HOST_CONFIG})
add_test(NAME splash_gate_static COMMAND ${TEST_SHELL} ${CMAKE_CURRENT_LIST_DIR}/../check_splash_gate_static.sh ${SRC}/main.c)

# ---- Claude sub-agent additions (AG-LCD / AG-REPLAY / AG-TOUCH / AG-INA); host models only, hardware NOT RUN ----
add_executable(lcd_sequence_tests test_lcd_sequence.c fake_lcd/fake_sdk.c ${SRC}/diag/core1_diag.c
               ${SRC}/hal/lcd.c ${SRC}/hal/spi_bus.c ${SRC}/input/touch_hw.c)
target_include_directories(lcd_sequence_tests PRIVATE fake_lcd ../../config ${SRC}/hal ${SRC}/input ${SRC})
target_compile_definitions(lcd_sequence_tests PRIVATE CHIPLER_P1_ILI9341_TOUCH=1 CHIPLER_HOST_SPI_FAKE=1)
add_test(NAME lcd_sequence_tests COMMAND lcd_sequence_tests)
add_executable(full_replay_tests test_full_replay.c ${SRC}/batt/smbus_target.c ${SRC}/batt/live_wire.c
               ${SRC}/batt/live_prepare.c ${SRC}/batt/profile.c
               ${SRC}/batt/live_setup.c ${SRC}/batt/auth_gate.c ${SRC}/batt/selector_pages.c)   # review 105 R3: armed like battery_bus.c
target_include_directories(full_replay_tests PRIVATE fake_mmio ../../config ${SRC}/hal ${SRC})
target_compile_definitions(full_replay_tests PRIVATE CHIPLER_P1_ILI9341_TOUCH=1)
add_test(NAME full_replay_tests COMMAND full_replay_tests)
add_executable(touch_hw_sim_tests test_touch_hw_sim.c ${SRC}/input/touch_hw.c ${SRC}/input/touch_core.c)
target_include_directories(touch_hw_sim_tests PRIVATE fake_touch ../../config ../..)
target_compile_definitions(touch_hw_sim_tests PRIVATE TOUCH_BACKEND=1 CHIPLER_P1_ILI9341_TOUCH=1 AG_TOUCH_PATCHED=1)
target_link_libraries(touch_hw_sim_tests m)
add_test(NAME touch_hw_sim_tests COMMAND touch_hw_sim_tests)
add_executable(ina226_reallog_tests test_ina226_reallog.c ${SRC}/hal/ina226_core.c ${SRC}/log/log_ring.c)
target_include_directories(ina226_reallog_tests PRIVATE fake_pico ../../config)
target_link_libraries(ina226_reallog_tests m)
add_test(NAME ina226_reallog_tests COMMAND ina226_reallog_tests)

# ---- translator edge-speed bench (open-drain PIO pulses on GP6/GP7) ----
add_executable(translator_bench_plan_tests test_translator_bench_plan.c)
add_test(NAME translator_bench_plan_tests COMMAND translator_bench_plan_tests)
add_test(NAME translator_bench_static COMMAND ${TEST_SHELL} ${CMAKE_CURRENT_LIST_DIR}/../check_translator_bench_static.sh)
add_executable(owner_state_tests test_owner_state.c ${SRC}/settings/owner_state.c ${SRC}/settings/settings_store.c)
target_link_libraries(owner_state_tests m)
add_test(NAME owner_state_tests COMMAND owner_state_tests)
# Review 106: shared 12 Pro engine for every phone model outside families 15/16/17 (owner build); default build: unchanged
add_executable(shared12_tests test_shared12.c ${SRC}/batt/profile.c ${SRC}/batt/smbus_target.c ${SRC}/batt/live_prepare.c
    ${SRC}/batt/live_setup.c ${SRC}/batt/auth_gate.c ${SRC}/batt/selector_pages.c ${SRC}/batt/owner_words.c ${SRC}/batt/soc_ctrl.c
    ${SRC}/ui/ui_nav.c ${SRC}/input/buttons.c ${SRC}/settings/owner_state.c ${SRC}/settings/settings_store.c)
target_link_libraries(shared12_tests m)
add_test(NAME shared12_tests COMMAND shared12_tests)

# Review 102: owner phone-test contract suites (Codex 098 R2). They pin owner-only behaviour, so they are registered
# only in the owner configuration; the default configuration keeps its own contracts. Review 110 R110-3: decided by
# CHIPLER_HOST_OWNER (preprocessor), and suite_inventory_tests fails if the registration and the compiled flag disagree.
if(CHIPLER_HOST_OWNER)
  set(OWNER_BATT ${SRC}/batt/smbus_target.c ${SRC}/batt/live_prepare.c ${SRC}/batt/profile.c)
  add_executable(owner_words_tests test_owner_words.c ${OWNER_BATT} ${SRC}/batt/owner_words.c)
  add_test(NAME owner_words_tests COMMAND owner_words_tests)
  add_executable(owner_boot12_tests test_owner_boot12.c ${OWNER_BATT} ${SRC}/batt/live_wire.c)
  add_test(NAME owner_boot12_tests COMMAND owner_boot12_tests)
  add_executable(owner_soc_picker_tests test_owner_soc_picker.c ${OWNER_BATT} ${SRC}/batt/soc_ctrl.c ${SRC}/batt/owner_words.c
      ${SRC}/ui/ui_nav.c ${SRC}/input/buttons.c)
  add_test(NAME owner_soc_picker_tests COMMAND owner_soc_picker_tests)
  set(OWNER_SUITES_REGISTERED 3)
else()
  set(OWNER_SUITES_REGISTERED 0)
endif()
add_executable(suite_inventory_tests test_suite_inventory.c)
target_compile_definitions(suite_inventory_tests PRIVATE OWNER_SUITES_REGISTERED=${OWNER_SUITES_REGISTERED})
add_test(NAME suite_inventory_tests COMMAND suite_inventory_tests)
# AUTO_LEARN_P1_RC1: matcher / generator / store / controller (pure logic)
add_executable(auto_core_tests test_auto_core.c ${SRC}/auto/auto_core.c ${SRC}/auto/auto_ctl.c ${SRC}/auto/auto_templates.c)
target_include_directories(auto_core_tests PRIVATE ${SRC})
add_test(NAME auto_core_tests COMMAND auto_core_tests)
# AUTO_LEARN_P1_RC1 UI contract: built WITH -DCHIPLER_AUTO_LEARN=1 in every configuration (no phone-model path)
add_executable(auto_nav_tests test_auto_nav.c ${LS_SRC} ${SRC}/auto/auto_core.c)
target_compile_definitions(auto_nav_tests PRIVATE GFX_W=320 GFX_H=240 CHIPLER_AUTO_LEARN=1)
target_include_directories(auto_nav_tests PRIVATE ${SRC})
target_link_libraries(auto_nav_tests m)
add_test(NAME auto_nav_tests COMMAND auto_nav_tests)
