#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define BRAND_NAME              "MyDazy"      // 品牌名称（用于蓝牙和热点配网）

// MyDazy UI硬件能力定义（编译时确定，运行时可检测更新）
#define MYDAZY_HAS_TOUCH       1    // 触摸屏支持
#define MYDAZY_HAS_4G_CAPABLE  1    // 4G模块支持
#define MYDAZY_HAS_ACCELEROMETER 1  // 加速度计支持
#define MYDAZY_HAS_BATTERY     1    // 电池支持

// ============================================================
// 系统配置
// ============================================================
#define IS_TEST_MODE            0     // 测试模式（开发调试用）
#define CONFIG_EN_SC7A20H_WAKE  1     // 启用SC7A20H传感器唤醒功能

// ============================================================
// 音频系统配置（ES8311 DAC + ES7210 ADC，老版本硬件）
// ============================================================

// 音频采样率配置
#define AUDIO_INPUT_SAMPLE_RATE  24000   // 音频输入采样率 (Hz)
#define AUDIO_OUTPUT_SAMPLE_RATE 24000   // 音频输出采样率 (Hz)
#define AUDIO_INPUT_REFERENCE    true    // 音频输入参考

// ===== GPIO配置 =====
#define AUDIO_PWR_EN_GPIO       GPIO_NUM_9     // AUD_VDD-3.3V 总电源开关：音频芯片(ES8311/ES7210) + 4G模块(ML307R) + LCD 三者共用（高=供电，低=断电；拉低再拉高可硬复位三者）
#define AUDIO_CODEC_PA_PIN      GPIO_NUM_10    // PA功放使能

// I2S接口
#define AUDIO_I2S_GPIO_MCLK     GPIO_NUM_17    // 主时钟
#define AUDIO_I2S_GPIO_BCLK     GPIO_NUM_16    // 位时钟
#define AUDIO_I2S_GPIO_WS       GPIO_NUM_14    // 字选择
#define AUDIO_I2S_GPIO_DOUT     GPIO_NUM_13    // 播放数据输出（ESP32→ES8311）
#define AUDIO_I2S_GPIO_DIN      GPIO_NUM_18    // 录音数据输入（ES7210→ESP32）

// I2C接口
#define AUDIO_CODEC_I2C_PORT    I2C_NUM_1      // 与触摸屏共用
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_11    // 外部已上拉
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_12    // 外部已上拉
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR  // 0x30 (对应7位地址0x18)
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR  // 0x80 (对应7位地址0x40)

// ============================================================
// 按钮 GPIO 配置
// ============================================================
#define BOOT_BUTTON_GPIO        GPIO_NUM_0     // BOOT按钮（多功能：单击/双击/长按/连按）
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_42    // 音量+按钮
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_45    // 音量-按钮
#define BUILTIN_LED_GPIO        GPIO_NUM_NC    // 内置LED（未使用）

// ============================================================
// 显示 SPI GPIO 配置
// ============================================================
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false  // 背光输出反转（false=高电平开启）
#define DISPLAY_BACKLIGHT       GPIO_NUM_41    // 背光PWM控制
#define DISPLAY_SPI_HOST        SPI2_HOST      // 显示SPI主机（SPI2）
#define DISPLAY_SPI_MISO        GPIO_NUM_NC    // SPI数据输入（未使用，单向传输）
#define DISPLAY_SPI_MOSI        GPIO_NUM_38    // SPI数据输出（MOSI到LCD）
#define DISPLAY_SPI_SCLK        GPIO_NUM_47    // SPI时钟信号（SCLK）
#define DISPLAY_LCD_RESET       GPIO_NUM_NC    // LCD 独立复位脚未连接；硬复位通过 AUDIO_PWR_EN_GPIO(GPIO9) 共享电源断电实现
#define DISPLAY_LCD_DC          GPIO_NUM_48    // LCD数据/命令选择（DC，高=数据，低=命令）
#define DISPLAY_LCD_CS          GPIO_NUM_39    // LCD片选信号（CS，低电平有效）
#define DISPLAY_LCD_TE          GPIO_NUM_40    // LCD撕裂效应信号（TE，预留未使用）
#define DISPLAY_INVERT_COLOR    false

// 显示物理参数
#define DISPLAY_WIDTH    284   // 显示宽度（像素）
#define DISPLAY_HEIGHT   240   // 显示高度（像素）
#define DISPLAY_MIRROR_X true  // 水平镜像（逆时针90度旋转）
#define DISPLAY_MIRROR_Y false // 垂直镜像
#define DISPLAY_SWAP_XY  true  // 交换XY坐标（横屏/竖屏切换）
#define DISPLAY_OFFSET_X 0     // X轴偏移（用于屏幕对齐）
#define DISPLAY_OFFSET_Y 0     // Y轴偏移

// ============================================================
// 触摸屏配置（AXS5106L触控芯片）
// ============================================================
#define TOUCH_RST_NUM      GPIO_NUM_4          // 触摸屏复位引脚
#define TOUCH_INT_NUM      GPIO_NUM_5          // 触摸屏中断引脚（触摸时触发）
#define TOUCH_I2C_SPEED_HZ (400 * 1000)        // 触摸屏I2C速率（400kHz，官方推荐速率）
#define TOUCH_SWAP_XY      false               // chip V2905 固件已内部 rotation，host 不再旋转
#define TOUCH_MIRROR_X     false               // 同上
#define TOUCH_MIRROR_Y     false               // 同上
// 注意：触摸屏与音频编解码器共用I2C总线（GPIO11/GPIO12，外部10kΩ上拉）

// ============================================================
// 4G模块配置（ML307R）
// ============================================================
#define ML307_IS_EXIST   1                     // ML307模块存在标志
#define ML307_RX_PIN     GPIO_NUM_1            // ML307 UART接收引脚（ESP32 TX -> ML307 RX）
#define ML307_TX_PIN     GPIO_NUM_2            // ML307 UART发送引脚（ESP32 RX <- ML307 TX）
#define ML307R_PWR_GPIO  GPIO_NUM_NC           // ML307 独立电源控制未连接；实际电源由 AUDIO_PWR_EN_GPIO(GPIO9) 与音频芯片共享，断电即重置
#define ML307_RST_GPIO   GPIO_NUM_NC           // ML307 复位引脚未连接；硬复位通过 AUDIO_PWR_EN_GPIO(GPIO9) 断电实现
// 注意：ML307模块无DTR引脚，深度睡眠后无法通过UART自动唤醒，依赖模块自身电源管理

// ============================================================
// 电源管理配置
// ============================================================
#define POWER_MANAGER_GPIO  GPIO_NUM_21    // 充电状态检测（低电平=充电中，开漏+内部上拉）
#define BATTERY_ADC_GPIO    GPIO_NUM_8     // 电池电压ADC检测（ADC1_CHANNEL_7）
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_7  // ADC通道（对应GPIO8）
#define BATTERY_CAPACITY_MAH 1000          // 电池容量（mAh）

// ============================================================
// 硬件扩展功能配置
// ============================================================
// SC7A20H 三轴加速度传感器（运动检测与唤醒）
#define SC7A20H_GPIO_INT1 GPIO_NUM_3       // SC7A20H中断引脚（运动检测时触发）

#endif // _BOARD_CONFIG_H_
