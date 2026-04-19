# esp_lcd_touch_axs5106l

MyDazy P30 系列用的 **AXS5106L** 电容触摸屏 C++ 驱动，自研（非乐鑫官方组件）。

## 芯片背景

- **厂家**：深圳芯之源 ChipSourceTek（`en.chipsourcetek.com`）
- **I2C 地址**：0x63（7 位）
- **协议**：和同家族 AXS15231B 兼容（复位命令、数据布局、升级命令完全一致）
- **Datasheet 不公开寄存器表**；本驱动所有寄存器来自厂家 demo + 对比 AXS15231B 乐鑫组件

## 屏幕适配

配套屏：**HQR180009BH**（湖南宏祺瑞，1.80" TFT 240×284，JD9853 驱动）。
屏厂把 AXS5106L 焊在屏 FPC 上并烧写定制固件 V2905。

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

V2905 固件 active area 硬编码为 **[21..273]×[1..236]**（实测 700+ 采样确认）。
屏幕左 21px / 右 10px / 下 3px 触不到。

**根因**：edge suppression 烧在 chip MTP 里，**无 I2C 寄存器可配**。
穷尽厂家 demo + 上游 AXS15231B 组件 + 本驱动所有寄存器访问（`0x01/0x05/0x08/0x19/0xF0` + 升级用 `0x80/0x90/0xA0/0xAA`），没有分辨率或 edge 配置。

**唯一解法**：找屏厂（湖南宏祺瑞）用 ChipSourceTek FAE 工具出一版 edge=0 固件。
量产 UI 按键避开屏边缘 ≥25px。

不要在驱动层做 MIN/MAX 线性拉伸——会让中心区域也被拉伸，精度下降 ~12%。

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
    └── axs5106l_firmware.h                    # 内嵌 V2905 二进制（15071 字节）
```

## 调试开关

`AXS5106L_TOUCH_DEBUG_OVERLAY`（默认 0）打开后：

- 屏幕画四条 2px 红色边缘线
- 触摸时显示红色跟手圆点
- 每 20 次触摸打印 `[calib] chip raw X=[..] Y=[..]` 帮助观察 edge suppression 范围

量产固件必须保持关闭。
