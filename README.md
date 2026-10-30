# CHIPLER Battery Emulator — Universal Smart Smartphone Battery Emulator

[![Status](https://img.shields.io/badge/Hardware_Status-Hardware_Verified_Universal-brightgreen.svg)]()
[![Target](https://img.shields.io/badge/MCU-RP2350_Dual_ARM_Cortex--M33-blue.svg)]()
[![Bus](https://img.shields.io/badge/Protocol-SMBus_1.8V_Level--Shifted-orange.svg)]()
[![Tests](https://img.shields.io/badge/Host_Tests-35%2F35_PASS_(100%25)-success.svg)]()
[![License](https://img.shields.io/badge/License-MIT-lightgrey.svg)](LICENSE)

*Read this in other languages:* [العربية 🇸🇦](README_AR.md)

> **The first open-source hardware emulator for Smartphone Smart Batteries powered by an autonomous Auto-Learn Engine and safe 1.8V bidirectional level shifting, built on the dual-core RP2350 MCU.**  
> Successfully benchmarked and proven on physical hardware: boots **modern smartphones** without an OEM battery, with real-time battery percentage control (1% – 100%) reflected immediately on the operating system.

---

## 👨‍💻 About the Developer & Platform

* **Lead Developer:** **Eng. Hamed Lasheen**
* **Organization & Brand:** **CHIPLER**
* 🌐 **Official Website:** [https://chipler.net/](https://chipler.net/)
* 📱 **Official Facebook Page:** [https://www.facebook.com/Chipler.eg](https://www.facebook.com/Chipler.eg)

### 📌 Platform Overview (Chipler.net):
> **CHIPLER (chipler.net):** A specialized engineering platform dedicated to microelectronics, smartphone board-level diagnostic tools, and universal hardware emulators. Our mission is to engineer high-precision, open-source testing solutions that empower repair engineers and electronics technicians globally to diagnose complex logic boards, simulate smart peripherals, and troubleshoot power delivery safely.

---

## 🌟 Key Technical Features

1. **Full Apple Smart Battery Protocol Emulation (SMBus 1.8V):**
   - Real-time response to all standard Veridian battery registers (`0x0B`) including voltage, current, temperature, design capacity, and cryptographic authentication queries.
   - Microsecond-level on-the-fly Packet Error Code (PEC / CRC-8) calculation and verification.

2. **Autonomous Auto-Learn Handshake Engine:**
   - No manual model selection or complex menu configuration required.
   - Upon target phone power-up, the firmware listens silently on the bus in `LISTEN` mode, captures the initial PMU query signature, identifies the matching profile (`COMPATIBLE`), and transitions automatically into `RUNNING` mode.

3. **Dynamic State-of-Charge (SOC) Control:**
   - Instantly adjust the emulated battery percentage between **1% and 100%** via the 3.5" color touchscreen or the PC serial console.
   - Real-time updates reflect immediately in iOS Settings and the top status bar without restarting the phone.

4. **100% Fail-Safe Hardware Isolation (1.8V Level Shifting):**
   - Complete protection for the phone's Power Management IC (PMU) via a dual-channel `BSS138 N-MOSFET` bidirectional translator.
   - High-speed, balanced pull-up network: **4.7kΩ** on the 1.8V phone side and **10kΩ** on the 3.3V MCU side (ultra-low 0.7mA bus loading, crisp square waveforms).
   - Low-noise linear regulator `AMS1117-1.8V` equipped with a 22µF tantalum output capacitor and a 150Ω permanent minimum load resistor to prevent voltage drift under all conditions.

5. **Integrated Power & Telemetry Monitoring:**
   - Onboard `INA226` high-precision digital power monitor on a dedicated internal 3.3V I2C bus with a 5mΩ low-side shunt resistor.
   - Continuous live telemetry measuring phone voltage, current draw (up to **5A continuous**), and power consumption.

---

## 📥 Official Downloads & Package Center

> 🚀 **Download the complete package exclusively at:**  
> ### 🌐 [https://chipler.net/](https://chipler.net/)
> Visit the official Chipler portal to download the ready-to-flash firmware (`.uf2`), high-resolution assembly guides, and complete wiring schematics.

---

## 📖 Step-by-Step Field Assembly Guide (PDF)

A comprehensive, beginner-friendly assembly manual has been prepared with full photographic evidence:
* 🌐 **[Download Assembly Guides & Firmware via chipler.net](https://chipler.net/)**
  - **Official product photographs** of every physical component.
  - **Complete Bill of Materials (BOM)** with verified component ratings.
  - **Detailed SOT-223 and SOT-23 pinout diagrams**.
  - **Wire-by-wire color-coded breadboard interconnection table**.
  - Safe phone battery flex termination guide (B+, GND, SDA, SCL).

---

## 🗂️ Repository Directory Structure

```text
CHIPLER-Battery-Emulator/
├── README.md                           # Main documentation (English)
├── README_AR.md                        # Documentation in Arabic
├── LICENSE                             # MIT Open Source License (Eng. Hamed Lasheen)
├── .gitignore                          # Git build and cache exclusions
├── docs/
│   ├── CHIPLER_Assembly_Guide_EN.pdf   # Complete Assembly Guide (English Edition)
│   └── CHIPLER_Assembly_AR.pdf         # Complete Assembly Guide (Arabic Edition)
├── firmware/
│   ├── source/                         # Clean canonical C source code (Pico SDK)
│   │   ├── CMakeLists.txt              # Primary firmware build specification
│   │   ├── pico_sdk_import.cmake       # Raspberry Pi Pico SDK importer
│   │   ├── boards/                     # RP2350-USB-A custom board definition
│   │   ├── config/                     # System parameters & safety boundaries
│   │   ├── src/                        # Core source tree (auto, batt, hal, ui, gfx, settings)
│   │   ├── bench/                      # Diagnostic pulse and translator benchmark suites
│   │   └── tests/                      # 35 host unit test suites (100% CTest pass)
│   └── releases/                       # Pre-compiled UF2 firmware ready to flash
│       ├── CHIPLER_AUTO_LEARN_P1_RC1_fd0c96b.uf2      # Latest Auto-Learn build (Multi-Model Support)
│       └── SHA256SUMS.txt                             # Cryptographic SHA256 checksum
```

---

## ⚡ Quick Pinout Reference

| Component | Pin | Connects To | Voltage | Function |
|---|---|---|---|---|
| **RP2350** | **3V3** | Breadboard 3.3V Rail | 3.3V | Powers LCD, INA226, and AMS1117 VIN |
| **RP2350** | **GND** | System Ground Rail | 0V | Common ground for MCU, LCD, sensor, and phone |
| **RP2350** | **GP0** | LCD Pin T_DO | 3.3V | Touch SPI MISO (Read coordinates) |
| **RP2350** | **GP1** | LCD Pin CS | 3.3V | LCD Chip Select |
| **RP2350** | **GP2** | LCD Pins SCK + T_CLK | 3.3V | Shared SPI Clock for Display & Touch |
| **RP2350** | **GP3** | LCD Pins SDI + T_DIN | 3.3V | Shared SPI MOSI for Display & Touch |
| **RP2350** | **GP4** | LCD Pin RESET | 3.3V | Hardware Reset for Display Controller |
| **RP2350** | **GP5** | LCD Pin DC/RS | 3.3V | Display Data / Command Select |
| **RP2350** | **GP6** | Q1 Drain (BSS138) | 3.3V | Battery SMBus SDA (MCU side + 10kΩ to 3.3V) |
| **RP2350** | **GP7** | Q2 Drain (BSS138) | 3.3V | Battery SMBus SCL (MCU side + 10kΩ to 3.3V) |
| **RP2350** | **GP8** | INA226 SDA | 3.3V | Dedicated internal I2C telemetry data |
| **RP2350** | **GP9** | INA226 SCL | 3.3V | Dedicated internal I2C telemetry clock |
| **RP2350** | **GP10** | INA226 ALERT | 3.3V | Overcurrent interrupt (internal pull-up enabled) |
| **RP2350** | **GP26** | LCD Pin T_CS | 3.3V | Touchscreen Chip Select |
| **RP2350** | **GP27** | LCD Pin T_IRQ | 3.3V | Touchscreen Pen Interrupt |
| **AMS1117** | **VOUT (2)** | Breadboard 1.8V Rail | 1.8V | Powers Q1/Q2 gates and 4.7kΩ phone-side pull-ups |
| **Q1 Source** | **Pin 2** | iPhone Battery SDA | 1.8V | Battery SMBus data to phone (+ 4.7kΩ to 1.8V) |
| **Q2 Source** | **Pin 2** | iPhone Battery SCL | 1.8V | Battery SMBus clock to phone (+ 4.7kΩ to 1.8V) |

---

## 🚀 How to Flash & Run

1. **Enter BOOTSEL Mode:**
   - Hold down the `BOOTSEL` button on the RP2350 development board.
   - Plug the USB cable into your computer, then release the button.
   - A removable mass storage drive named `RPI-RP2` will appear.

2. **Deploy the Firmware:**
   - Drag and drop the approved UF2 binary:
     [`firmware/releases/CHIPLER_AUTO_LEARN_P1_RC1_fd0c96b.uf2`](firmware/releases/CHIPLER_AUTO_LEARN_P1_RC1_fd0c96b.uf2)
   - The board will automatically reboot, and the LCD will immediately illuminate displaying the live telemetry interface in `LISTEN` mode.

3. **Power & Boot the Target Phone:**
   - Set an external regulated DC bench power supply to **4.0V** with a **2.5A** current limit.
   - Connect the power supply through the INA226 shunt to the phone battery flex (B+ and GND).
   - Connect the 1.8V level-shifted SDA and SCL lines to the phone battery flex.
   - Press the iPhone power button. The emulator will automatically detect the bus transactions, transition to `RUNNING` mode, and display the battery percentage on the iPhone screen.

---

## 🧪 Verification & Test Suite

The codebase has undergone rigorous engineering testing and passed with **100% compliance**:
- **35/35 Host Unit Test Suites (CTest):** 100% PASS in 1.19 seconds.
- **225 Auto-Learn Assertions:** Full verification of the handshake engine, fail-closed flash persistence, and profile matching.
- **7,886 UI Test Cycles:** Complete regression test verifying automatic mode transitions without manual picker intervention.
- **Physical Hardware Verification:** Live boot verified on multiple target smartphone models, with real-time SOC adjustment (50% to 100%) confirmed on the device display.

---

## 📜 License

This project is licensed under the open-source **[MIT License](LICENSE)**. You are free to use, modify, and distribute this software with attribution to the author:  
**Eng. Hamed Lasheen — CHIPLER Platform (chipler.net)**.

---

## 📬 Contact & Inquiries

* 🌐 **Website:** [chipler.net](https://chipler.net/)
* 📘 **Facebook:** [facebook.com/Chipler.eg](https://www.facebook.com/Chipler.eg)
* 💼 For technical inquiries, collaboration, or hardware diagnostic tools development, please reach out through the official links above.
