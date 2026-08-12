# Open-Source Closed-Loop Insulin Pump — DIY Project (Theoretical / Educational Prototype)

> 📖 中文说明：[README.md](README.md)  ·  English documentation (this file)

<p align="center">
  <img src="simulator/lvgl_sdl/preview/01_home.png" width="320" alt="Home screen preview">
</p>

> ## 🚫🚫🚫 CRITICAL WARNING / 最高优先级警告
>
> **This is a THEORETICAL / EDUCATIONAL prototype. It is NOT a medical device and MUST NOT be used on any human body — including yourself.**
>
> **本项目是「理论验证 + 教学原型」性质的开源 DIY 项目，绝对、严禁用于任何人体（包括你自己）。**
>
> It has **NOT** been certified by any regulatory authority (China NMPA, US FDA, EU CE, etc.), has **NOT** undergone clinical validation, has **NO** reliable accuracy calibration, **NO** fail-safe redundancy, and **NO** biologically compatible delivery path. Using it to inject insulin into a human body may cause hypoglycemic coma, ketoacidosis, severe injury, or death. All consequences are borne by the user; the authors and contributors assume no medical, legal, or financial liability.

---

## ⚠️ Safety & Disclaimer (read first)

1. **Not a medical device** — this project is an open-source DIY project for education / R&D / animal experimentation only. It is not a medical device in any form.
2. **No human use** — do not use it for human insulin injection, infusion, or any scenario where it enters or contacts the human body.
3. **Accuracy is not clinically validated** — the 0.05 U (0.5 µL) target precision exists only at the theoretical / desktop-simulation level; it has not been metrologically calibrated, temperature-drift compensated, or long-term aged.
4. **No fail-safe redundancy** — under fault conditions (stall, lost steps, battery drop, software deadlock) there is no independent hardware interlock (e.g. mechanical check valve, independent watchdog delivery cutoff).
5. **Legal & liability** — the authors and all contributors explicitly disclaim all medical, legal, and financial liability arising from use, modification, or distribution of this project. Downloading, cloning, or forking this repository means you understand and agree to the above.
6. **License disclaimer** — the code is under the MIT License and the hardware under CERN-OHL-S; both **exclude any implied warranty of merchantability or fitness for a particular purpose** and disclaim damages arising from use (see `LICENSE` and `LICENSE-HARDWARE`).

> If you need a working closed-loop insulin therapy, use a regulator-approved commercial device (Medtronic, Tandem, Insulet, etc.) together with a legally compliant loop system such as AndroidAPS / Loop running on compliant hardware.

---

## 1. Project Positioning & Goals

This project attempts to replicate a closed-loop insulin pump prototype that talks to the **AndroidAPS (AAPS)** loop algorithm, built from an **ESP32-C6 + low-cost linear stepper actuator**, targeting a BOM under ¥1000 (≈ US$140) with fully open-source hardware and software.

| Item | Specification |
|------|---------------|
| MCU | ESP32-C6 1.47" LCD dev board (WiFi 6 + BLE 5, onboard antenna) |
| Stepper | SM2012 linear actuator (12 V nominal, 11.1 V direct-supply compatible, integrated leadscrew) |
| Driver | DRV8825 (up to 1/32 microstep; M0/M1/M2/nSLEEP hard-wired) |
| Battery | 3S Li-ion (11.1 V nominal, 18650 ×3, ≈3000 mAh) |
| Charging | Type-C + 3S dedicated BMS (IP3002 / HY2213) |
| Regulation | DC-DC 11.1 V→5 V (system) + AMS1117-3.3 (DRV8825 VDD) |
| Monitoring | INA226 current/voltage sense (lost-step / stall / origin supervision) |
| Dosing precision | **0.05 U (0.5 µL U-100 insulin)** (theoretical target, uncalibrated) |
| Comms | BLE 5.0 GATT: AAPS **Dana-i impersonation** (Plan B, GATT `FFF0/FFF1/FFF2` + custom Dana CRC + two-level encryption); Wi-Fi (OTA / debug) |
| Display | 1.47" LCD (ST7789, onboard, 172×320 landscape, logical 320×172) |
| Control | Phone app (AAPS plugin + custom app) + 4-button keypad |
| Reservoir | Standard 3 mL syringe-type reservoir (Dana PH300 / CY-13 compatible, 300 U) |

> **Hardware platform (locked):** the MCU is uniformly the **ESP32-C6 1.47" LCD dev board** (RISC-V core, WiFi 6 + BLE 5, onboard ST7789 screen); battery 3S 11.1 V; DC-DC step-down 11.1 V→5 V; includes INA226 current/voltage monitoring and a 4-button keypad; motor driven directly from 11.1 V. DRV8825 is retained. (An earlier Rev.1 ESP32-C3 evaluation was fully abandoned in favor of ESP32-C6.)
>
> ⚠️ **Hardware is still at the "design / prototype" stage, not a wearable delivery terminal.** PCB, enclosure, and BOM are theoretical designs — assess the risk yourself per `docs/`.

---

## 2. Software Architecture (Simulator + Firmware + UI Abstraction)

To decouple "UI logic" from "real hardware" and enable fast verification without hardware, the project uses a **UI-HAL abstraction layer**:

```
┌──────────────────────────────────────────────────────────────┐
│  ui_screen.cpp  (LVGL 9.5.0 UI + state-machine display + nav) │  ← shared by both targets
│      │  calls ui_hal_* to read data / trigger actions          │
│      ▼                                                         │
│  ui_hal.h  (unified hardware-abstraction interface)            │
│      ├── ui_hal_sim.cpp   (PC simulator backend: mock data)    │
│      └── ui_hal_fw.cpp    (ESP32 firmware backend: real mods)  │
└──────────────────────────────────────────────────────────────┘
        │                                          │
   simulator side                              firmware side
   SDL2 window render                       ST7789 SPI render
   mock_hal (stub)                          motor/ina226/ble/safety…
```

- **`ui_screen.cpp`**: a single copy of the UI, running on both simulator and firmware, so the UI never drifts away from the firmware again.
- **`ui_hal.h`**: defines data reads (BG / trend / loop mode / TBR / daily total / clock / basal) and actions (bolus / prime / switch mode / clear alarm / backlight / key tone).
- **`basal_scheduler`**: a 3-minute periodic scheduler that computes the current basal rate from the local profile / loop command / TBR / pause, enqueues motor microsteps, and maintains the daily total and reservoir remaining.
- **Chinese fonts**: a self-built generator (`gen_cn_font.py`, fontTools subsetting + Pillow rendering) outputs LVGL `fmt_txt` packed at **bpp=4**, 442 glyphs, 16px ≈ 308 KB / 12px ≈ 198 KB, fitting ESP32 4 MB Flash.

### FreeRTOS task layout (firmware)
`safety_task` (highest) · `motor_task` · `ble_task` · `battery_task` · `keypad_task` · `display_task` · `basal_scheduler_task` · other auxiliary tasks.

### AAPS closed-loop integration (Dana-i impersonation, Plan B)
The firmware uses the `aaps_dana.{h,cpp}` module to **impersonate a Dana-i insulin pump**, so an installed AndroidAPS connects to this device directly and pushes basal / bolus / extended bolus / TBR / time-sync / CGM — with **no custom app or driver patch** required.

- **Protocol authority** = the AndroidAPS `pump/danars` module (`BleEncryption.kt` + `DanaRSPacket*`), implemented after line-by-line verification.
- **Transport spec**: GATT `FFF0` (write) / `FFF1` (notify) / `FFF2` (write-no-response) + CCCD `2902`; envelope `A5 A5 len TYPE OPCODE params CRC16 5A 5A`; single-byte opcode scheme (`0x4A` step bolus / `0xC1` APS-TBR / `0x47` extended bolus / `0x62` stop TBR / `0x48` CGM / `0x02` status …); device name must be exactly 10 chars, matching `^[a-zA-Z]{3}[0-9]{5}[a-zA-Z]{2}$`.
- **Security**: level-1 envelope XOR obfuscation (incl. CRC_L) + level-2 BLE_5 deterministic byte obfuscation; handshake (PUMP_CHECK / TIME_INFORMATION) skips level-2; level-2 engages only after handshake completes.
- **Byte-level verification**: `test/aaps_dana_test.cpp` (g++ pure logic) + `test/oracle_aaps.py` (AAPS-source-translated oracle) → `test/run_tests.sh` extracts `^PKT ` lines, sorts, and diffs — **205 scenarios match byte-for-byte** (handshake / command response / notify / 200 random command packets).
- **Treatment logging (verified on real hardware, 2026-08-12)**: AAPS builds its treatment records (bolus / TBR / extended) from the pump's **history-event replay** (`0xC2` APS_HISTORY_EVENTS). The firmware now records bolus / TEMP_START / TEMP_STOP / EXT_START / EXT_STOP into a ring buffer and replays them on `0xC2` (with a trailing `0xFF`). On real hardware, a manually delivered 0.5 U bolus was confirmed written into the AAPS Treatments page (`**NEW** EVENT BOLUS ... 0.5U`). TBR / extended hooks are implemented and unit-tested; full on-device TBR validation is pending.

---

## 3. UI Screens (simulator preview)

The UI is a **white background + medical blue (#006bb7)** Maishitong-style, **Chinese-language**, landscape 320×172. All screenshots below come from the PC simulator (SDL2 + LVGL 9.5.0) and share the same `ui_screen.cpp` with the firmware:

| Screen | Screenshot | Description |
|--------|-----------|-------------|
| Home | ![home](simulator/lvgl_sdl/preview/01_home.png) | BG / trend / loop status / daily total / clock |
| Menu | ![menu](simulator/lvgl_sdl/preview/02_menu.png) | Basal / Bolus / Prime / Alarm / Loop / Settings |
| Basal | ![basal](simulator/lvgl_sdl/preview/03_basal.png) | Local profile / AAPS takeover toggle, 24 slots |
| Bolus menu | ![bolus_menu](simulator/lvgl_sdl/preview/04_bolus_menu.png) | Normal / Extended / Dual / Wizard / Meals |
| Normal bolus | ![bolus](simulator/lvgl_sdl/preview/05_bolus_normal.png) | 0.05 U step dose input |
| Bolus wizard | ![wizard](simulator/lvgl_sdl/preview/06_bolus_wizard.png) | Carbs / current BG / IOB suggestion |
| Alarm | ![alarm](simulator/lvgl_sdl/preview/07_alarm.png) | Stall / low battery / empty reservoir / over-limit highlight |
| Loop | ![loop](simulator/lvgl_sdl/preview/08_loop.png) | AAPS takeover / open-loop local / paused |
| Settings | ![settings](simulator/lvgl_sdl/preview/09_settings.png) | Backlight / key tone / about |

> Note: the simulator uses `mock_hal` stub functions to produce demo data and **does not represent real drug delivery**.

---

## 3.1 AAPS Loop Demo (desktop, no hardware needed)

The **real firmware command core** (`aaps_dana` + dose conversion + scheduler) is compiled into the PC simulator, driven by a scripted 17-step session engine over `g_pump_state`, so the pump screen repaints in strict sync with AAPS commands every frame. A Python control panel provides a four-pane synchronized visualization, handy for demonstrating the full loop "AAPS commands → firmware receives → motor pushes insulin → pump screen responds" without hardware.

### Four synchronized panes (control panel `test/link_demo_gui.py`)
| Pane | Content | Data source (real, not faked) |
|------|---------|-------------------------------|
| ① Stepper delivery | canvas draws the stepper (rotor turns with microsteps) + leadscrew + reservoir plunger advancing as dose delivered, labeled "delivered X.XX U / microsteps N / plunger travel mm" | firmware `motor_delivered_units_x100()` → `dosing.h` single source of truth |
| ② AAPS send window | raw in-flight bytes AAPS sends over BLE + opcode name + human intent (e.g. "bolus 2.00 U") | `dana_build_packet` real packing |
| ③ Firmware receive window | firmware unpack (CRC / type) + actual dispatch (e.g. `motor_enqueue`) + reply bytes; tampered-CRC commands show red "REJECTED" | `aaps_dana_feed_rx_test` real unpack/dispatch |
| ④ Pump screen UI | canvas replica of the 320×172 landscape status screen, live-updating (or just watch the real SDL window) | `g_pump_state` repaint per frame |

> Panes ②③ use **real Dana-i protocol** bytes — `link_session` really goes through `dana_build_packet` packing + firmware `aaps_dana_feed_rx_test` unpack/dispatch; the protocol trace buffer captures every send/receive and feeds both windows.

### One-click start (recommended)
```bash
# macOS: double-click to auto-build the demo, pop the pump-screen window, and launch the panel
open test/run_link_demo.command
# or from terminal
bash test/run_link_demo.sh
```
Press ▶ in the panel to watch the four panes sync. The script auto-ensures the "link-mode" binary is built (via the `.built_link_mode` marker) and cleans up processes on close.

### Manual build (optional)
```bash
bash test/build_sim_link.sh     # build link-mode (SIM_LINK_MODE=ON), binary at /Users/feelingme/pump_sim_build/simulator
bash test/build_sim_mock.sh     # switch back to normal mock simulator
SIM_HEADLESS=1 /Users/feelingme/pump_sim_build/simulator &   # headless (sandbox / no display)
python3 test/link_demo_gui.py                                # connect the panel
```
- Verification: 17/17 steps, 50 checks 0 failures; 18 protocol traces (incl. 1 tamper-rejected); final `motor_units=2.00 / microsteps=4356` (2 U × 2178).
- Pure-logic smoke test also at `test/link_mode_smoke.cpp` (no LVGL/SDL dependency).

---

## 4. Directory Structure

```
closed-loop-insulin-pump/
├── README.md                 ← Chinese overview + safety statement
├── README_en.md              ← This file (English overview)
├── CHANGELOG.md              ← Change log
├── LICENSE                   ← Software license (MIT + disclaimer)
├── LICENSE-HARDWARE          ← Hardware license (CERN-OHL-S + disclaimer)
├── NOTICE                    ← Safety / disclaimer highlights
├── docs/                     ← Technical docs (14 files: system/power/motor/PCB/firmware/AAPS/Android/mechanical/safety/testing/BOM/12-AAPS-Dana protocol/12-wiring + UI design)
├── code/
│   ├── esp32_firmware/       ← ESP32-C6 firmware (Arduino framework, Rev.2)
│   │   ├── esp32_firmware.ino     ← entry (setup/loop + FreeRTOS tasks)
│   │   ├── src/                   ← all modules + lv_conf.h (LVGL) / config.h (pins & constants)
│   │   │   ├── ui_screen.{h,cpp}  ← shared UI (white bg, Chinese, 320×172)
│   │   │   ├── ui_hal.h           ← UI hardware-abstraction interface
│   │   │   ├── ui_hal_fw.cpp      ← firmware backend (real modules)
│   │   │   ├── aaps_dana.{h,cpp}  ← AAPS Dana-i impersonation BLE module (Plan B)
│   │   │   ├── iob_model.{h,cpp}  ← IOB model (Walsh triangular decay)
│   │   │   ├── rtc_clock.{h,cpp}  ← ESP32 hardware RTC (no 49-day wraparound)
│   │   │   ├── basal_scheduler.{h,cpp} ← basal periodic scheduler
│   │   │   ├── lv_font_cn_16.cpp / lv_font_cn_12.cpp ← bpp=4 Chinese fonts
│   │   │   ├── pump_types.h / pump_state.{h,cpp} ← state machine / CRC
│   │   │   ├── motor_controller.* / ina226.* / lcd_display.*
│   │   │   ├── keypad.* / battery_monitor.* / safety_monitor.*
│   │   │   ├── ble_comm.* / storage.* / history_log.*
│   │   │   └── power_manager.*
│   │   └── test/                 ← host unit tests (aaps_dana byte-level / link smoke)
│   └── android_app/          ← Android app (Kotlin + Compose)
├── simulator/
│   └── lvgl_sdl/             ← PC simulator (SDL2 + LVGL 9.5.0)
│       ├── README.md
│       ├── CMakeLists.txt    ← with SIM_LINK_MODE option (loop demo)
│       ├── src/              ← ui_screen/ui_hal/mock_hal/pump_state…
│       │                     ←   + link_session/link_ipc/ui_hal_link (demo engine)
│       └── preview/          ← 11 UI screenshots (see above)
├── test/                     ← AAPS loop demo (link_demo_gui.py / run_link_demo.* / build_sim_*.sh / aaps_link_sim.cpp / host/)
├── pcb/                      ← PCB schematic / layout / Gerber
├── mechanical/               ← Mechanical CAD / 3D print / drawings
├── diagrams/                 ← System block diagrams
└── resources/                ← Datasheets, reference images
```

---

## 5. Hardware Configuration (Rev.2, design stage)

| Module | Key part | Notes |
|--------|----------|-------|
| MCU board | Waveshare ESP32-C6-LCD-1.47 | ST7789 172×320 onboard LCD, GPIO8 = WS2812 |
| Motor | SM2012 linear actuator | 200 steps/rev + 1/32 microstep = 6400 microsteps/rev, leadscrew pitch 0.5 mm (to be measured) |
| Driver | DRV8825 | VREF target 400 mV |
| Power | 3S 11.1 V (18650 ×3) + DC-DC 5 V | system supply; AMS1117-3.3 for DRV8825 logic |
| Monitoring | INA226 | current/voltage, stall / lost-step / origin supervision |
| Interaction | 4-button keypad | short press navigate; long-press SET = origin, long-press ESC = power off |

> Detailed BOM, schematics, and mechanical design are in [`docs/11-bom.md`](docs/11-bom.md), [`docs/04-pcb-schematic.md`](docs/04-pcb-schematic.md), [`docs/08-mechanical-design.md`](docs/08-mechanical-design.md). **All hardware data is theoretical and unverified on real units.**

### 5.1 Wiring quick reference

> 🚫 For theoretical-validation use only, **NO human use**; wire only with the battery disconnected, no medication, pure-circuit environment.

**Core principle**: the ESP32-C6 dev board (with screen) is the mainboard; all peripherals hang on its GPIO; all modules must share ground; 11.1 V goes ONLY to motor / INA226 / DC-DC, **never to GPIO**.

| Peripheral | ESP32 GPIO | Key note |
|------------|-----------|----------|
| DRV8825 driver | STEP=**9** / DIR=**10** / ENABLE=**11** (active-low) / nFAULT→**16** | VMOT→11.1 V, VDD→3.3 V; M0/M1/M2 hard-wired H/H/L = 1/32 microstep; nSLEEP/nRESET pulled high |
| SM2012 motor | DRV8825 AOUT1/AOUT2, BOUT1/BOUT2 | 4 wires in A/B phases; wrong phase only jitters, swap one pair |
| INA226 monitor | SDA=**18** / SCL=**19** (**must be explicit, NOT default 21/22**) | VCC→**3.3 V** (never 5 V, or the C6 burns); VBUS→11.1 V bus; IN± across 20 mΩ shunt; A0/A1 grounded = 0x40 |
| 4-button keypad | Up=**20** / Down=**23** / SET=**4** / ESC=**5** | one end GPIO, other GND (internal pull-up, active-low) |
| Limit switches ×2 | Forward=**2** / Back=**3** | one end GPIO, other GND |
| Buzzer | Signal=**0** (PWM) | other end GND |
| WS2812 status LED | onboard GPIO8 | integrated, no wiring |
| DC-DC enable (opt) | **17** | leave floating if module has no EN |

- LCD, USB, and WS2812 are **onboard** — no wiring needed; GPIO6/7/14/15/21/22 are occupied by the LCD, do not reuse.
- Full wiring table, power-tree diagram, phase determination, and checklist: 👉 [**`docs/12-wiring.md`**](docs/12-wiring.md).

---

## 6. Quick Start

### 6.1 Recommended reading order
1. [`docs/01-system-architecture.md`](docs/01-system-architecture.md) — whole system
2. [`docs/02-power-system.md`](docs/02-power-system.md) — power design
3. [`docs/03-motor-drive.md`](docs/03-motor-drive.md) — **0.05 U precision calculation**
4. [`docs/05-firmware-design.md`](docs/05-firmware-design.md) — software architecture (ui_hal / scheduler / BLE protocol)
5. [`docs/06-aaps-integration.md`](docs/06-aaps-integration.md) — AAPS integration
6. [`docs/09-safety-design.md`](docs/09-safety-design.md) — safety design (**must read**)
7. [`docs/12-wiring.md`](docs/12-wiring.md) — hardware wiring (**read before touching anything**)
8. [`docs/11-bom.md`](docs/11-bom.md) — bill of materials

### 6.2 PC simulator (no hardware, fastest UI check)
```bash
# macOS paths cannot contain Chinese characters; build via a pure-ASCII symlink
ln -s "/Users/yourname/Desktop/闭环胰岛素泵项目/simulator/lvgl_sdl" ~/pump_sim
ln -s ~/pump_sim ~/pump_sim_build   # or choose your own out-of-source build dir
cd ~/pump_sim_build
cmake -G Ninja -B build ~/pump_sim
cmake --build build -j
./build/simulator            # pops SDL2 window; ↑↓ navigate / SET confirm / ESC back
```
Dependencies: SDL2, CMake, Ninja, Python (fontTools + Pillow, for `gen_cn_font.py` font regeneration). See [`simulator/lvgl_sdl/README.md`](simulator/lvgl_sdl/README.md).

### 6.3 Firmware build & flash (recommended: prebuilt bin)

**Easiest**: flash a prebuilt image from the browser — **no Arduino / compilation needed locally**.
Prebuilt images are under `code/esp32_firmware/build_out/release/`:

- `pump_default_factory.bin` (default variant)
- `pump_aaps_factory.bin` (AAPS Dana-i impersonation variant)

Open <https://esp.huhn.me> in a browser → select port → select bin (offset `0x0`) → check "Erase All" → Flash.
See [`docs/13-烧录指南.md`](docs/13-烧录指南.md) (includes esptool / Espressif Flash Download Tool / rebuild-from-source paths and troubleshooting).

> **Safety red line**: this project is an educational / theoretical prototype, **NOT a medical device**, and must not be used on any human body; on real hardware you may only verify mechanical motion with an **empty syringe + water**.

> ⚠️ **ST7789 gotchas** (already in `config.h` / `lcd_display.cpp`): 172-wide screen needs column offset `LCD_X_GAP=34`; color format `RGB565_SWAPPED`; GPIO8 is WS2812 and needs `rgbLedWriteOrdered()`; INA226 I2C must use `Wire.begin(18,19)` (21/22 are taken by the LCD).

### 6.4 Android app
Kotlin + Jetpack Compose, 44 source files, 5 Compose screens. BLE protocol aligned with the firmware binary + CRC-8/CCITT; base UUID `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`. See [`docs/07-android-app.md`](docs/07-android-app.md) and [`code/android_app/README.md`](code/android_app/README.md).

### 6.5 AAPS loop demo (desktop, no hardware)
See §3.1 above. One line: `open test/run_link_demo.command` (macOS double-click) auto-builds the link-mode simulator, pops the pump-screen window, and launches the four-pane panel; press ▶ to watch "① motor delivery ② AAPS send ③ firmware receive ④ pump screen UI" live. For teaching/demo only — **no human use**.

---

## 7. Roadmap

| Phase | Content | Status |
|-------|---------|--------|
| Phase 0 | Design & documentation | ✅ Done |
| Phase 1 | Firmware / simulator code (ui_hal abstraction, basal scheduler, BLE CRC protocol, AAPS Dana-i impersonation) | ✅ Done (theoretical code) |
| Phase 2 | Component procurement + breadboard prototype | ⏳ Not started |
| Phase 3 | Motor single-step precision validation (0.05 U, needs metrology) | ⏳ Not started |
| Phase 4 | ESP32-C6 + DRV8825 integration test | ⏳ Not started |
| Phase 5 | BLE comms + Android app integration | ⏳ Not started |
| Phase 6 | PCB design + 3D-printed enclosure | ⏳ Not started |
| Phase 7 | AAPS integration (Dana-i impersonation + desktop loop demo) | ✅ Byte-level 205-scenario match + four-pane sync demo + **real-device firmware flash & AAPS treatment-logging verification (manual 0.5 U bolus confirmed written to AAPS Treatments, 2026-08-12)** ; on-device TBR validation pending |
| Phase 8 | Safety validation + long-term stability | ⏳ Not started (**most critical; do NOT use on humans until done**) |

---

## 8. Key Reference Projects

| Project | Author | Feature |
|---------|--------|---------|
| Ultra-low-cost Insulin Pump | Lublin et al. | $89 BOM, AAA battery (PMC9679028) |
| Arduino Insulin Pump | charan-271 | ESP32 + OLED + stepper |
| Insulin Pump Prototype | AndreOliveira | ESP32 + web dashboard + state machine |
| InsulinManager | Kimpalele | ESP32-C3 + HM10 BLE + iOS app |
| Ultra-low-cost pump | China research team | ESP32 + DRV8833 + rat experiments |
| OpenAPS / AndroidAPS | community | closed-loop algorithm reference implementations |

---

## 9. License

- **Software (code)**: MIT License — see [`LICENSE`](LICENSE).
- **Hardware (PCB / mechanical / schematic)**: CERN Open Hardware Licence v2 — Strongly Reciprocal (CERN-OHL-S) — see [`LICENSE-HARDWARE`](LICENSE-HARDWARE).
- Both licenses **explicitly exclude any implied warranty of merchantability or fitness for a particular purpose, and disclaim damages arising from use**.
- Safety & disclaimer highlights also in [`NOTICE`](NOTICE).

> **Reiterated**: open-source ≠ usable as a medical device. Strictly follow the safety statements at the top of this document and in `NOTICE`.

---

<p align="center">
  <b>🚫 This project is for learning, R&D, and animal experimentation ONLY. NO human use. 🚫</b>
</p>
