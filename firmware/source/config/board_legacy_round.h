/*
 * CHIPLER Smart Battery Emulator - board configuration
 *
 * STATUS: PROVISIONAL / NOT BENCH-VERIFIED (2026-10-15).
 * Assumes Waveshare RP2350-USB-A (RP2350A, USB-A on GP12/13, WS2812 on GP16).
 * Owner must confirm board silkscreen + continuity before wiring (checklist H in
 * "02-05 Hardware Architecture" doc). Every pin below is a PROPOSAL.
 */
#ifndef CHIPLER_BOARD_CONFIG_H
#define CHIPLER_BOARD_CONFIG_H

/* ---------------- Display (SPI0) ---------------- */
#define PIN_LCD_DC        0
#define PIN_LCD_CS        1
#define PIN_LCD_SCK       2   /* module pin labelled "SCL" (it is SPI clock) */
#define PIN_LCD_MOSI      3   /* module pin labelled "SDA" (it is SPI MOSI)  */
#define PIN_LCD_RST       4
#define LCD_SPI_INSTANCE  spi0
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ        (20u * 1000u * 1000u)  /* start safe; raise after bench test */
#endif

/* Panel geometry: round 240x240 (GC9A01-class per supplier photo; UNVERIFIED) */
#define LCD_WIDTH         240
#define LCD_HEIGHT        240

/* Controller selection: 1 = GC9A01 (expected), 2 = ST7789 (fallback test) */
#ifndef LCD_CONTROLLER
#define LCD_CONTROLLER    1
#endif
/* Colour order / inversion knobs for bench bring-up */
#ifndef LCD_MADCTL
#define LCD_MADCTL        0x08   /* BGR bit set, as in common GC9A01 drivers */
#endif
#ifndef LCD_INVERT
#define LCD_INVERT        1
#endif
/* V02-04: build identity. Each prebuilt display set (A..D) passes these on the compiler line;
 * shown on the startup/test screen and in the USB log. Settings live in the UF2 itself - they do
 * NOT persist from one UF2 to another. */
#ifndef LCD_SET_ID
#define LCD_SET_ID        "A"
#endif
#ifndef DISPLAY_VARIANT
#define DISPLAY_VARIANT   "GC9A01 BGR INV"
#endif

/* ---------------- Buttons (CR-003-r1: three independent keys) ----------------
 * PROVISIONAL until the owner's board silkscreen is matched. Each key: one leg to the GPIO,
 * the diagonally opposite leg to GND; internal pull-up enabled (+ optional external 10k / 100nF).
 * GP29 was VIN_SENSE (ADC3) in v0.1.x and is REASSIGNED to OK: all 15 header GPIOs were
 * allocated and GP29 was the only redundant function (independent PSU OV protection is still unqualified). */
#define PIN_BTN_UP        27   /* header L6 (assumed Waveshare RP2350-USB-A) */
#define PIN_BTN_DOWN      26   /* header L7 */
#define PIN_BTN_OK        29   /* header L4 */
#define BTN_ACTIVE_LOW    1

/* ---------------- Battery bus (I2C1 target, via level translator) -------- */
#define BAT_I2C_INSTANCE  i2c1
#define PIN_BAT_SDA       6
#define PIN_BAT_SCL       7
#define PIN_BAT_BUS_EN    5    /* translator/buffer enable, 100k pull-down */
#define BAT_BUS_EN_PRESENT 1

/* ---------------- Power monitor (I2C0 master) ---------------- */
#define PM_I2C_INSTANCE   i2c0
#define PIN_PM_SDA        8
#define PIN_PM_SCL        9
#define PIN_PM_ALERT      10
#define PM_I2C_HZ         (100u * 1000u)
#define INA228_ADDR       0x40   /* A0=A1=GND (breakout default; verify) */
#define SHUNT_OHMS        0.010f /* Owner R010 nominal; NOT calibrated */
#define CURRENT_MAX_A     8.0f

/* ---------------- Output control / sense ---------------- */
#define PIN_OUT_EN        28   /* eFuse EN, 100k pull-down -> OFF at reset */
#define PROTECTION_HARDWARE_FITTED 0 /* Owner 2026-10-18: absent in prototype; final design only */
#define PROTOTYPE_INPUT_V 5.0f
#define PROTOTYPE_INPUT_A 5.0f
/* PIN_VIN_SENSE removed in v0.2.0-dev (GP29 -> PIN_BTN_OK) */

/* ---------------- On-board ---------------- */
#define PIN_WS2812        16

/* ---------------- Safety limits (defaults, owner to confirm) ------------- */
#define LIMIT_OV_V        4.45f
#define LIMIT_UV_V        3.30f
#define LIMIT_OC_A        3.50f

/* ---- CHARGE TEST mode (CP-09, merged UI in v0.3.0-dev) ----
 * Hardware that a real charge test needs does NOT exist yet. Keep both 0 until a reviewed
 * source/sink design, calibration and fault tests exist (CP-09 acceptance, after the main mission).
 * With CHG_SINK_FITTED = 0 the firmware can never start a charge test. */
#define CHG_SINK_FITTED          0
#define CHG_INPUT_SENSOR_FITTED  0

#endif
