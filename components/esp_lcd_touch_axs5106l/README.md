# esp_lcd_touch_axs5106l

MyDazy P30 系列用的 **AXS5106L** 电容触摸屏 C++ 驱动，自研（非乐鑫官方组件）。

## 芯片背景

- **厂家**：深圳芯之源 ChipSourceTek（`en.chipsourcetek.com`）
- **I2C 地址**：0x63（7 位）
- **协议**：和同家族 AXS15231B 兼容（复位命令、数据布局、升级命令完全一致）
- **Datasheet 不公开寄存器表**；本驱动所有寄存器来自厂家 demo + 对比 AXS15231B 乐鑫组件

## 屏幕适配

配套屏：**HQR180009BH**（湖南宏祺瑞，1.80" TFT 240×284，JD9853 驱动）。
屏厂把 AXS5106L 焊在屏 FPC 上并烧写定制固件，当前版本 **V2907**（2026-04-20 升级，前版 V2905）。

## 对外接口

```cpp
Axs5106lTouch(i2c_master_bus_handle_t bus,
              gpio_num_t rst_gpio,    // 和 LCD 共用复位线
              gpio_num_t int_gpio,
              uint16_t width, uint16_t height,
              bool swap_xy, bool mirror_x, bool mirror_y);

bool Initialize();                         // 一次性初始化（硬复位 + 固件升级 + LVGL 注册）
bool InitializeHardware();                 // 仅 chip 硬件层（LCD 共用复位线时必须先调此）
bool InitializeInput();                    // LVGL 指针设备注册（在 Display init 后调用）
void Sleep();
void Resume();
void SetGestureCallback(TouchGestureCallback cb);   // 单击/双击/长按/四向滑动
```

和官方 esp_lcd_touch BSP 接口不同：本驱动直接暴露 C++ 对象 + LVGL indev，
以支持 P30 所需的特殊能力（见下文）。

## 关键设计

### 1. 坐标映射 1:1（不缩放）

`TOUCH_MAX_X=284, TOUCH_MAX_Y=240`，**chip 已内部做 rotation**，raw_x/raw_y 直接等于屏幕像素。
验证过 v1/udour/v32 三版驱动行为一致，不要加任何缩放。

### 2. 4G RF 抗干扰（主要价值）

P30 是 4G 设备，TX 突发会让 I2C 总线误触。驱动内建：

- **INT 去抖**：GPIO5 低电平持续 `INT_DEBOUNCE_US=3ms` 才读 I2C
- **按下窗口**：首次确认按下前等 `PRESS_CONFIRM_US=20ms`
- **释放去抖**：连续 2 帧无触摸才算释放
- **速度滤波**：阈值 `MAX_SPEED_THRESHOLD=2000px/s`，超过丢弃（正常滑动 ~1400px/s）
- **异常帧抑制**：连续 5 帧 I2C 失败，暂停触摸 300ms

**不要**切换到 chip 硬件 gesture 寄存器或 esp_lcd_touch BSP 接口——两者都会丢失上述滤波。

### 3. 固件自动升级

启动时读 chip 固件版本（0x05 寄存器），若和内嵌 `axs5106l_firmware.h` 版本不一致，
进入 0xAA/0x90/0xA0 调试模式重刷 MTP。

## ⚠️ 已知限制：Edge Suppression

| 固件 | X 可达 | Y 可达 | 死区（左/右/上/下） |
|---|---|---|---|
| V2905（2025-07 老版） | [21..273] | [1..236] | 21px / 10px / 1px / 3px |
| **V2907（2026-04-20 当前）** | **[9..272]** | **[1..255]** | **9px / 11px / 1px / 0px** |

> Y raw 最大 255 是 chip 在屏外的"溢出残留坐标"，host 层 `sy >= height_ ? height_-1 : sy` 已 clamp 到 239，对 UI 无影响。

V2907 相较 V2905 **左边缘改善 12px**（21→9），右边缘基本持平，下边缘完全可达。

**根因**：edge suppression 烧在 chip MTP 里，**无 I2C 寄存器可配**。
穷尽厂家 demo + 上游 AXS15231B 组件 + 本驱动所有寄存器访问（`0x01/0x05/0x08/0x19/0xF0` + 升级用 `0x80/0x90/0xA0/0xAA`），没有分辨率或 edge 配置。

**唯一彻底解法**：找屏厂（湖南宏祺瑞）用 ChipSourceTek FAE 工具出一版 edge=0 固件。
当前量产 UI 按键避开屏左右边缘 ≥ **12px** 即可（V2905 时代要求 25px）。

不要在驱动层做 MIN/MAX 线性拉伸——会让中心区域也被拉伸，精度下降 ~12%。

### 📋 待办

1. 继续推动屏厂 **湖南宏祺瑞**（HQR180009BH）出 edge=0 的 V29xx 固件二进制
2. 拿到后：
   - 替换 `include/axs5106l_firmware.h` 内嵌数据（目前 V2907，15071 字节）
   - 驱动自带 `CheckAndUpgradeFirmware()` 升级通道，OTA 自动刷入
3. 刷完后开 `AXS5106L_TOUCH_DEBUG_OVERLAY=1` 验证 raw 覆盖 [0..283]×[0..239]
4. 验证通过后把 `axs5106l_touch.h` 的 `AXS5106L_TOUCH_DEBUG_OVERLAY` 改回 0

### 坐标方向核对（踩坑记录）

**重要**：chip V2905/V2907 **均已内部 rotation** 输出 landscape 坐标，host 层 `TOUCH_SWAP_XY/MIRROR_X/MIRROR_Y` 必须**全 false**。不要跟 `DISPLAY_SWAP_XY/MIRROR_X` 对齐——LCD 是物理旋转，触摸是固件层旋转，两者在不同层完成。

判据（V2907 量产配置）：`[calib]` 日志应为 `raw X∈[9..272]`, `raw Y∈[1..255]`；触左上角 LVGL 收到 (≈9, ≈1)、触右下角收到 (≈272, 239)。如果 X 范围落在 [0..239]（短边）或 Y 超过 283，说明 swap/mirror 配反了。

## 文件结构

```
components/esp_lcd_touch_axs5106l/
├── CMakeLists.txt
├── idf_component.yml
├── README.md                                  # 本文件
├── axs5106l_touch.cc                          # 主驱动（C++ 类）
├── axs5106l_upgrade.cc                        # 固件升级流程
└── include/
    ├── axs5106l_touch.h                       # 对外 C++ 类头
    ├── axs5106l_upgrade.h
    └── axs5106l_firmware.h                    # 内嵌 V2907 二进制（15071 字节）
```

## 调试开关

`AXS5106L_TOUCH_DEBUG_OVERLAY`（默认 0）打开后：

- 触摸时显示 12×12 红色跟手圆点（`LV_LAYER_TOP` 层，不遮挡 UI）
- 每 20 次触摸打印 `[calib] chip raw X=[..] Y=[..]` 帮助观察 edge suppression 范围

量产固件必须保持关闭。

## 集成示例（以 P30-4G 板为例）

在 `config.h` 定义 GPIO 和显示参数：

```c
#define DISPLAY_WIDTH      284     // JD9853 旋转后的 landscape 宽
#define DISPLAY_HEIGHT     240     // 高
#define TOUCH_RST_NUM      GPIO_NUM_4
#define TOUCH_INT_NUM      GPIO_NUM_5
#define TOUCH_SWAP_XY      false   // chip V2905/V2907 已内部做旋转，不用 swap
#define TOUCH_MIRROR_X     false
#define TOUCH_MIRROR_Y     false
```

分两阶段初始化（关键：硬件层必须在 LCD 上电后、LVGL 启动前；输入层在 LVGL 启动后）：

```cpp
// 板子类里保存 I2C bus 和 touch_driver_ 指针
i2c_master_bus_handle_t i2c_bus_ = nullptr;
Axs5106lTouch* touch_driver_ = nullptr;

// --- 阶段 1: LCD 初始化前，拉 reset + 升级固件 + 验 chip ID ---
void PrepareTouchHardware() {
    touch_driver_ = new Axs5106lTouch(
        i2c_bus_, TOUCH_RST_NUM, TOUCH_INT_NUM,
        DISPLAY_WIDTH, DISPLAY_HEIGHT,
        TOUCH_SWAP_XY, TOUCH_MIRROR_X, TOUCH_MIRROR_Y);

    if (!touch_driver_->InitializeHardware()) {
        ESP_LOGE(TAG, "触摸屏硬件初始化失败");
        delete touch_driver_;
        touch_driver_ = nullptr;
    }
}

// --- 阶段 2: LCD + LVGL 启动后，注册输入设备 + 挂回调 ---
void InitializeTouch() {
    if (!touch_driver_) return;

    if (!touch_driver_->InitializeInput()) {
        delete touch_driver_;
        touch_driver_ = nullptr;
        return;
    }

    // 按下边沿唤醒 PowerSaveTimer
    touch_driver_->SetWakeCallback([this]() { WakeUp(); });

    // 手势上报
    touch_driver_->SetGestureCallback(
        [this](TouchGesture gesture, int16_t x, int16_t y) {
            WakeUp();
            switch (gesture) {
                case TouchGesture::SingleClick:
                    HandleTouchSingleClick();  // 唤醒对话 / 打断 TTS
                    break;
                case TouchGesture::DoubleClick:
                    /* 切换 AEC 等 */
                    break;
                case TouchGesture::LongPress:
                    /* 开始录音 */
                    break;
                case TouchGesture::SwipeLeft:
                case TouchGesture::SwipeRight:
                    /* 翻页 */
                    break;
                default: break;
            }
        });
}
```

初始化时序（在板子 Init 里）：

```
InitializeI2c();              // I2C bus
InitializeSpi();              // LCD SPI
PrepareTouchHardware();       // ← 此时可拉 TOUCH_RST（和 LCD RESET 独立）
InitializeDisplay();          // JD9853 + LVGL
InitializeTouch();            // ← LVGL 准备好后再注册 indev
```

深度睡眠前调用 `Sleep()`，唤醒后调用 `Resume()` 或整颗重启。
