# esp_lcd_touch_axs5106l

[![Component Registry](https://components.espressif.com/components/mydazy/esp_lcd_touch_axs5106l/badge.svg)](https://components.espressif.com/components/mydazy/esp_lcd_touch_axs5106l)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

ESP-IDF C++ driver for the **AXS5106L** capacitive touch controller (ChipSourceTek), with LVGL input integration, gesture recognition, RF-noise tolerance, and runtime firmware upgrade.

## Features

- ✅ **LVGL input device** — registers as `lv_indev_t` via `esp_lvgl_port`
- ✅ **Gesture recognition** — single click / double click / long press / four-direction swipe
- ✅ **Noise-tolerant** — INT debouncing, press-confirmation window, release-glitch filter, speed threshold and consecutive-failure suppression for use under strong wireless RF interference
- ✅ **Two-phase init** — supports panels where the touch IC shares its reset line with the LCD
- ✅ **Runtime firmware upgrade** — verifies running chip firmware on boot and reflashes from an embedded image on mismatch
- ✅ **AXS15231B-compatible register layout** — most existing reference code applies
- ✅ **C++17, RAII** — no global state, no hidden allocations

## Hardware

| | |
|---|---|
| Part | AXS5106L (ChipSourceTek) |
| Family | Register-compatible with AXS15231B |
| Bus | I2C, up to 400 kHz |
| Address | `0x63` (7-bit) |
| Interrupt | Active-low |
| Reset | Active-low; may be shared with LCD reset |

## Installation

```yaml
dependencies:
  mydazy/esp_lcd_touch_axs5106l: "^1.0.0"
```

Or via CLI:

```bash
idf.py add-dependency "mydazy/esp_lcd_touch_axs5106l^1.0.0"
```

## Quick Start

The driver uses **two-phase initialization** because the touch IC's reset line is often shared with the LCD reset — the firmware-upgrade flow must run before LVGL starts, while the input device must be registered after LVGL is up.

```cpp
#include "axs5106l_touch.h"

i2c_master_bus_handle_t i2c_bus;   // your initialized I2C bus
Axs5106lTouch* touch = nullptr;

// ── Phase 1: before LVGL — GPIO, reset pulse, firmware upgrade, chip-ID verify
touch = new Axs5106lTouch(
    i2c_bus,
    rst_gpio, int_gpio,
    width, height,
    /*swap_xy=*/ false,
    /*mirror_x=*/ false,
    /*mirror_y=*/ false);

if (!touch->InitializeHardware()) {
    delete touch; touch = nullptr;
}

// ── (initialize your LCD panel + start LVGL here) ──

// ── Phase 2: after LVGL — register lv_indev_t, install gesture callbacks
if (touch && !touch->InitializeInput()) {
    delete touch; touch = nullptr;
}

touch->SetGestureCallback([](TouchGesture g, int16_t x, int16_t y) {
    switch (g) {
        case TouchGesture::SingleClick:  /* ... */ break;
        case TouchGesture::DoubleClick:  /* ... */ break;
        case TouchGesture::LongPress:    /* ... */ break;
        case TouchGesture::SwipeLeft:
        case TouchGesture::SwipeRight:
        case TouchGesture::SwipeUp:
        case TouchGesture::SwipeDown:    /* ... */ break;
        default: break;
    }
});
```

## API Reference

| Method | Purpose |
|---|---|
| `Axs5106lTouch(bus, rst, int, w, h, swap, mx, my)` | Construct (no I/O) |
| `InitializeHardware()` | Phase 1: GPIO + reset pulse + optional firmware upgrade + chip-ID verify |
| `InitializeInput()` | Phase 2: register `lv_indev_t` via `esp_lvgl_port` |
| `SetGestureCallback(cb)` | Receive single/double click, long press, 4-direction swipe |
| `SetWakeCallback(cb)` | Called on first touch press (useful for power-save wakeup) |
| `Sleep()` / `Resume()` | Toggle low-power mode |
| `GetLvglDevice()` | Returns the underlying `lv_indev_t*` for advanced integration |

See [`include/axs5106l_touch.h`](include/axs5106l_touch.h) for full Doxygen-style docs.

## Coordinate Transform

`swap_xy` / `mirror_x` / `mirror_y` are applied in software at construction time.

> **Note**: some AXS5106L panels ship with built-in firmware-level coordinate rotation. In that case all three flags must be `false` and the panel reports native landscape coordinates. Verify with the calibration overlay (see Debug) before enabling any transform.

## Firmware Upgrade

`InitializeHardware()` reads the running firmware version (register `0x05`) and compares it against the version embedded in `include/axs5106l_firmware.h`. On mismatch it enters debug mode (`0xAA → 0x90 → 0xA0`) and reflashes the MTP region.

### About the embedded firmware

The shipped `axs5106l_firmware.h` byte array is the firmware image developed for **mydazy custom panel solutions** and is redistributed under Apache-2.0 as part of this component. It is provided **as a reference image** that works on AXS5106L silicon programmed through the standard debug-mode upgrade path; it makes no assumption about a specific panel vendor.

If your panel ships a different firmware image, replace `include/axs5106l_firmware.h` with the byte array supplied by your panel manufacturer and rebuild — the upgrade flow itself is panel-agnostic.

## Noise Filter Tuning

Internal constants (`INT_DEBOUNCE_US`, `PRESS_CONFIRM_US`, `MAX_SPEED_THRESHOLD`, etc.) are tuned for embedded environments with strong RF noise. They live in `axs5106l_touch.cc` — adjust there if your application needs different sensitivity.

## Debug

Define `AXS5106L_TOUCH_DEBUG_OVERLAY=1` at compile time to enable:

- a 12×12 px tracking dot rendered on `LV_LAYER_TOP` for every touch
- periodic `[calib] raw X=[..] Y=[..]` log lines (every 20 touches) to observe the chip's actual coordinate range

Disable in production builds.

## Compatibility

| Target | I2C readout | Gestures | LVGL indev |
|---|:---:|:---:|:---:|
| ESP32 | ✅ | ✅ | ✅ |
| ESP32-S2 | ✅ | ✅ | ✅ |
| ESP32-S3 | ✅ | ✅ | ✅ |
| ESP32-C3 | ✅ | ✅ | ✅ |
| ESP32-C6 / H2 / P4 | ✅ | ✅ | ✅ |

Tested on ESP-IDF **5.3 / 5.4 / 5.5** with ESP32-S3.

## License

Apache-2.0 — see [LICENSE](LICENSE).
