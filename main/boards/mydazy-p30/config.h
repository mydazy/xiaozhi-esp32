#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>


// MyDazy UI硬件能力定义（编译时确定，运行时可检测更新）
#define MYDAZY_HAS_TOUCH       1    // 触摸屏支持
#define MYDAZY_HAS_4G_CAPABLE  1    // 4G模块支持（可能存在）
#define MYDAZY_HAS_ACCELEROMETER 1  // 加速度计支持
#define MYDAZY_HAS_BATTERY     1    // 电池支持

// 音频采样率配置
// 硬件I2S共用时钟,输入输出必须相同采样率
// 软件层自动做 24kHz ↔ 16kHz 重采样 (AFE/Opus使用16kHz)
#define AUDIO_INPUT_SAMPLE_RATE  24000   // 硬件采样率 (Hz)
#define AUDIO_OUTPUT_SAMPLE_RATE 24000   // 硬件采样率 (Hz)
#define AUDIO_INPUT_REFERENCE    true    // 音频输入参考

// 双麦克风物理配置（实际只连接MIC1和MIC2，14mm间距）
#define AUDIO_MIC_SPACING_MM     14      // 麦克风间距（毫米）
#define AUDIO_MIC_SPACING_M      0.014f  // 麦克风间距（米，用于DOA算法）

// DOA声源定位配置（双麦克风左右定位，基于24kHz采样率和14mm间距）
#define AUDIO_DOA_ENABLED        1       // 启用DOA声源定位功能
#define AUDIO_DOA_RESOLUTION     30.0f   // DOA角度分辨率（度），双麦14mm推荐30度
#define AUDIO_DOA_FRAME_SAMPLES  1024    // DOA处理帧大小（1024点@24kHz=42.7ms）

// 音频 GPIO 配置
#define AUDIO_I2S_GPIO_DOUT     GPIO_NUM_13    // I2S 数据输出（DAC输出到ES8311）
#define AUDIO_I2S_GPIO_WS       GPIO_NUM_14    // I2S 字选择信号（左右声道切换）
#define AUDIO_I2S_8311_DIN      GPIO_NUM_15    // ES8311 ASDOUT（AEC参考信号）
#define AUDIO_I2S_GPIO_BCLK     GPIO_NUM_16    // I2S 位时钟（串行时钟）
#define AUDIO_I2S_GPIO_MCLK     GPIO_NUM_17    // I2S 主时钟（256fs基准时钟）
#define AUDIO_I2S_GPIO_DIN      GPIO_NUM_18    // I2S 数据输入（ADC输入从ES7210）

// 音频编解码器 I2C 配置（外部已有10kΩ上拉，禁用内部上拉）
// #define AUDIO_PWR_EN_GPIO       GPIO_NUM_48// tiger 9     // 音频电源使能（控制音频芯片供电）
#define AUDIO_PWR_EN_GPIO       GPIO_NUM_15     // 音频电源使能（控制音频芯片供电）
#define AUDIO_CODEC_PA_PIN      GPIO_NUM_10    // 音频功放使能（PA功放控制）

#define CC_ADC_PIN              GPIO_NUM_6 //CC_ADC YZT 

#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_11   // I2C 数据线（与触摸屏共用）
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_12   // I2C 时钟线（与触摸屏共用）
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR  // ES8311 DAC地址
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR  // ES7210 ADC地址
//#define AUDIO_CODEC_ES7243E_ADDR  ES7243E_CODEC_DEFAULT_ADDR  //tiger  ES7243E ADC地址

// ============================================================
// 按钮 GPIO 配置
// ============================================================
#define BOOT_BUTTON_GPIO        GPIO_NUM_0     // BOOT按钮（多功能：单击/双击/长按/连按）
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_42    // 音量+按钮
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_45    // 音量-按钮
#define BUILTIN_LED_GPIO        GPIO_NUM_NC    // 内置LED（未使用）

// 显示 SPI GPIO 配置
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false  // 背光输出反转（false=高电平开启）
#define DISPLAY_BACKLIGHT       GPIO_NUM_41    // 背光PWM控制
#define DISPLAY_SPI_HOST        SPI2_HOST      // 显示SPI主机（SPI2）
#define DISPLAY_SPI_MISO        GPIO_NUM_NC    // SPI数据输入（未使用，单向传输）
#define DISPLAY_SPI_MOSI        GPIO_NUM_38    // SPI数据输出（MOSI到LCD）
#define DISPLAY_SPI_SCLK        GPIO_NUM_47    // SPI时钟信号（SCLK）
#define DISPLAY_LCD_RESET       GPIO_NUM_NC    // LCD复位信号（未使用，软件复位）
//#define DISPLAY_LCD_DC          GPIO_NUM_40// tiger 48    // LCD数据/命令选择（DC，高=数据，低=命令）
#define DISPLAY_LCD_DC          GPIO_NUM_48    // LCD数据/命令选择（DC，高=数据，低=命令）
#define DISPLAY_LCD_CS          GPIO_NUM_39    // LCD片选信号（CS，低电平有效）
#define DISPLAY_LCD_TE          GPIO_NUM_NC// tiger  40    // LCD撕裂效应信号（TE，预留未使用）
//#define DISPLAY_LCD_TE          GPIO_NUM_40    // LCD撕裂效应信号（TE，预留未使用）
#define DISPLAY_INVERT_COLOR    false

// 显示物理参数
#define DISPLAY_WIDTH    284   // 显示宽度（像素）
#define DISPLAY_HEIGHT   240   // 显示高度（像素）
#define DISPLAY_MIRROR_X true  // 水平镜像（根据屏幕安装方向调整）
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
// 注意：触摸屏与音频编解码器共用I2C总线（GPIO11/GPIO12，外部10kΩ上拉）

// ============================================================
// 4G模块配置（ML307R）
// ============================================================
#define ML307_IS_EXIST   1                     // ML307模块存在标志
#define ML307_RX_PIN     GPIO_NUM_1            // ML307 UART接收引脚（ESP32 TX -> ML307 RX）
#define ML307_TX_PIN     GPIO_NUM_2            // ML307 UART发送引脚（ESP32 RX <- ML307 TX）
#define ML307R_PWR_GPIO  GPIO_NUM_NC           // ML307 电源控制（未连接，模块常供电）
#define ML307_RST_GPIO   GPIO_NUM_NC           // ML307 复位引脚（未连接，软件复位）
// 注意：ML307模块无DTR引脚，深度睡眠后无法通过UART自动唤醒，依赖模块自身电源管理

// ============================================================
// 电源管理配置
// ============================================================
//#define POWER_MANAGER_GPIO  GPIO_NUM_21    // 充电状态检测（高电平=充电中）
#define POWER_MANAGER_GPIO  GPIO_NUM_44    // 充电状态检测（高电平=充电中）ADD YZT
#define BATTERY_ADC_GPIO    GPIO_NUM_8     // 电池电压ADC检测（ADC1_CHANNEL_7）
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_7  // ADC通道（对应GPIO8）
#define BATTERY_CAPACITY_MAH 1000          // 电池容量（mAh）

// ============================================================
// 硬件扩展功能配置
// ============================================================
// SC7A20H 三轴加速度传感器（运动检测与唤醒）
#define SC7A20H_GPIO_INT1 GPIO_NUM_3       // SC7A20H中断引脚（运动检测时触发）



// 触摸屏 I2C 单独测试：屏蔽其它 I2C 设备（音频编解码器/SC7A20H/NFC）
#define MYDAZY_TOUCH_I2C_ONLY_TEST 1


#endif // _BOARD_CONFIG_H_