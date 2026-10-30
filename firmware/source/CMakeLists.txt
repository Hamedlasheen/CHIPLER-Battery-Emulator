cmake_minimum_required(VERSION 3.13)

# Board: custom header for the (assumed) Waveshare RP2350-USB-A
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD chipler_rp2350_usb_a CACHE STRING "Board")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

# P1_r1: every firmware target is the ILI9341 320x240 touch variant. GFX_W/GFX_H size the shared
# framebuffer renderer (default 240x240 in gfx.h only for the historical host tests).
# PICO_MEM_IN_RAM keeps memcpy/memset in SRAM: the I2C IRQ path calls them (tools/CLAUDE_check_isr_ram.py).
# Stacks: core 0 gets 8 KiB (SDK default 2 KiB; newlib float printf + LOG + settings code are deep), core 1 supplies its own
# 8 KiB stack in main.c (multicore_launch_core1_with_stack), so no default core-1 reservation.
add_compile_definitions(CHIPLER_P1_ILI9341_TOUCH=1 GFX_W=320 GFX_H=240 PICO_MEM_IN_RAM=1 PICO_STACK_SIZE=0x2000 PICO_CORE1_STACK_SIZE=0)

include(pico_sdk_import.cmake)
project(chipler_battery_emulator C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

set(COMMON_SRC
    src/gfx/gfx.c src/gfx/fonts_data.c
    src/ui/ui_ls.c src/ui/ui_nav.c
    src/input/buttons.c src/input/touch_core.c src/input/touch_hw.c
    src/settings/settings_store.c src/settings/owner_state.c src/hal/spi_bus.c
    src/safety/safety.c
    src/batt/profile.c src/batt/target_engine.c src/batt/soc_ctrl.c src/batt/owner_words.c
    src/charge/charge_diag.c src/ctrl/ctrl_cycle.c src/log/log_ring.c src/log/log.c
    src/batt/smbus_core.c src/batt/capture_replay.c src/batt/target_prepare.c
    src/batt/smbus_target.c src/batt/live_prepare.c src/batt/live_wire.c src/hal/live_port.c
    src/batt/auth_gate.c src/batt/selector_pages.c src/batt/live_setup.c
    src/auto/auto_core.c src/auto/auto_ctl.c src/auto/auto_templates.c src/auto/auto_flash.c
    src/hal/ina226_core.c src/hal/ina226_pico.c src/hal/power_monitor.c
    src/hal/ina228_core.c
    src/hal/lcd.c src/hal/ina228.c src/hal/board_io.c src/hal/battery_bus.c src/diag/core1_diag.c)

set(COMMON_LIBS pico_stdlib pico_multicore pico_sync pico_i2c_slave pico_flash
    hardware_spi hardware_dma hardware_i2c hardware_adc hardware_watchdog hardware_irq hardware_flash)

# Review 102: reproducible images. The SDK stores __DATE__ as the program build date by default, so two builds of the
# same source on different days differ. The build identity is an explicit string instead (release builds pass
# -DCHIPLER_BUILD_ID=<release id>); it is stored in binary info and printed at boot.
set(CHIPLER_BUILD_ID "dev" CACHE STRING "build identity (stored as program build date, printed at boot)")
function(chipler_target name main)
    add_executable(${name} ${main} ${COMMON_SRC})
    target_compile_definitions(${name} PRIVATE PICO_PROGRAM_BUILD_DATE="${CHIPLER_BUILD_ID}" CHIPLER_BUILD_ID_STR="${CHIPLER_BUILD_ID}")
    target_include_directories(${name} PRIVATE src config)
    target_compile_options(${name} PRIVATE -Wall -Wno-unused-parameter -Wno-misleading-indentation -fno-tree-loop-distribute-patterns -fno-optimize-crc)
    # I2C target engine and the live SMBus path run from SRAM (called from the I2C IRQ)
    target_compile_definitions(${name} PRIVATE TE_RAMFUNC=__attribute__\(\(section\(\".time_critical.te\"\)\)\))
    target_link_libraries(${name} ${COMMON_LIBS})
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_add_extra_outputs(${name})
endfunction()

# P1_r1: one matched set. MAIN is the only image shipped in 06_Releases/P1_r1.
#   <SET>_2_UI_DEMO  same UI, synthetic V/A/W labelled SIMULATED, output never armable (internal check)
#   <SET>_3_MAIN     full UI, real telemetry only, no phone profile armable
function(chipler_p1_set set)
    chipler_target(${set}_2_UI_DEMO src/main.c)
    chipler_target(${set}_3_MAIN src/main.c)
    foreach(t ${set}_2_UI_DEMO ${set}_3_MAIN)
        target_compile_definitions(${t} PRIVATE LCD_SET_ID="P1" DISPLAY_VARIANT="ILI9341 320x240 touch")
    endforeach()
    target_compile_definitions(${set}_2_UI_DEMO PRIVATE CHIPLER_SIM_TELEMETRY=1)
endfunction()

chipler_p1_set(P1_ILI9341_TOUCH)

# display + touch bring-up image (nothing else wired/needed)
chipler_target(bench_p1_display_touch bench/p1_display_touch_bench.c)
# live SMBus exerciser on the production path (isolated bench master only)
chipler_target(bench_smbus_live bench/smbus_live_bench.c)
# Gate 2 bench: I2C0 master <-> I2C1 target on one board, no phone (display-independent)
chipler_target(bench_i2c_loopback bench/loopback_test.c)
# PM-ID-001 bench: INA226 identity + raw readings on I2C0 (GP8/GP9/GP10), no phone
chipler_target(bench_ina226_id bench/ina226_id_test.c)
target_include_directories(bench_ina226_id PRIVATE bench)
# CR-006-INA226 Phase A bench: ina226_core + thin Pico HAL (bench target only; main unchanged)
chipler_target(bench_ina226_drv bench/ina226_drv_bench.c)
# Translator edge-speed bench: open-drain PIO pulses on GP6/GP7 only (never drives HIGH); display/INA226 pins untouched; phone disconnected
chipler_target(bench_translator_pulse bench/translator_pulse_bench.c)
target_include_directories(bench_translator_pulse PRIVATE bench)
target_link_libraries(bench_translator_pulse hardware_pio hardware_clocks)
