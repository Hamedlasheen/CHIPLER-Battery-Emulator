/*
 * CHIPLER P1_ILI9341_TOUCH board configuration (revision P1_r1).
 *
 * STATUS: PROVISIONAL. Pin numbers mirror 01_Hardware/CODEX_NETLIST_r1.csv (checked automatically by
 * tools/CLAUDE_check_netlist.py). They are a proposal until the owner confirms the module and board.
 * Nothing here is hardware-verified. The touch chip is marked HR2046 at U1 on the received V1.1 module
 * (photo-confirmed marking; the owner reports touch tracking and calibration worked on bench r7 without a capture; main-firmware stability is NOT proven). ILI9341 is only the display driver.
 *
 * Differences from the historical round-panel board:
 *   GP0  : SPI0 RX (touch T_DO only)  - was LCD DC
 *   GP5  : LCD DC                     - was BUS_EN. There is NO translator enable in this variant, so
 *                                       PIN_BAT_BUS_EN is deliberately NOT defined (any use fails to compile).
 *   GP26 : T_CS, GP27 : T_IRQ         - were DOWN/UP keys
 *   GP29 : unconnected reserve (no key; leave empty)
 */
#ifndef CHIPLER_BOARD_P1_H
#define CHIPLER_BOARD_P1_H

#define CHIPLER_VARIANT_ID   "P1_ILI9341_TOUCH"
#define CHIPLER_REVISION     "beta 1 u063"

/* ---------------- Shared SPI0 (display + touch) ---------------- */
#define LCD_SPI_INSTANCE  spi0
#define PIN_SPI_MISO      0    /* touch T_DO only; LCD SDO is intentionally left unconnected */
#define PIN_LCD_CS        1
#define PIN_LCD_SCK       2    /* shared with T_CLK */
#define PIN_LCD_MOSI      3    /* shared with T_DIN */
#define PIN_LCD_RST       4
#define PIN_LCD_DC        5
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ        (10u * 1000u * 1000u)   /* ILI9341 V1.05 p237 18.3.4: write cycle >= 100 ns => <= 10 MHz. SDK divider (150 MHz clk_peri) gives 9.375 MHz */
#endif
_Static_assert(LCD_SPI_HZ <= 10000000u, "LCD_SPI_HZ above the ILI9341 published write-clock limit (10 MHz); raise only after a bench test");
#define LCD_SPI_HZ_TOUCH  (1u * 1000u * 1000u)    /* XPT2046-family limit is 2 MHz; stay at 1 MHz */

/* ---------------- Display: ILI9341 2.8" 320x240 landscape ---------------- */
#define LCD_CONTROLLER    3    /* 1 = GC9A01 (historical), 2 = ST7789 (historical), 3 = ILI9341 */
#define LCD_WIDTH         320
#define LCD_HEIGHT        240
/* MADCTL bits: MY 0x80, MX 0x40, MV 0x20, BGR 0x08. 0x28 = MV|BGR landscape; 0xE8 = rotated 180 deg.
 * Colour order / orientation of the owner's module are UNVERIFIED (see README bring-up checklist). */
#ifndef LCD_MADCTL
#define LCD_MADCTL        0x28
#endif
#define LCD_MADCTL_ROT180 0xE8
#ifndef LCD_INVERT
#define LCD_INVERT        0
#endif
#ifndef LCD_SET_ID
#define LCD_SET_ID        "P1"
#endif
#ifndef DISPLAY_VARIANT
#define DISPLAY_VARIANT   "ILI9341 320x240 touch"
#endif

/* ---------------- Touch (HR2046 at U1; owner-reported OK on bench r7) ----------------
 * TOUCH_BACKEND 0 = none (input layer only), 1 = XPT2046/ADS7846-family command set.
 * The backend only issues standard 8-bit control words on a dedicated CS. It never touches I2C,
 * the battery bus or any power pin. If the HR2046 does not answer these commands the raw values are meaningless; the
 * TOUCH DIAG screen shows them so the owner can tell. Pen detection uses the pressure measurement,
 * T_IRQ is only read for diagnostics until its polarity is confirmed (TOUCH_IRQ_GATE 0). */
#define PIN_TOUCH_CS      26
#define PIN_TOUCH_IRQ     27
#ifndef TOUCH_BACKEND
#define TOUCH_BACKEND     1
#endif
#define TOUCH_IRQ_GATE    0
#define TOUCH_IRQ_ACTIVE_LOW 1

/* ---------------- Input: TOUCH ONLY (CODEX_TOUCH_ONLY_INPUT_UPDATE) ----------------
 * No external UP/DOWN/OK keys. GP26/GP27 belong to the touch controller, GP29 is an unconnected reserve
 * (input, no pulls). The board BOOT/RESET buttons are unchanged. Old key wires must be removed from
 * GP26/GP27/GP29, otherwise a pressed key could pull T_CS/T_IRQ to ground. */
#define PIN_RESERVED_GP29 29
#define BTN_ACTIVE_LOW    1
#define PIN_BTN_UP        -1
#define PIN_BTN_DOWN      -1
#define PIN_BTN_OK        -1

/* ---------------- Battery bus (I2C1 target, via external translator) ----------------
 * No enable line exists for the discrete-MOSFET translator. Stopping releases GP6/GP7 to inputs
 * without pulls; that does NOT electrically isolate the translator or cut the phone supply. */
#define BAT_I2C_INSTANCE  i2c1
#define PIN_BAT_SDA       6
#define PIN_BAT_SCL       7
#define BAT_BUS_EN_PRESENT 0

/* ---------------- Power monitor (I2C0 master) - OWNER LOCKED: GP8/GP9/GP10 ---------------- */
#define PM_I2C_INSTANCE   i2c0
#define PIN_PM_SDA        8
#define PIN_PM_SCL        9
#define PIN_PM_ALERT      10
#define PM_I2C_HZ         (100u * 1000u)
#define INA226_ADDR       0x40
#define INA228_ADDR       INA226_ADDR   /* legacy name still used by hal/ina228.c */
#define SHUNT_OHMS        0.010f /* R010 nominal; NOT calibrated */
#define CURRENT_MAX_A     8.0f

/* ---------------- Output control: reserved, no physical switch fitted ---------------- */
#define PIN_OUT_EN        28
#define PROTECTION_HARDWARE_FITTED 0
#define PROTOTYPE_INPUT_V 5.0f
#define PROTOTYPE_INPUT_A 5.0f

#define PIN_WS2812        16

#define LIMIT_OV_V        4.45f
#define LIMIT_UV_V        3.00f  /* owner request 2026-10-23 (was 3.30) */
#define LIMIT_OC_A        5.00f  /* owner request 2026-10-23 (was 3.50) */

/* Charge test has no hardware in this prototype. The landscape CHARGE TEST screens (ported from the round UI)
 * exist in the code but are unreachable unless CHG_UI_ENABLED is 1; the owner chose to hide the tile. Even when
 * shown, START stays refused while CHG_SINK_FITTED is 0. */
#define CHG_SINK_FITTED          0
#define CHG_INPUT_SENSOR_FITTED  0
#define CHG_UI_ENABLED           0

#endif
