# MyDazy P30 4G — Reference Board

> A battery-powered handheld AI voice companion built on ESP32-S3 with on-board 4G modem, capacitive touch LCD, dual-mic + reference-loop ADC array, 3-axis accelerometer and a single shared LDO power tree.
>
> This directory is the **canonical reference design** of the project — fork it, rename it, change the pinout, and you have your own custom board. **All board-specific behavior is contained inside this directory; the upstream `main/boards/common/` files are kept untouched** so your fork stays mergeable.

| Field | Value |
|---|---|
| Schematic | [`MYDAZY-P30.pdf`](MYDAZY-P30.pdf) (3-page A4) |
| Hardware manual | [`HARDWARE.md`](HARDWARE.md) — pin-by-pin engineering doc |
| Custom-board guide | [`docs/custom-board.md`](../../../docs/custom-board.md) · [中文](../../../docs/custom-board_zh.md) |
| MCP guide | [`docs/mcp-usage.md`](../../../docs/mcp-usage.md) · [中文](../../../docs/mcp-usage_zh.md) |
| Code style | [`docs/code_style.md`](../../../docs/code_style.md) |

---

## 1. At a glance

| Spec | Value |
|---|---|
| MCU | ESP32-S3R8 (8 MB Octal PSRAM, 16 MB flash `BY25Q128`) |
| Display | 1.83" custom TFT `HQR180009BH` with **R = 25 px** rounded corners, JD9853 driver, **284×240** landscape (rotated from native 240×284) |
| Touch | AXS5106L capacitive (firmware `V2905`), I²C `0x63`, hardware INT |
| **Audio** | **ES8311 mono DAC** + **ES7210 4-ch ADC** (2 mics + 2 reference-loop) + NS4150B 8.5 W class-D PA, **on-device AEC** |
| Network | Wi-Fi 4 + BLE 5.0 + ML307R Cat.1 4G (DualNetworkBoard auto-failover) |
| Sensor | SC7A20H 3-axis accelerometer (motion / shake / pickup wake) |
| Battery | 1000 mAh Li-Po, TP4054 charger, ADC voltage sense + open-drain charge-detect |
| Buttons | BOOT (multi-function) + VOL+ / VOL− |
| **Power gate** | **GPIO 9 → ME6211 LDO → AUD_VDD-3.3V → cascade LCD + audio + 4G `VDD_EXT`** |
| Wake sources | EXT0 (BOOT) · EXT1 (motion) · TIMER (alarm) |
| IDF target | `esp32s3` |
| IDF version | **5.5+** (uses `esp_lcd_jd9853` v2.0.0 component which targets IDF 5.5) |
| Open-source drivers | [`mydazy/esp_lcd_jd9853`](https://components.espressif.com/components/mydazy/esp_lcd_jd9853) · [`mydazy/esp_lcd_touch_axs5106l`](https://components.espressif.com/components/mydazy/esp_lcd_touch_axs5106l) · [`mydazy/esp_sc7a20h`](https://components.espressif.com/components/mydazy/esp_sc7a20h) — all pure-C handle style |

---

## 2. Hardware architecture

### 2.1 System block diagram

```
                ┌──────────────────────────────────────────────────────┐
                │                     ESP32-S3R8                         │
                │  Core 0 (modem/codec/network)  Core 1 (audio/LVGL)   │
                └───────────┬─────────────┬─────────────┬──────────────┘
                            │I²C1         │I²S0         │SPI2
                            │400 kHz      │24 kHz       │40 MHz
            ┌───────────────┼─────────────┼─────────────┼────────────┐
            │               │             │             │            │
        ┌───▼───┐      ┌────▼────┐   ┌────▼────┐   ┌────▼─────┐
        │ES8311 │◄────►│ ES7210  │   │ MIC×2   │   │ JD9853   │
        │  DAC  │      │ 4-ch ADC│   │REF L/R  │   │ 1.83" TFT│
        └───┬───┘      └────┬────┘   └─────────┘   └────┬─────┘
            │NS4150B        │MIC1/MIC2 + REF_L/R         │
            │PA             │                             │ AXS5106L
            ▼               ▼                             │ touch (I²C)
         ┌─────┐        analog mic           ┌────────────┴─────┐
         │ 8Ω  │                             │  PWM backlight   │
         │spkr │                             │  GPIO 41 (LEDC)  │
         └─────┘                             └──────────────────┘

UART1 ──► ML307R Cat.1 4G ──► SIM + 2× IPEX antenna
GPIO3 ◄── SC7A20H accelerometer INT (also EXT1 wake)
GPIO 21 ◄ TP4054 CHRG (open-drain, low = charging)
GPIO 8 ── ADC1_CH7 ◄ 1MΩ:1MΩ divider on VBAT
```

### 2.2 Power tree (single rail / single LDO)

```
USB-C 5V ─┬─► TP4054 ──► 1000 mAh Li-Po ──► VBAT (3.0–4.2 V)
          │
          └────────────────────────────────►
                                            │
       ┌────────────────────────────────────┘
       ▼
   ME6211 LDO (EN = GPIO 9 = AUDPWR-EN)
       │
       ▼ 3.3 V (AUD_VDD-3.3V)
       │
       ├──► ES8311 / ES7210 (codec)
       ├──► NS4150B PA
       ├──► JD9853 LCD core + driver IC
       └──► ML307R `VDD_EXT`  (modem digital domain)
```

> **Why this matters**: setting `GPIO9 = 0` resets *all four* peripherals at once. There is no independent reset line for the LCD or the modem on this revision — the LDO cycle is the only path. Software treats this as a feature: the [`ShutdownHandler`](#33-reboot-model--esp_register_shutdown_handler) below uses it to recover the JD9853 panel from any corrupted state on every `esp_restart()`.

---

## 3. Audio chain — dual-mic + AEC reference loop 🎙️

This is the most critical signal path on the board. AEC (acoustic echo cancellation) requires the firmware to know **what came out of the speaker** so it can subtract that contribution from **what the mics picked up** — the reference loop is what makes that physically possible.

### 3.1 Signal path (full duplex)

```
                                  ┌─── MIC1  (top, primary)
                                  │
                                  │    ┌── MIC2  (bottom, secondary)
                                  │    │
                                  ▼    ▼
                              ┌─────────┐
                              │ ES7210  │  4-ch I²S TDM @ 24 kHz / 16-bit
                              │ 4ch ADC │
                              │         │  CH1 = MIC1
                              │         │  CH2 = MIC2
                              │         │  CH3 = REF_L  ◄──┐  re-sampled
                              │         │  CH4 = REF_R  ◄──┤  speaker line
                              └────┬────┘                   │   (analog loop-back)
                                   │ DIN GPIO18              │
                                   ▼                          │
                              ESP32-S3 I²S0 RX                │
                                   │                          │
                              ┌────▼─────────┐                │
                              │ esp-sr AFE   │                │
                              │ (AEC + NS    │                │
                              │  + VAD + AGC)│                │
                              └────┬─────────┘                │
                                   │ clean mic stream         │
                                   ▼                          │
                              wake-word + ASR + WS to cloud   │
                                                              │
                                                              │
                              cloud TTS PCM ──────► I²S0 TX   │
                                                       │      │
                                                       ▼      │
                                                  ┌─────────┐ │
                                                  │ ES8311  │ │
                                                  │  DAC    │ │
                                                  └────┬────┘ │
                                                       ▼       │
                                              ┌──────────────┐ │
                                              │  NS4150B PA  │─┴─► 8Ω speaker
                                              └──────────────┘  (and analog feedback
                                                                 to ES7210 CH3/CH4)
```

### 3.2 Why two reference channels (REF_L / REF_R)?

ES7210 is a 4-channel ADC; the board wires **2 mics** + **stereo loopback** of the analog speaker line. Even though the DAC is mono and mostly only `REF_L` carries useful signal, taking both halves:

1. lets the AEC reject common-mode noise (LDO ripple coupled into both REF lines cancels out),
2. survives PCB rework where REF_L could be unwired (`AUDIO_INPUT_REFERENCE = true` lets `BoxAudioCodec` pick whichever channel is non-zero).

### 3.3 AEC enable knobs (sdkconfig)

| Flag | Default | Effect |
|---|---|---|
| `CONFIG_USE_DEVICE_AEC` | **`y`** (this board) | esp-sr AFE consumes REF_L/REF_R, runs AEC on-device |
| `CONFIG_USE_SERVER_AEC` | n | (mutually exclusive with the above) ship raw mic + ref to server |

Set in [`config.json`](config.json) — the board's release build is opinionated: AEC is always on-device because the 4G uplink is too narrow for raw mic+ref at 24 kHz.

### 3.4 Sample rate

24 kHz / 16-bit / 4 channels @ ~1.5 Mbps over I²S TDM. Matches what the wake-word + ASR models expect; do **not** raise to 48 kHz without checking esp-sr model compatibility.

### 3.5 Adding a 3rd mic / replacing the array

1. ES7210 is 4-ch ADC — you can wire **up to 4 mics with no reference** (if you do server-side AEC) by setting `AUDIO_INPUT_REFERENCE = false` in `config.h`.
2. To add a 3rd mic on top of the AEC path: not possible without dropping one of REF_L/REF_R. Use a different 6-ch ADC (e.g. ES8389) and rewrite [`BoxAudioCodec`](../../audio/codecs/box_audio_codec.cc) → `Es8389AudioCodec`.

---

## 4. Pin map (summary)

> Authoritative source: [`config.h`](config.h). Full table including reserved / strap / risk-flagged pins is in [`HARDWARE.md §2`](HARDWARE.md).

| Bus / function | Pins |
|---|---|
| **I²S0** (audio) | MCLK 17 · BCLK 16 · WS 14 · DOUT 13 · DIN 18 |
| **I²C1** (codec + touch + sensor, 400 kHz) | SDA 11 · SCL 12 (10 kΩ external pull-up) |
| **SPI2** (LCD) | MOSI 38 · CS 39 · SCLK 47 · DC 48 (TE 40 reserved) |
| **UART1** (ML307R 4G) | RX 1 · TX 2 (DTR `NC` — modem auto-powered) |
| Audio PA enable | GPIO 10 |
| LCD backlight (PWM) | GPIO 41 |
| 🔴 Master LDO (`AUDPWR-EN`) | **GPIO 9** — gates LCD + audio + 4G VDD_EXT |
| Buttons | BOOT 0 · VOL+ 42 · VOL− 45 |
| Touch | RST 4 · INT 5 |
| Accelerometer INT | GPIO 3 |
| Battery sense | GPIO 8 (ADC1_CH7) |
| Charge detect | GPIO 21 (open-drain, low = charging) |
| USB | D+ 20 · D− 19 (S3 native USB-OTG) |

---

## 5. Build & flash

```bash
source idf55                                     # or set IDF_PATH manually to v5.5+
idf.py set-target esp32s3
idf.py menuconfig                                # Xiaozhi Assistant → Board Type → MyDazy P30 4G
idf.py build
idf.py -p /dev/cu.usbmodem2101 -b 2000000 flash monitor
```

CI / release packaging (uses `config.json` `sdkconfig_append`):

```bash
python scripts/release.py mydazy-p30-4g
# → releases/v<x.y.z>_mydazy-p30-4g.zip  (single merged-binary.bin, full-flash image)
```

Effective `sdkconfig_append` from [`config.json`](config.json):

| Flag | Effect |
|---|---|
| `CONFIG_USE_DEVICE_AEC=y` | enable on-device AEC (consumes ES7210 ref loop) |
| `CONFIG_BOARD_TYPE_MYDAZY_P30_4G=y` | select this board |
| `CONFIG_USE_EMOTE_MESSAGE_STYLE=y` | render UI with animated `EmoteDisplay` |
| `CONFIG_LV_USE_GIF/PNG/LODEPNG=y` | image decoders for `emoji/*.gif` |
| `CONFIG_BT_NIMBLE_BLUFI_ENABLE=y` | BLE provisioning (BluFi) |
| `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_MODE_OCT=y` | Octal PSRAM (R8 chip requires) |
| `CONFIG_LV_DEF_REFR_PERIOD=33` | LVGL refresh period (~30 fps) |

---

## 6. Directory layout

| File | Role | When you'd touch it |
|---|---|---|
| [`config.h`](config.h) | All GPIO / sample-rate / battery / display macros | Any pinout / panel change |
| [`config.json`](config.json) | Build matrix entry — name, target, `sdkconfig_append` | Toggle build-time features, rename output `.bin` |
| [`mydazy_p30_board.cc`](mydazy_p30_board.cc) | Board class — bring-up, button events, deep-sleep flow, MCP tool registration | Rewire init order, replace a peripheral, customize gestures |
| [`p30_button.h`](p30_button.h) | `class P30Button : public Button` — extends upstream `Button` with **multi-stage long-press** + **multi-count multi-click** via espressif/button native event_args. **No upstream patches.** | Re-tune long-press thresholds / click counts |
| [`p30_backlight.h`](p30_backlight.h) | `class P30Backlight : public PwmBacklight` — clamps LEDC duty to ~30 % of full-scale (matches BL current budget) + 15 % minimum brightness. **No upstream patches.** | Tune brightness range / clamp |
| [`power_manager.h`](power_manager.h) | Battery state-of-charge curves, charge detect, ADC calibration, low-battery callback | Different battery chemistry / capacity |
| [`emote.json`](emote.json) | Emote name ↔ asset mapping (24 emotes, GIF/PNG, fps, loop) | Swap facial expressions |
| [`layout.json`](layout.json) | Screen layout slots (`eye_anim`, `status_icon`, `toast_label`, `clock_label`, `listen_anim`) | Reposition UI elements without touching code |
| [`emoji/`](emoji/) | 24 Twitter-style 284×240 GIF emojis, baked into the asset partition via `BOARD_EMOJI_COLLECTION` | Replace any GIF; names must match `emote.json` |
| [`HARDWARE.md`](HARDWARE.md) | Full hardware engineering doc (pin-by-pin, design caveats, next-rev wishlist) | Hardware bring-up & PCB respin |
| [`MYDAZY-P30.pdf`](MYDAZY-P30.pdf) | Schematic (3 pages, A4) | Cross-check against `config.h` |

---

## 7. Board class — `MyDazyP30_4GBoard`

```cpp
class MyDazyP30_4GBoard : public DualNetworkBoard {
public:
    MyDazyP30_4GBoard();             // ctor wires the entire pipeline

    // Required Board interface overrides
    AudioCodec*  GetAudioCodec()  override;   // BoxAudioCodec(ES8311 + ES7210 + AEC ref)
    Display*     GetDisplay()     override;   // EmoteDisplay or SpiLcdDisplay
    Backlight*   GetBacklight()   override;   // P30Backlight (duty-clamped PwmBacklight)
    bool         GetBatteryLevel(int& l, bool& chg, bool& dschg) override;
    void         SetPowerSaveLevel(PowerSaveLevel) override;

    // Board-specific
    void         SwitchNetworkType();         // Wi-Fi ↔ ML307 (writes NVS, esp_restart())
    void         WakeUp();                    // wake the PowerSaveTimer
    static void  ShutdownHandler();           // ESP-IDF shutdown hook (see §7.3)

    // No destructor — single-instance board, process lifetime = device lifetime
    // (matches all 70+ upstream boards)
};
```

### 7.1 Boot sequence (constructor)

```
esp_register_shutdown_handler(ShutdownHandler)   // 1. install reboot hook FIRST
        │
        ▼
InitializeGpio()                                  // 2. backlight off, PA enable, LDO soft-start, wake-cause dispatch
InitializeI2c()                                   // 3. I²C1 @ 400 kHz, 4 slaves
PrepareTouchHardware()                            // 4. AXS5106L reset + MTP firmware verify (must come before LCD because RST is shared)
InitializeSpi()                                   // 5. SPI2 host
InitializeDisplay()                               // 6. JD9853 + LVGL/Emote bring-up
InitializeTouch()                                 // 7. attach touch as lv_indev (LVGL is up now)
InitializeSc7a20h()                               // 8. accelerometer + motion-detect IRQ
InitializePowerManager()                          // 9. ADC battery + charge GPIO
InitializePowerSaveTimer()                        // 10. light-sleep / deep-sleep scheduler
InitializeButtons()                               // 11. wire BOOT + VOL gestures
InitializeTools()                                 // 12. register board-specific MCP tools
GetAudioCodec()                                   // 13. instantiate BoxAudioCodec (lazy until now to give I²S the bus alone)
GetBacklight()->RestoreBrightness()               // 14. fade BL up to NVS-saved value
ApplyDefaultSettings()                            // 15. seed NVS first-boot defaults
StartWelcomeTask()                                // 16. async boot logo + welcome OGG (Core 1, 3 KB stack, self-deletes)
```

Touch init is split into two phases (`PrepareTouchHardware` before LVGL, `InitializeTouch` after) because the AXS5106L `RST` line is shared with the LCD reset.

### 7.2 Power & sleep model

Three shutdown paths, each with a different teardown:

| Path | Trigger | Teardown |
|---|---|---|
| **Reboot** (`esp_restart()`) | OTA, factory-reset, network switch, panic | `ShutdownHandler` runs first → drop `GPIO9`, hold low across reset, 500 ms cap discharge |
| **Deep sleep** | 5 min idle / VOL+VOL- power-off / 4 click power-off | `EnterDeepSleep()` — explicit teardown (audio, touch, I²C, BL, LDO, GPIOs to input) → arm EXT0/EXT1/Timer wake → `esp_deep_sleep_start()` |
| **Brown-out** | VBAT < 3.4 V & not charging | Alert → 3 s → `EnterDeepSleep(false)` (no motion wake to avoid endless loop) |

### 7.3 Reboot model — `esp_register_shutdown_handler`

```cpp
static void ShutdownHandler() {
    gpio_set_level(AUDIO_PWR_EN_GPIO, 0);          // drop master LDO
    rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);           // hold low across esp_restart()
    esp_rom_delay_us(500 * 1000);                  // wait 22 µF cap to discharge
}

// Registered once, in the constructor:
esp_register_shutdown_handler(ShutdownHandler);
```

This is **the** way to make every `esp_restart()` recover the JD9853 panel — and it works **without any patch to `Board::`**, `application.cc` or `system_reset.cc`. Upstream `esp_restart()` calls from any path (OTA, factory reset, panic, or this board's own `SwitchNetworkType()`) all run the handler before the actual restart.

> **Open-source rationale**: an earlier draft added a `virtual void PrepareForReboot()` to `Board`. That was rejected because it forces every other board author to think about a P30-specific hook. The shutdown-handler pattern keeps the workaround local to this directory.

### 7.4 Buttons — `P30Button` (extends `Button`, no upstream patches)

`p30_button.h` adds two capabilities by calling `iot_button_register_cb` with `event_args` directly:

- `OnLongPress(cb, press_time_ms)` — register multiple thresholds on the same key (700 / 3000 / 5000 ms below)
- `OnMultipleClick(cb, count)` — register multiple click-counts on the same key (3 / 9)

Hides the base-class single-callback versions only on `P30Button` instances; the upstream `Button` class is untouched, so `volume_up_button_` / `volume_down_button_` (still typed `Button`) keep their original behavior.

#### Event matrix (BOOT key)

| Gesture | Action | Implementation |
|---|---|---|
| Single click | wake / abort TTS / toggle chat ; cancel pending factory-reset confirmation | `HandleBootClick` |
| Double click | (a) confirm factory reset within 15 s window from 9-click trigger ; (b) toggle device-side AEC mode (when `CONFIG_USE_DEVICE_AEC`) | `HandleBootDoubleClick` |
| 3× rapid clicks | toggle network Wi-Fi ↔ 4G | `HandleBootMultiClick3_SwitchNetwork` |
| 9× rapid clicks | **arm** factory-reset (Alert: "double-click to confirm in 15 s") | `HandleBootMultiClick9_FactoryReset` |
| Long press 700 ms | start PTT recording | `HandleBootLongPress` |
| Long press 3 s | shutdown countdown warning | `HandleBootLongPress3s_ShutdownWarn` |
| Long press 5 s | execute shutdown → deep sleep | `HandleBootLongPress5s_ShutdownConfirm` |
| Press up | stop PTT (send) ; cancel shutdown countdown | `HandleBootPressUp` |

#### Volume keys (typed plain `Button`)

| Gesture | Action |
|---|---|
| Single click VOL+ / VOL− | volume ± 10 |
| Long press | continuous ramp (FreeRTOS task, ± 5 every 200 ms, on Core 1) |
| Press up | stop ramp |

### 7.5 MCP tools registered by this board

Built on top of `McpServer::AddCommonTools()` (which already provides `set_volume`, `set_brightness`, `set_theme`, `take_photo`, `reboot`, `upgrade_firmware`, `get_device_status`…). This board's `InitializeTools()` adds three P30-specific ones:

| Name | Visibility | Purpose |
|---|---|---|
| `self.network.switch_to_4g` | public (AI-callable) | switch to ML307 cellular, soft reboot |
| `self.network.switch_to_wifi` | public | switch to Wi-Fi, soft reboot |
| `self.power.deep_sleep` | **user-only** (hidden from AI) | enter deep sleep, wake via BOOT or motion |

`user-only` means the cloud must pass `withUserTools=true` when listing — see [`docs/mcp-usage.md`](../../../docs/mcp-usage.md). User-only tools are intended for companion-app initiated actions, not autonomous AI commands.

### 7.6 First-boot NVS defaults

Idempotent — only writes when the key is missing, so user choices made later are preserved across firmware updates:

| NVS namespace | Key | Default | Meaning |
|---|---|---|---|
| `audio` | `output_volume` | 80 | clamp to ≥ 50 if a previously-saved value was lower |
| `audio` | `playWelcome` | 1 | play welcome OGG on first boot |
| `wifi` | `blufi` | 1 | use BluFi (BLE) provisioning |
| `display` | `theme` | `dark` | UI theme |
| `display` | `brightness` | 35 | LEDC duty (0–100) |
| `network` | `type` | 1 | 1 = ML307 (4G), 0 = Wi-Fi |

---

## 8. Extensibility 🔧

This is the section to read if you're using P30 as a starting point for your own product.

### 8.1 Adding a headphone jack (auto PA mute)

Goal: when a 3.5 mm TRRS plug is inserted, mute the speaker PA and route audio to the headphone tip/ring.

**Hardware** (one extra GPIO + one mechanical detect contact):

```
GPIO_HP_DETECT (any free GPIO, e.g. GPIO 7, with internal pull-up)
       │
       └── 3.5 mm TRRS jack mechanical sense pin (closes to GND when plug inserted)

NS4150B (PA) /SD pin
       │
       └── controlled by GPIO_PA_SD (could reuse AUDIO_CODEC_PA_PIN with logic inversion)
                                    or a dedicated GPIO to keep PA on for jack-out
```

**Firmware patches** (all inside this directory):

1. `config.h` — add `#define HEADPHONE_DETECT_GPIO GPIO_NUM_7`.
2. `mydazy_p30_board.cc` — in `InitializeGpio()` configure as input + pull-up + edge interrupt.
3. Add an ISR that toggles `audio_codec_->EnableOutput(true/false)` and the PA GPIO based on `gpio_get_level()`.
4. *(Optional)* register an MCP tool `self.audio.headphone_status` returning `{"plugged":bool}`.

The ES8311 itself doesn't need to know — the analog mux is upstream of the codec (PA → speaker vs. line-out → headphone tip). A simpler PCB approach is a single SPDT analog switch (e.g. SGM3005) on the PA output, gated by the same detect signal.

### 8.2 Type-C expansion

The board already routes USB D+/D− (GPIO 20/19) to the ESP32-S3 native OTG controller. Type-C-specific signals (CC1/CC2, SBU1/SBU2) are not currently sensed in firmware but are wired to test points. Extending support is straightforward:

| Feature | How |
|---|---|
| **Plug detect (UFP)** | one GPIO with an external 56 kΩ Rd to GND on each CC line, ESP32 reads the divider voltage via ADC. Or use a CC sense IC (FUSB302, TUSB320, HUSB238) over I²C1. |
| **USB Audio Class (UAC) sink** | enable `CONFIG_TINYUSB_USB_AUDIO_DEVICE` in sdkconfig, add a `tinyusb_uac` task that pipes UAC PCM to the existing I²S0 TX path (replacing or paralleling the ES8311 stream). |
| **USB CDC console** | `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` — already free, just enable. Useful as a secondary log channel when 4G is the only network. |
| **PD source/sink negotiation** | requires an external PD controller (FUSB302) on I²C1; firmware uses [`espressif/usb_pd`](https://components.espressif.com/components/espressif/usb_pd) component. |
| **DisplayPort alt-mode** | not supported on ESP32-S3 (no MUX, no DP PHY). Would require a USB-C to HDMI bridge IC. |

For all of the above, **changes stay inside `mydazy-p30-4g/`** — add an `InitializeUsb()` method to the board class and call it from the constructor.

### 8.3 Swapping peripherals

| Want to change | Files to touch |
|---|---|
| **LCD panel** | `config.h` (size + mirror flags), `mydazy_p30_board.cc::InitializeDisplay()` (replace `esp_lcd_jd9853_create_panel` with the new vendor's call) |
| **Touch IC** | `config.h` (RST/INT pins), replace `axs5106l_*` calls with the equivalent driver (e.g. `cst816s`, `gt911` from `espressif/esp_lcd_touch_*` components) |
| **Audio codec** | `mydazy_p30_board.cc::GetAudioCodec()` — swap `BoxAudioCodec` for `Es8311AudioCodec` (single mic, no AEC), `Es8388AudioCodec` (stereo), or write a custom subclass of `AudioCodec` |
| **Accelerometer / IMU** | `config.h` + `InitializeSc7a20h()` — for BMI270 use [`espressif/bmi270_sensor`](https://components.espressif.com/components/espressif/bmi270_sensor) which is already in `idf_component.yml` |
| **Battery chemistry / capacity** | `power_manager.h` `levels_charging_` / `levels_discharging_` arrays |
| **Brightness range** | `p30_backlight.h` — change the `(300 * brightness) / 100` to your panel's safe duty cap |
| **Button thresholds** | `mydazy_p30_board.cc::InitializeButtons()` — change the `, 700)`, `, 3000)`, `, 5000)` long-press thresholds and the `OnMultipleClick(..., N)` count |

### 8.4 Forking to a brand-new board

```bash
cp -r main/boards/mydazy-p30-4g main/boards/mybrand-x1
```

Then edit (in this order):

1. **`config.h`** — flip `MYDAZY_HAS_*` capability flags, change pin numbers, panel size, sample rate, `BRAND_NAME`.
2. **`config.json`** — rename `name`, drop `sdkconfig_append` flags you don't need (e.g. `CONFIG_BT_*` if no BLE).
3. **`<name>_board.cc`** — change the class name, swap `BoxAudioCodec` if your codec differs, drop `DualNetworkBoard` → `WifiBoard` if no 4G, drop `ShutdownHandler` if you have a real LCD reset line.
4. **`main/Kconfig.projbuild`** — add a `config BOARD_TYPE_MYBRAND_X1 / depends on IDF_TARGET_ESP32S3` entry so `menuconfig` lists it.
5. **`main/CMakeLists.txt`** — add an `elseif(CONFIG_BOARD_TYPE_MYBRAND_X1)` branch with `set(BOARD_TYPE "mybrand-x1")` + font / emoji collection.
6. **`HARDWARE.md`** — keep the structure (block diagram, GPIO table, design notes), update the contents.
7. **`MYDAZY-P30.pdf`** — replace with your own schematic export.

⚠️ **Do not reuse `BOARD_TYPE_MYDAZY_P30_4G`** or you'll cross-pollinate OTA channels. Each board needs its own identity (see [`docs/custom-board.md`](../../../docs/custom-board.md)).

### 8.5 What to keep vs. drop when forking

| If your hardware doesn't have | Drop |
|---|---|
| Shared LDO (independent power rails) | `ShutdownHandler` and its `esp_register_shutdown_handler` registration |
| 4G modem | `DualNetworkBoard` → `WifiBoard`; drop `SwitchNetworkType`, MCP `switch_to_4g/wifi` tools |
| Reference loopback for AEC | set `AUDIO_INPUT_REFERENCE = false` in `config.h`; AEC moves to server-side or off |
| Multi-stage long-press needs | drop `p30_button.h`, use plain upstream `Button` |
| Brightness duty clamping | drop `p30_backlight.h`, use plain upstream `PwmBacklight` |
| Battery | drop `power_manager.h` and the corresponding `GetBatteryLevel` override |
| Accelerometer | drop `InitializeSc7a20h` and EXT1 wakeup; only EXT0 (BOOT) wakes the device |

---

## 9. Open-source compliance 🌍

This board follows three rules to remain a clean fork target for the upstream xiaozhi-esp32 project:

1. **No upstream patches required** — every P30-specific behavior lives in `main/boards/mydazy-p30-4g/`. The `main/boards/common/` directory, `application.cc`, `system_reset.cc`, `Board` base class are kept at upstream baseline. Verify with `git diff origin/main main/boards/common/ main/application.cc` (should show no functional changes related to this board).

2. **Standard IDF mechanisms over custom hooks**:
    - Reboot side-effects → `esp_register_shutdown_handler` (ESP-IDF stock API), not a new `virtual` on `Board`.
    - Multi-stage button events → `iot_button_register_cb` with `event_args` (espressif/button stock API), not a fork of `main/boards/common/button.cc`.
    - Brightness clamp → subclass `PwmBacklight`, not a global `#ifdef` in common backlight code.

3. **Drivers as standalone components** — three reusable peripheral drivers (`esp_lcd_jd9853`, `esp_lcd_touch_axs5106l`, `esp_sc7a20h`) are published on the Espressif Component Registry under the `mydazy/` namespace, with semver releases and CHANGELOGs. Vendored copies in `components/` are kept in sync.

If you find a P30-specific change leaking into `main/`, it's a bug — please open an issue.

---

## 10. Open-source drivers <a id="open-source-drivers"></a>

The three peripheral drivers used by this board are published as standalone components on the Espressif Component Registry. They are vendored into [`components/`](../../../components/) for reproducibility, but downstream forks should pull them via `idf_component.yml`:

```yaml
dependencies:
  mydazy/esp_lcd_jd9853:           "^2.0.0"
  mydazy/esp_lcd_touch_axs5106l:   "^2.0.0"
  mydazy/esp_sc7a20h:              "^2.0.0"
```

| Component | Purpose | Registry · GitHub |
|---|---|---|
| `mydazy/esp_lcd_jd9853` | JD9853 panel driver (1.83″ 240×284 IPS, 4-wire SPI, RGB565/666). | [Registry](https://components.espressif.com/components/mydazy/esp_lcd_jd9853) · [GitHub](https://github.com/mydazy/esp_lcd_jd9853) |
| `mydazy/esp_lcd_touch_axs5106l` | AXS5106L capacitive touch + LVGL `lv_indev_t` integration + on-boot MTP firmware upgrade. | [Registry](https://components.espressif.com/components/mydazy/esp_lcd_touch_axs5106l) · [GitHub](https://github.com/mydazy/esp_lcd_touch_axs5106l) |
| `mydazy/esp_sc7a20h` | SC7A20H 3-axis accelerometer + motion-detect interrupt + one-call EXT1 deep-sleep wakeup. | [Registry](https://components.espressif.com/components/mydazy/esp_sc7a20h) · [GitHub](https://github.com/mydazy/esp_sc7a20h) |

**v2.0.0 — pure-C handle API**: `*_handle_t` opaque pointer + free functions; callbacks use C function pointers + `void *user_ctx` (no `std::function`). C-callable from C++, ESP-Brookesia, Rust binding, etc.

```cpp
// Touch — two-phase init (RST shared with LCD)
axs5106l_touch_config_t tcfg = AXS5106L_TOUCH_DEFAULT_CONFIG(
    i2c_bus_, TOUCH_RST_NUM, TOUCH_INT_NUM, DISPLAY_WIDTH, DISPLAY_HEIGHT);
tcfg.swap_xy = false; tcfg.mirror_x = false; tcfg.mirror_y = false;
axs5106l_touch_new(&tcfg, &touch_driver_);              // before LVGL
// ... bring up LCD + start LVGL ...
axs5106l_touch_attach_lvgl(touch_driver_);              // after LVGL
axs5106l_touch_set_gesture_callback(touch_driver_, &OnTouchGesture, this);

// Accelerometer — one-call init + motion-detect IRQ
sc7a20h_config_t scfg = SC7A20H_DEFAULT_CONFIG();
scfg.i2c_addr = 0x19;
sc7a20h_create_with_motion_detection(i2c_bus_, &scfg, NULL, &sc7a20h_sensor_);
```

Lambda captures (`[this](...){...}`) become **static trampolines + `this` user-context** — see `OnTouchWake` / `OnTouchGesture` in `mydazy_p30_board.cc`.

---

## 11. Hardware caveats

| 🔴 Severity | Issue | Mitigation |
|---|---|---|
| 🔴 P0 | `GPIO9` cascades LCD + audio + 4G `VDD_EXT` | `ShutdownHandler` recovers all on reboot; respin to split for next rev |
| 🔴 P0 | ML307R has **no `RESET-N`** line | `AT+CFUN=4 → 1`, fall back to GPIO9 cycle |
| 🔴 P0 | Octal PSRAM occupies GPIO 33–37 | Don't repurpose them as GPIO |
| 🟡 P1 | I²C bus shared by 4 slaves — 4G RF can corrupt traffic | Driver retries 3× with 5/10/20 ms back-off |
| 🟡 P1 | Touch FW `V2905` has hard-coded edge suppression | UI tap targets ≥ 25 px from edge |
| 🟡 P1 | Tasks running on Core 0 during NVS / OTA writes need **internal-RAM stacks** | See `HARDWARE.md` "PSRAM stack double-exception trap" |
| 🟢 P2 | LCD `TE` is wired (GPIO40) but VSYNC software not enabled | Possible future upgrade for tear-free rendering |
| 🟢 P2 | BOOT key on a strap pin — long-press at power-up enters download mode | RC delay on next rev |

---

## 12. Debug recipes

| Symptom | First check |
|---|---|
| Touch dead | I²C corrupted by 4G RF — driver auto-retries; if still dead, `axs5106l: fw V2905` in boot log |
| LCD glitches | shared LDO — only recoverable with reboot. Issue soft restart (`Ctrl-T R` in monitor); `ShutdownHandler` will cycle GPIO9 |
| 4G stuck "no service" | `AT+CFUN=4 → 1`; check antenna mating (A2/A3 IPEX) and SIM seating (USIM1) |
| Battery reads ~0% | Verify 1 MΩ : 1 MΩ divider; boot log should say `PowerManager: Calibration Success` |
| `SP=0x60100000` panic | PSRAM-stacked task scheduled during NVS/OTA flash op — see `HARDWARE.md` stack placement |
| Reboot → black screen | If you forked and forgot to register `ShutdownHandler`, GPIO9 was never cycled — JD9853 GRAM kept stale data. Re-add the registration. |

---

## 13. License & references

- Project root: [`../../../README.md`](../../../README.md)
- Custom board guide: [`docs/custom-board.md`](../../../docs/custom-board.md)
- WebSocket protocol: [`docs/websocket.md`](../../../docs/websocket.md)
- MQTT + UDP protocol: [`docs/mqtt-udp.md`](../../../docs/mqtt-udp.md)
- MCP usage: [`docs/mcp-usage.md`](../../../docs/mcp-usage.md)
- Code style: [`docs/code_style.md`](../../../docs/code_style.md)
- ESP32-S3 datasheet: <https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf>
- ML307R Cat.1: <https://www.openluat.com/>

License: MIT (matches project root). Schematic and the three `mydazy/*` driver components are Apache-2.0.

---

# MyDazy P30 4G —— 参考开发板（中文）

> ESP32-S3 电池供电手持 AI 语音陪伴设备：板载 4G、电容触摸 LCD、双麦+回采 ADC、三轴加速度计、单 LDO 共享电源树。
>
> 本目录是项目的**标准参考设计** —— Fork、改名、改 pinout 即得到你自己的板。**所有板专属逻辑都隔离在本目录内，上游 `main/boards/common/` 保持原版**，方便后续合并上游。

## 1. 一览

| 项 | 值 |
|---|---|
| 主控 | ESP32-S3R8（8 MB Octal PSRAM、16 MB Flash） |
| 显示 | 1.83" 定制 TFT，**R=25 px 圆角**，JD9853 驱动，284×240 横屏 |
| 触控 | AXS5106L（固件 `V2905`），I²C `0x63` |
| **音频** | **ES8311 单声 DAC** + **ES7210 4 通道 ADC**（**双麦 + 双声道回采**）+ NS4150B 8.5 W 功放，**设备端 AEC** |
| 网络 | Wi-Fi 4 + BLE 5.0 + ML307R Cat.1 4G（双网卡自动切换） |
| 传感器 | SC7A20H 三轴加速度（运动/晃动/拿起唤醒） |
| 电池 | 1000 mAh 锂电、TP4054 充电、ADC 采样 + 充电检测 |
| **电源主开关** | **GPIO 9 → ME6211 LDO → AUD_VDD-3.3V，级联 LCD + 音频 + 4G `VDD_EXT`** |
| 唤醒源 | EXT0（BOOT）· EXT1（运动）· TIMER（闹钟） |
| IDF 版本 | **5.5+** |
| 自研开源驱动 | [`mydazy/esp_lcd_jd9853`](https://components.espressif.com/components/mydazy/esp_lcd_jd9853) · [`mydazy/esp_lcd_touch_axs5106l`](https://components.espressif.com/components/mydazy/esp_lcd_touch_axs5106l) · [`mydazy/esp_sc7a20h`](https://components.espressif.com/components/mydazy/esp_sc7a20h) |

## 2. 音频信号链 — 双麦 + 回采 AEC 🎙️

```
  MIC1 (主麦)    MIC2 (副麦)
     │              │
     ▼              ▼
   ┌────────────────────────┐
   │      ES7210 4ch ADC     │  TDM I²S @ 24 kHz / 16-bit
   │  CH1 = MIC1             │
   │  CH2 = MIC2             │
   │  CH3 = REF_L  ◄──────┐  │  ←── 喇叭模拟回采（用作 AEC 参考）
   │  CH4 = REF_R  ◄──────┤  │
   └────────────┬─────────┼──┘
                │ DIN     │
                ▼         │
            ESP32-S3      │ 模拟回环
                │         │
            esp-sr AFE    │
            (AEC+NS+VAD)  │
                │         │
                ▼         │
        干净人声 → 上行    │
                          │
       下行 PCM →  I²S0 TX
                ▼         │
            ┌───────┐     │
            │ES8311 │     │
            │  DAC  │     │
            └───┬───┘     │
                ▼         │
            NS4150B PA ───┴───► 8Ω 喇叭
```

**为什么要双声道回采？**

ES7210 是 4 通道 ADC：板上接 **2 麦 + 立体声回采**。虽然 DAC 是单声道，但取双声道：
1. AEC 可抑制共模噪声（LDO 纹波耦合到两路 REF 上等量出现，相减后抵消）。
2. PCB 重工后 REF_L 可能掉线，`AUDIO_INPUT_REFERENCE = true` 让 `BoxAudioCodec` 自动选有信号的那一路。

**核心 sdkconfig**：`CONFIG_USE_DEVICE_AEC=y`（设备端 AEC，吃 ES7210 回采通道）。固化在 `config.json` 里 —— 4G 上行带宽不够裸传双麦+双回采。

**采样率**：24 kHz / 16-bit / 4 ch ≈ 1.5 Mbps —— 与 esp-sr 唤醒+ASR 模型对齐，不要拉到 48 kHz。

## 3. 重启模型 — 用 ESP-IDF 标准 `esp_register_shutdown_handler`

P30 的特殊点：LCD 没有独立复位脚，**唯一**复位办法是断 LDO（GPIO9）。要让任何 `esp_restart()`（OTA、出厂、网络切换、panic）都自动走断电流程，本板用 ESP-IDF 标准 API 注册 shutdown handler：

```cpp
static void ShutdownHandler() {
    gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
    rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);    // 穿越 esp_restart 保持 LOW
    esp_rom_delay_us(500 * 1000);           // 等 22 µF 电容放电
}

// 构造函数中注册一次：
esp_register_shutdown_handler(ShutdownHandler);
```

**完全不需要修改** `Board` 基类、`application.cc`、`system_reset.cc` —— 上游不接受额外虚函数也能工作。这是本板对"全球开源规范"的具体落实。

## 4. 按键事件矩阵（最新）

`p30_button.h` 的 `P30Button : public Button` 用 `iot_button_register_cb + event_args` 实现"多段长按"和"多次连击"，**不修改上游 `main/boards/common/button.cc`**。

| 手势 | 动作 |
|---|---|
| 单击 | 唤醒 / 打断 TTS / 切换对话；同时取消"等待出厂复位确认"标志 |
| 双击 | （a）若处于 9 连击触发后 15 s 窗口 → 确认恢复出厂；（b）切换设备端 AEC 开关 |
| 3 连击 | Wi-Fi ↔ 4G 切换 |
| 9 连击 | 触发恢复出厂请求（提示 15 s 内双击确认） |
| 长按 0.7 s | 开始 PTT 录音 |
| 长按 3 s | 关机倒计时提醒 |
| 长按 5 s | 真正关机进深睡 |
| 抬起 | PTT 录音停止发送；或取消关机倒计时 |

音量键（普通 `Button`）：单击 ±10、长按持续递增（每 200 ms ±5）、抬起停止。

## 5. MCP 工具

通用工具（音量/亮度/主题/拍照/重启/升级/`get_device_status`/`get_system_info` 等）由 `McpServer::AddCommonTools()` 自动注册。本板新增 3 个：

| 工具名 | 类型 | 用途 |
|---|---|---|
| `self.network.switch_to_4g` | 公开 | 切到蜂窝网（重启） |
| `self.network.switch_to_wifi` | 公开 | 切到 Wi-Fi（重启） |
| `self.power.deep_sleep` | user-only | 进深睡（仅 companion app 可调，AI 不可主动关机） |

`user-only` 含义：云端必须以 `withUserTools=true` 列举才能看到 — 详见 [`docs/mcp-usage_zh.md`](../../../docs/mcp-usage_zh.md)。

## 6. 扩展性 🔧

### 6.1 增加耳机通道

目标：插入 3.5 mm TRRS 时自动静音 PA、把音频路由到耳机 tip/ring。

**硬件**：
- 一个 GPIO（如 GPIO 7）+ 内部上拉，接 TRRS 座的机械检测脚（插入时拉到 GND）
- NS4150B 的 `/SD` 脚由独立 GPIO 控制（或 SGM3005 SPDT 模拟开关在 PA 输出后切换）

**固件改动**（全部在本目录）：
1. `config.h` 加 `#define HEADPHONE_DETECT_GPIO GPIO_NUM_7`
2. `mydazy_p30_board.cc::InitializeGpio()` 配置为输入 + 上拉 + 边沿中断
3. ISR 内根据电平翻转 `audio_codec_->EnableOutput(true/false)` 和 PA GPIO
4. 可选：注册 MCP 工具 `self.audio.headphone_status`

ES8311 不感知耳机，模拟切换在功放后端做即可（PA → 喇叭 vs. line-out → 耳机）。

### 6.2 Type-C 扩展

板已经把 USB D+/D− 接到 ESP32-S3 native OTG。CC1/CC2、SBU 信号在测试点上，固件可后续扩展：

| 功能 | 实现思路 |
|---|---|
| 插拔检测（UFP） | 一根 GPIO + 56 kΩ Rd 下拉，ADC 读分压；或用 FUSB302 等 CC 检测 IC 走 I²C1 |
| USB Audio Class（UAC sink） | sdkconfig 开 `CONFIG_TINYUSB_USB_AUDIO_DEVICE`，`tinyusb_uac` 任务把 UAC PCM 接入现有 I²S0 TX 通路 |
| USB CDC 串口 | `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` —— 现成可用，作 4G 单网时辅助日志通道 |
| PD 源/汇协商 | 加 FUSB302 走 I²C，使用 [`espressif/usb_pd`](https://components.espressif.com/components/espressif/usb_pd) 组件 |
| DisplayPort alt-mode | ESP32-S3 不支持（无 MUX、无 DP PHY），需要外置 USB-C → HDMI 桥接 IC |

所有改动**保持在本目录**：在 board 类加 `InitializeUsb()` 方法、构造函数调用即可。

### 6.3 替换外设

| 想换 | 改动文件 |
|---|---|
| LCD | `config.h` + `InitializeDisplay()`（替换 `esp_lcd_jd9853_create_panel` 为新厂商 API） |
| 触摸 IC | `config.h` + 把 `axs5106l_*` 替换为 `cst816s` / `gt911`（用 `espressif/esp_lcd_touch_*`） |
| 音频 codec | `GetAudioCodec()` —— `BoxAudioCodec` 换为 `Es8311AudioCodec`（单麦无 AEC）/`Es8388AudioCodec`（立体声）/ 自定义 `AudioCodec` 子类 |
| IMU | `config.h` + `InitializeSc7a20h()` —— BMI270 用 `espressif/bmi270_sensor` |
| 电池 | `power_manager.h` 的 `levels_*` 数组 |
| 亮度范围 | `p30_backlight.h` 的 `(300 * brightness) / 100` 改为你屏的安全 duty 上限 |
| 按键阈值 | `mydazy_p30_board.cc::InitializeButtons()` 的 `, 700)`、`, 3000)`、`, 5000)` 与 `OnMultipleClick(..., N)` |

### 6.4 Fork 派生新板

```bash
cp -r main/boards/mydazy-p30-4g main/boards/mybrand-x1
```

依次改 `config.h` → `config.json` → `<name>_board.cc`（类名 + `BOARD_TYPE` 字符串）→ `Kconfig.projbuild` → `main/CMakeLists.txt` → `HARDWARE.md` → 原理图。**绝不能复用** `BOARD_TYPE_MYDAZY_P30_4G`（OTA 通道会串）。

### 6.5 Fork 时按需丢弃的模块

| 你的硬件不具备 | 删 |
|---|---|
| 共享 LDO（独立电源） | `ShutdownHandler` 与其注册 |
| 4G 模块 | `DualNetworkBoard` → `WifiBoard`，删 `SwitchNetworkType` 和 `switch_to_4g/wifi` MCP 工具 |
| 双声道回采 | `config.h` 中 `AUDIO_INPUT_REFERENCE = false`，AEC 转到服务端或关闭 |
| 多段长按需求 | 删 `p30_button.h`，用上游 `Button` |
| 亮度 duty 钳位 | 删 `p30_backlight.h`，用上游 `PwmBacklight` |
| 电池 | 删 `power_manager.h` 与 `GetBatteryLevel` override |
| 加速度计 | 删 `InitializeSc7a20h`，只保留 EXT0（BOOT）唤醒 |

## 7. 开源合规承诺 🌍

本板对上游 xiaozhi-esp32 项目的承诺：

1. **不需要任何上游 patch** —— 所有 P30 专属行为都在本目录内。`main/boards/common/`、`application.cc`、`system_reset.cc`、`Board` 基类保持上游基线。
2. **优先使用 ESP-IDF / espressif 标准 API**：
    - 重启副作用 → `esp_register_shutdown_handler`
    - 多段按键事件 → `iot_button_register_cb` + `event_args`
    - 亮度钳位 → 子类化 `PwmBacklight`
3. **驱动作为独立组件发布** —— 三个驱动以 `mydazy/` namespace 上架 Espressif Component Registry，有 semver 和 CHANGELOG。

如果发现 P30 专属改动泄漏到 `main/`，那是 bug —— 欢迎提 issue。

## 8. 硬件陷阱（速记）

详细见 [`HARDWARE.md`](HARDWARE.md)。

| 等级 | 问题 | 对策 |
|---|---|---|
| 🔴 P0 | GPIO9 级联 LCD + 音频 + 4G | 流程上规避；下版分离 |
| 🔴 P0 | ML307R 无 RESET-N | `AT+CFUN=4 → 1`，兜底 GPIO9 断电 |
| 🔴 P0 | Octal PSRAM 占 GPIO 33–37 | 不当普通 GPIO 用 |
| 🟡 P1 | I²C 4 设备共线，4G RF 干扰 | 驱动层 3 次重试 + 退避 |
| 🟡 P1 | 触摸固件边缘抑制 | UI 距边 ≥ 25 px |
| 🟡 P1 | NVS/OTA 写入期 PSRAM 栈崩溃 | 见 HARDWARE.md "PSRAM 栈双异常陷阱" |

## 9. 参考链接

- 项目根：[`../../../README.md`](../../../README.md) · [`README_zh.md`](../../../README_zh.md)
- 自定义开发板：[`docs/custom-board_zh.md`](../../../docs/custom-board_zh.md)
- WebSocket：[`docs/websocket_zh.md`](../../../docs/websocket_zh.md)
- MQTT + UDP：[`docs/mqtt-udp_zh.md`](../../../docs/mqtt-udp_zh.md)
- MCP 用法：[`docs/mcp-usage_zh.md`](../../../docs/mcp-usage_zh.md)
- ESP32-S3 数据手册：<https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf>
- ML307R：合宙官方 <https://www.openluat.com/>

License: MIT（与项目根一致）。原理图与三个 `mydazy/*` 驱动组件 Apache-2.0。
