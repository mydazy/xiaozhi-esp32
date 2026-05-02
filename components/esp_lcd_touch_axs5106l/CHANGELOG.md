# Changelog

All notable changes to **esp_lcd_touch_axs5106l** will be documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.1.0] - 2026-05-02

### Added

- **Two-stage RF noise filter for cellular-modem coexistence.** When AXS5106L
  shares an I2C bus / supply rail with a 4G modem (e.g. Quectel ML307R), TDD
  uplink bursts inject phantom touches into the controller's ADC. Two new
  layers reject them without sacrificing first-touch latency:
  - **Post-release guard** (`POST_RELEASE_GUARD_US = 200 ms`): a press edge
    arriving within 200 ms of the previous release is held pending for one
    additional LVGL frame. A single-frame RF spike is dropped silently. Idle-
    to-press transitions, long swipes and long-presses skip the guard — first
    tap latency is unchanged.
  - **Bus-recovery state reset:** when consecutive I2C failures trigger
    `i2c_master_bus_reset()` (SDA pulled low by RF), transient touch state
    (`int_low_since`, `last_time`, `press_pending`) is now cleared, preventing
    a phantom press from surviving the reset.

### Changed

- `CLICK_MIN_TIME_US`: 20 ms → **40 ms** — sub-40 ms presses are reclassified
  as RF spikes; physical fingertip taps remain ≥80 ms.
- `SWIPE_THRESHOLD`: 30 px → **25 px** — better short-swipe recognition on
  compact panels (1.83"/284 px).
- `CLICK_MAX_MOVE`: 35 px → **24 px** — eliminates the previous 30~35 px
  ambiguous zone where small swipes were absorbed as taps.
- `CLICK_MAX_TIME_US`: 400 ms → **500 ms** — tolerates slow-lift taps.

### Recommendations for boards that share an I2C bus with a 4G modem

- Bump `i2c_master_bus_config_t::glitch_ignore_cnt` from 7 to 15 (≈87 ns →
  ≈187 ns hardware glitch filter). Safe at 100/400 kHz, no impact on healthy
  edges.
- Decouple the touch IC's INT pin with an RC low-pass (10 kΩ + 1 nF) on
  noise-prone PCBs.

## [2.0.0] - 2026-04-26

### ⚠️ BREAKING CHANGES

Driver fully rewritten in **C** to align with the Espressif ecosystem (100% of `esp_lcd_touch` drivers in the Registry are written in C). Existing C++ users must migrate.

### Changed

- `class Axs5106lTouch` → opaque `axs5106l_touch_handle_t` + free functions.
- `enum class TouchGesture` → `typedef enum` with `AXS5106L_GESTURE_*` prefix.
- `class Axs5106lUpgrade` → opaque `axs5106l_upgrade_handle_t` + free functions in `axs5106l_upgrade.h`.
- Two-phase init renamed: `InitializeHardware()` → `axs5106l_touch_new()`; `InitializeInput()` → `axs5106l_touch_attach_lvgl()`.
- Constructor takes a `axs5106l_touch_config_t` struct (use `AXS5106L_TOUCH_DEFAULT_CONFIG()` macro).
- Callbacks (`axs5106l_wake_cb_t`, `axs5106l_gesture_cb_t`) use C function pointers + `void *user_ctx` instead of `std::function` lambdas.
- Lifecycle now reports `esp_err_t` (was `bool`).

### Migration

```c
// before (v1.0.x, C++)
auto* tp = new Axs5106lTouch(bus, RST, INT, W, H, swap, mx, my);
tp->InitializeHardware();
// ... start LVGL ...
tp->InitializeInput();
tp->SetGestureCallback([this](TouchGesture g, int16_t x, int16_t y) { ... });
delete tp;

// after (v2.0.0, C)
axs5106l_touch_handle_t tp = NULL;
axs5106l_touch_config_t cfg = AXS5106L_TOUCH_DEFAULT_CONFIG(bus, RST, INT, W, H);
cfg.swap_xy = swap; cfg.mirror_x = mx; cfg.mirror_y = my;
axs5106l_touch_new(&cfg, &tp);
// ... start LVGL ...
axs5106l_touch_attach_lvgl(tp);
axs5106l_touch_set_gesture_callback(tp, on_gesture, this);  // static fn + this ctx
axs5106l_touch_del(tp);
```

### Notes

- Wire protocol, register sequences, gesture thresholds and firmware-upgrade flow unchanged. Behavior is bit-for-bit identical to v1.0.2.
- Embedded firmware image (`axs5106l_firmware.h`) is unchanged.

## [1.0.2] - 2026-04-26

### Changed

- All in-source comments and log strings translated from Chinese to English for global readability.
- `axs5106l_firmware.h` header annotation reformatted with copyright/license attribution; build-time metadata removed.
- README expanded with explicit "About the embedded firmware" section clarifying that the shipped image is a mydazy custom-panel reference image (Apache-2.0), and panel-specific firmware should be substituted by the integrator.

### Added

- `.clang-format` — Google C++ based style with ESP-IDF conventions.
- GitHub Actions multi-IDF-version build matrix (5.3 / 5.4 / 5.5).
- `.gitignore` rules to prevent vendor-private firmware preprocessor sources (`*.i`, `*HQR*`, `*FPC-*`, `*KY[0-9]*`) from being committed by mistake.

### Notes

- No API changes. Source-compatible drop-in for v1.0.1.

## [1.0.1] - 2026-04-26

### Changed

- Documentation rewritten for public release. No API or behavior changes.

## [1.0.0] - 2026-04-26

### Added

- Initial release.
- C++ driver for **AXS5106L** capacitive touch controller (ChipSourceTek).
- LVGL input device integration via `esp_lvgl_port` (`lv_indev_t`).
- Gesture recognition: single click, double click, long press, four-direction swipe.
- Two-phase initialization (`InitializeHardware()` + `InitializeInput()`) for panels where the touch IC shares its reset line with the LCD.
- Built-in noise filtering: INT debouncing, press-confirmation window, release-glitch filter, speed thresholding and consecutive I/O-failure suppression — designed for environments with strong wireless RF interference.
- Runtime firmware-upgrade flow with embedded firmware image.
- `Sleep()` / `Resume()` low-power APIs.
- Coordinate transform (swap-XY / mirror-X / mirror-Y) at construction time.
- Optional `AXS5106L_TOUCH_DEBUG_OVERLAY` compile-time switch for visual calibration.

### Notes

- AXS5106L is register-compatible with **AXS15231B**; existing reference code applies.
- I2C address: `0x63` (7-bit).
- Tested on ESP-IDF 5.3 / 5.4 / 5.5 with ESP32-S3.
