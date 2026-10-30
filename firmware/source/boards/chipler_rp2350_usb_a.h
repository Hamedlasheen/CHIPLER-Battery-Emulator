/*
 * Board header for the Waveshare RP2350-USB-A as used by CHIPLER (ASSUMED board - confirm).
 * From vendor docs: RP2350A, W25Q16JV 2 MB QSPI flash, 12 MHz crystal, WS2812 on GP16,
 * USB-A host port on GP12/GP13 (PIO-USB, unused here). No default UART/I2C/SPI pins are
 * assigned on purpose: CHIPLER drivers use explicit pins from config/board_config.h.
 */
#ifndef _BOARDS_CHIPLER_RP2350_USB_A_H
#define _BOARDS_CHIPLER_RP2350_USB_A_H

// pico_cmake_set PICO_PLATFORM=rp2350

#define PICO_RP2350A 1
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif
// pico_cmake_set_default PICO_FLASH_SIZE_BYTES = (2 * 1024 * 1024)
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif
#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#endif
/* Chip stepping unknown until the owner reads the marking; keep A2 workarounds enabled */
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif
#define CHIPLER_WS2812_PIN 16
/* V02-02: stdio USB writes never wait long for a host that is not reading (main LOG uses a
 * ring + log_pump that only writes what fits; this bounds the bench tools' printf as well). */
#ifndef PICO_STDIO_USB_STDOUT_TIMEOUT_US
#define PICO_STDIO_USB_STDOUT_TIMEOUT_US 2000
#endif
#endif
