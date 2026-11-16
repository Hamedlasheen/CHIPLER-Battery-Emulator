# CHIPLER Battery Emulator

**Hardware-level battery protocol emulator for smartphone motherboard diagnostics on a DC bench power supply.**

![Platform](https://img.shields.io/badge/MCU-RP2350B-blue)
![Protocol](https://img.shields.io/badge/Protocols-SWI%20%7C%20I2C%20%7C%20BSI-green)
![Status](https://img.shields.io/badge/Status-Active%20Development-orange)
![License](https://img.shields.io/badge/License-MIT-lightgrey)

---

## Overview

Modern smartphones verify battery presence, thermal status, and fuel-gauge telemetry at boot and periodically during operation. When powering a logic board from a standard DC bench power supply (`VBAT+` and `GND` only), missing battery communication triggers:

- Continuous boot loops or failure to trigger power-on
- **3-minute kernel panic reboots** (`Userspace watchdog timeout` / missing thermal & gas-gauge sensors)
- CPU/GPU throttling and charging circuit lockouts during board-level troubleshooting

**CHIPLER Battery Emulator** sits between your DC bench power supply and the smartphone's battery FPC connector. Powered by the **Raspberry Pi RP2350B**, it emulates real-time battery handshake protocols (SWI, I2C, and BSI/NTC signals) so the motherboard recognizes a healthy, fully authenticated battery during testing and data recovery.

---

## Key Features

- **Deterministic Protocol Emulation:** Uses RP2350B PIO (Programmable I/O) state machines and dedicated hardware peripherals for cycle-accurate **SWI (Single-Wire Interface)**, **I2C/SMBus**, and **BSI** responses.
- **Anti-Reboot Stability:** Emulates valid fuel-gauge registers (State of Charge, Voltage, Design Capacity, Cycle Count, and NTC Temperature) to prevent 3-minute diagnostic reboots.
- **Modular Hardware Design:**
  - **4-Layer Mainboard:** Houses the RP2350B MCU, level-shifting circuitry, ESD protection, and isolated power regulation.
  - **2-Layer Auxiliary / Adapter Board:** Interchangeable FPC interface boards tailored for specific device families.
- **Configurable Battery Profiles:** Easily switch between supported device profiles (`[e.g., iPhone X – 15 Pro Max / Custom Profiles]`).
- **Bench-Safe Protection:** Integrated level translation (`1.8V` logic compatibility) and transient voltage suppression to protect sensitive CPU/PMIC data lines.

---

## Hardware Architecture

### Core Specifications

| Component | Specification |
| :--- | :--- |
| **Microcontroller** | Raspberry Pi RP2350B (Dual-Core Arm Cortex-M33 / Hazard3 RISC-V + PIO) |
| **Logic Level Interface** | `1.8V` bi-directional level-shifted data lines (PMIC safe) |
| **Supported Protocols** | SWI / HDQ, I2C, BSI / NTC Analog Emulation |
| **Input Supply (Bench PSU)** | `3.8V – 4.3V DC` (Recommended: `4.0V`, `3A–5A` current limit) |
| **PCB Stackup** | 4-Layer Main Controller + 2-Layer FPC Adapter Interface |

### Default Pinout (`[Update to match your schematic]`)

| RP2350B Pin | Signal Name | Target Battery Line | Description |
| :--- | :--- | :--- | :--- |
| `GPIO [XX]` | `SWI_DATA` | `BAT_SWI / HDQ` | Single-wire battery communication data line |
| `GPIO [XX]` | `I2C_SDA` | `BAT_SDA` | I2C Fuel Gauge Data (`1.8V` level-shifted) |
| `GPIO [XX]` | `I2C_SCL` | `BAT_SCL` | I2C Fuel Gauge Clock (`1.8V` level-shifted) |
| `GPIO [XX]` | `BSI_CTRL` | `BAT_BSI / NTC` | Battery Status / Thermal ID emulation |
| `GPIO [XX]` | `STATUS_LED` | Onboard LED | Handshake & protocol activity indicator |

---

## Repository Structure

```text
CHIPLER-Battery-Emulator/
├── firmware/           # RP2350B C/C++ source code & PIO state machine programs
│   ├── src/            # Protocol handlers (SWI, I2C, fuel-gauge register maps)
│   ├── include/        # Header files and device battery profile definitions
│   └── CMakeLists.txt  # Pico SDK / PlatformIO build configuration
├── hardware/           # PCB schematics, layout files, BOM, and Gerber exports
│   ├── mainboard-4L/   # 4-layer RP2350B controller PCB files
│   └── aux-adapter-2L/ # 2-layer FPC connector adapter board files
├── enclosure/          # 3D printable STL/STEP files for protective housing
├── docs/               # Wiring diagrams, oscilloscope captures, and protocol notes
└── README.md
```

---

## Getting Started

### Prerequisites

- **Raspberry Pi Pico SDK** (`v2.0.0` or newer with RP2350 support) or **PlatformIO**
- **ARM GCC Toolchain** (`arm-none-eabi-gcc`) & **CMake** (`>= 3.13`)
- USB-C data cable for flashing the RP2350B

### Building & Flashing the Firmware

1. **Clone the repository:**
   ```bash
   git clone [https://github.com/Hamedlasheen/CHIPLER-Battery-Emulator.git](https://github.com/Hamedlasheen/CHIPLER-Battery-Emulator.git)
   cd CHIPLER-Battery-Emulator/firmware
   ```

2. **Configure and compile:**
   ```bash
   mkdir build && cd build
   cmake -DPICO_BOARD=pico2 ..
   make -j4
   ```

3. **Flash to the board:**
   - Hold the **BOOTSEL** button on the CHIPLER mainboard while connecting it via USB-C to your computer.
   - Drag and drop the generated `chipler_battery_emulator.uf2` file onto the `RP2350` mass storage drive.

---

## Bench Usage & Safety Notice

> **WARNING:** Always verify your DC bench power supply voltage before connecting a smartphone logic board. Exceeding **4.4V** on `VBAT+` can permanently damage the PMIC and charging ICs.

1. Set your DC bench power supply to **`4.0V`** with a current limit of **`3.0A`** (or **`1.0A`** during initial short-circuit checks).
2. Connect the bench power supply leads (`+` and `-`) to the emulator's power input terminals.
3. Attach the matching FPC battery flex cable to the target smartphone motherboard.
4. Power on the bench supply and trigger the device's power button; the status LED will indicate active battery handshake communication.

---

## Roadmap

- [x] Core RP2350B hardware architecture & 4-layer PCB layout
- [x] SWI & I2C fuel-gauge handshake emulation
- [ ] On-the-fly profile switching via physical button / OLED display
- [ ] Expanded multi-brand Android I2C fuel-gauge register profiles
- [ ] Real-time current & telemetry logging over USB CDC serial

---

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

---

**Developed by [Hamed Lasheen](https://github.com/Hamedlasheen) — Chipler Hardware & Embedded Diagnostics**
