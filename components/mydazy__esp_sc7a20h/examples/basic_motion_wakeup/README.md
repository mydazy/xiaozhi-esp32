# SC7A20H — basic motion wakeup

Minimal example that:
1. Initialises an SC7A20H over I2C,
2. enables motion detection on INT1 with built-in 500 ms debounce,
3. reads a few raw samples in milli-g,
4. configures deep-sleep wake-up on INT1 and enters deep sleep.

The next physical motion (any axis) wakes the MCU.

## Wiring (ESP32-S3 reference)

| SC7A20H | ESP32-S3 |
|---------|----------|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 6 (4.7 kΩ pull-up to 3V3) |
| SCL | GPIO 7 (4.7 kΩ pull-up to 3V3) |
| INT1 | GPIO 3 (RTC-capable) |

Pin numbers can be changed at the top of [`main/main.cc`](main/main.cc).

## Build & flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Expected output

```
I (300) sc7a20h_example: X =     2.0 mg   Y =     5.0 mg   Z =  1014.0 mg
I (500) sc7a20h_example: X =     1.0 mg   Y =     6.0 mg   Z =  1015.0 mg
...
I (1500) Sc7a20h: Deep-sleep wakeup configured on GPIO3
I (1510) sc7a20h_example: Entering deep sleep — move the device to wake.
```

When you pick up or shake the board, the chip's INT1 line goes high, the MCU resets, and `app_main` runs again.
