# esp_sc7a20h

[![Component Registry](https://components.espressif.com/components/mydazy/esp_sc7a20h/badge.svg)](https://components.espressif.com/components/mydazy/esp_sc7a20h)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

ESP-IDF C++ driver for the **SC7A20H** 3-axis accelerometer (Silan Microelectronics) — a low-cost, low-power MEMS sensor with **register-level compatibility with ST LIS2DH12 / LIS3DH**.

## Features

- ✅ **Drop-in I2C driver** — uses ESP-IDF v5.x `i2c_master` API
- ✅ **Acceleration readout** — raw 12-bit + scaled `mg` units, configurable range (±2/4/8/16 g) and ODR (1 Hz – 400 Hz)
- ✅ **Motion-detect interrupt** — XYZ OR-trigger with configurable threshold/duration, latched on INT1
- ✅ **One-line deep-sleep wakeup** — `ConfigDeepSleepWakeup(gpio)` handles RTC GPIO + EXT1 setup for you
- ✅ **One-line "pickup-to-wake" init** — `InitWithMotionDetection()` ships with built-in 500 ms debounce
- ✅ **`esp_err_t` everywhere** — no silent failures, plays nicely with `ESP_ERROR_CHECK`
- ✅ **RAII cleanup** — destructor unregisters the I2C device and powers down the sensor
- ✅ **No private base class** — pure self-contained C++17, no project-specific dependencies

## Hardware

| | |
|---|---|
| Part | SC7A20H (Silan), pin/register-compatible with **LIS2DH12 / LIS3DH** |
| Bus | I2C, up to 400 kHz |
| Address | `0x18` (SA0=GND) or `0x19` (SA0=VCC, default) |
| Supply | 1.71 – 3.6 V |
| Resolution | 12-bit (high-resolution mode) |
| Range | ±2 g / ±4 g / ±8 g / ±16 g |
| ODR | 1 / 10 / 25 / 50 / 100 / 200 / 400 Hz |
| Interrupts | INT1 (motion / AOI), INT2 (click) |
| Sleep current | < 2 µA in power-down |

## Installation

### Add to your project's `idf_component.yml`

```yaml
dependencies:
  mydazy/esp_sc7a20h: "^1.0.0"
```

Then run `idf.py reconfigure` — the manager will fetch the component automatically.

### Or install via CLI

```bash
idf.py add-dependency "mydazy/esp_sc7a20h^1.0.0"
```

## Quick Start

### 1. Pickup-to-wake (recommended — one line)

Perfect for handheld / wearable devices that wake on motion:

```cpp
#include "sc7a20h.h"

i2c_master_bus_handle_t i2c_bus;  // your I2C bus
Sc7a20h sensor(i2c_bus);          // default address 0x19

if (sensor.InitWithMotionDetection() == ESP_OK) {
    sensor.ConfigDeepSleepWakeup(GPIO_NUM_3);  // INT1 wired to GPIO3
    esp_deep_sleep_start();   // wakes when device is moved
}
```

### 2. Read acceleration

```cpp
Sc7a20h sensor(i2c_bus);
sensor.Initialize(Sc7a20hRange::kRange4G, Sc7a20hOdr::kOdr100Hz);

Sc7a20hAcce acc;
if (sensor.GetAcce(acc) == ESP_OK) {
    ESP_LOGI("APP", "X=%.1fmg  Y=%.1fmg  Z=%.1fmg", acc.x, acc.y, acc.z);
}
```

### 3. Custom motion-detect thresholds

```cpp
Sc7a20hMotionConfig cfg;
cfg.threshold = 0x10;   // ~500 mg @ ±4 g
cfg.duration  = 0x05;   // 5 / ODR samples
cfg.enable_z  = false;  // ignore Z-axis (e.g. detect only horizontal motion)

sensor.SetMotionDetection(true, &cfg);
sensor.SetWakeupCallback([] {
    ESP_LOGI("APP", "Motion!");
});
```

### 4. Manual power management

```cpp
sensor.EnterPowerDown();   // < 2 µA
// ... later ...
sensor.ExitPowerDown();    // restore previous ODR
```

## API Reference

| Method | Purpose |
|---|---|
| `Sc7a20h(bus, addr=0x19)` | Construct, register I2C device |
| `Initialize(range, odr)` | Verify WHO_AM_I, configure sensor |
| `GetRawAcce(raw)` | 12-bit signed XYZ |
| `GetAcce(acce)` | XYZ in `mg` (float) |
| `SetMotionDetection(en, cfg*)` | Configure INT1 motion interrupt |
| `SetWakeupCallback(cb)` | User-space callback (run-time, not ISR) |
| `EnterPowerDown()` / `ExitPowerDown()` | Toggle low-power mode |
| `SetRange(range)` / `SetOdr(odr)` | Reconfigure at runtime |
| `ConfigDeepSleepWakeup(gpio)` | One-line EXT1 wakeup setup |
| `InitWithMotionDetection(cfg*)` | One-line init + IRQ + debounce |

See [`include/sc7a20h.h`](include/sc7a20h.h) for full Doxygen-style docs.

## Wiring

```
   SC7A20H        ESP32
  ─────────     ─────────
  VCC  ──────►  3V3
  GND  ──────►  GND
  SCL  ──────►  any GPIO (I2C SCL, with 4.7k pull-up)
  SDA  ──────►  any GPIO (I2C SDA, with 4.7k pull-up)
  INT1 ──────►  any RTC-GPIO (for deep-sleep wakeup) — optional
  SA0  ──────►  GND (addr 0x18) or VCC (addr 0x19)
```

## Compatibility

| Target | I2C readout | Motion IRQ | Deep-sleep wakeup |
|---|:---:|:---:|:---:|
| ESP32 | ✅ | ✅ | ✅ (RTC GPIO only) |
| ESP32-S2 | ✅ | ✅ | ✅ (RTC GPIO only) |
| ESP32-S3 | ✅ | ✅ | ✅ (RTC GPIO only) |
| ESP32-C3 | ✅ | ✅ | ⚠️ (no EXT1 — use GPIO wakeup) |
| ESP32-C6 / H2 / P4 | ✅ | ✅ | ⚠️ (use LP-IO wakeup) |

Tested on ESP-IDF **5.3 / 5.4 / 5.5**. Requires C++17.

## License

Apache-2.0 — see [LICENSE](LICENSE).

## Acknowledgments

- Register layout reference: ST [LIS2DH12](https://www.st.com/resource/en/datasheet/lis2dh12.pdf) / [LIS3DH](https://www.st.com/resource/en/datasheet/lis3dh.pdf) datasheets
- SC7A20H part documentation: [Silan Microelectronics](http://www.silan.com.cn/)
