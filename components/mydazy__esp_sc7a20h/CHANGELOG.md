# Changelog

All notable changes to **esp_sc7a20h** will be documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.1.0] - 2026-05-05

### Fixed

- `sc7a20h_config_deep_sleep_wakeup()` now reads `INT1_SRC` (0x31) before
  arming `ESP_EXT1_WAKEUP_ANY_LOW`. A residual latched motion event used to
  hold INT1 LOW and fire the wakeup the instant `esp_deep_sleep_start()` ran,
  causing devices that integrate this driver to "wake immediately after going
  to sleep". Failure to read is logged as a warning (caller may have already
  torn down I2C) but no longer aborts wakeup setup.
- Comment for `CTRL_REG6 = 0x02` corrected: `H_LACTIVE` bit makes INT1
  **active LOW**, not active high. Idle level is HIGH on the INT line.

### Added

- `sc7a20h_clear_motion_latch(handle, *src)` — public API to consume a
  pending latched motion event without going through the wakeup path.
  Useful when handling a motion ISR in active mode, or before re-arming
  deep-sleep manually.

### Notes

- Wire protocol, register layout and existing call signatures unchanged.
  Source-compatible drop-in for v2.0.x.

## [2.0.0] - 2026-04-26

### ⚠️ BREAKING CHANGES

Driver fully rewritten in **C** to align with the Espressif ecosystem (100% of `esp_lcd` / `esp_lcd_touch` / sensor drivers in the Registry are written in C). Existing C++ users must migrate.

### Changed

- `class Sc7a20h` → opaque `sc7a20h_handle_t` + free functions (`sc7a20h_create`, `sc7a20h_del`, `sc7a20h_get_acce`, ...).
- `enum class` → `typedef enum` with `SC7A20H_*` prefix (`SC7A20H_RANGE_4G`, `SC7A20H_ODR_100HZ`).
- Constructor takes a `sc7a20h_config_t` struct (use `SC7A20H_DEFAULT_CONFIG()` macro).
- Convenience helper `sc7a20h_create_with_motion_detection()` replaces the old `InitWithMotionDetection()` method.
- Public callbacks (`sc7a20h_wakeup_cb_t`) now use C function pointers + `void *user_ctx` instead of `std::function`.

### Migration

```c
// before (v1.0.x, C++)
Sc7a20h* sensor = new Sc7a20h(i2c_bus, 0x19);
sensor->InitWithMotionDetection();
// ...
delete sensor;

// after (v2.0.0, C)
sc7a20h_handle_t sensor = NULL;
sc7a20h_config_t cfg = SC7A20H_DEFAULT_CONFIG();
cfg.i2c_addr = 0x19;
sc7a20h_create_with_motion_detection(i2c_bus, &cfg, NULL, &sensor);
// ...
sc7a20h_del(sensor);
```

### Notes

- Wire protocol, register addresses and motion-detection algorithm unchanged. Behavior is bit-for-bit identical to v1.0.1.
- Example `examples/basic_motion_wakeup/` rewritten in C.

## [1.0.1] - 2026-04-26

### Changed

- All in-source comments and log strings translated from Chinese to English for global readability.
- Header documentation rewritten in English Doxygen style.

### Added

- `examples/basic_motion_wakeup/` — minimal pickup-to-wake project demonstrating I2C init, motion detection, deep-sleep wake-up.
- `.clang-format` — coding style based on Google C++ with ESP-IDF conventions (4-space indent, 100-column).
- GitHub Actions multi-IDF-version build matrix (5.3 / 5.4 / 5.5) for the example.

### Notes

- No API or wire-format changes. Source-compatible drop-in for v1.0.0.

## [1.0.0] - 2026-04-26

### Added

- Initial public release.
- I2C driver using ESP-IDF v5.x `i2c_master` API.
- 12-bit raw + scaled `mg` acceleration readout.
- Configurable range (±2 / 4 / 8 / 16 g) and ODR (1 Hz – 400 Hz).
- Motion-detect interrupt with XYZ OR-trigger, configurable threshold/duration, latched on INT1.
- One-line `ConfigDeepSleepWakeup(gpio)` — sets up EXT1 wakeup + RTC GPIO pull-up.
- One-line `InitWithMotionDetection(cfg)` — init + IRQ + 500 ms debounce.
- `esp_err_t` error propagation throughout.
- RAII destructor: powers down sensor and unregisters I2C device.

### Notes

- LIS2DH12 / LIS3DH register-compatible — most LIS2DH12 reference code applies.
- Tested on ESP-IDF 5.3 / 5.4 / 5.5 with ESP32-S3.
