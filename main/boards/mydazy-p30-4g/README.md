# MyDazy P30 4G 开发板（老版本硬件）

## 硬件配置

- **主控**: ESP32-S3 (8MB PSRAM + 16MB Flash)
- **显示**: 284x240 LCD + AXS5106L 电容触摸
- **音频**: ES8311 (DAC) + ES7210 (ADC)，BoxAudioCodec 驱动
- **网络**: WiFi + BLE + ML307R 4G
- **传感器**: SC7A20H 三轴加速度计
- **电源**: 1000mAh 锂电池

## 与 P31 的差异

| 特性 | P30-4G (本板) | P31 (新版) |
|------|---------------|------------|
| 音频 DAC | ES8311 (I2C) | ES7111 (纯 I2S) |
| 音频驱动 | BoxAudioCodec | Es7111AudioCodec |
| 音频电源 | GPIO_9 | GPIO_15 |
| 充电检测 | GPIO_21 | GPIO_44 |
| TE 引脚 | GPIO_40 | NC |
| Type-C 耳机 | 无 | 有 |
| NFC | 无 | WS1850S |
| GPS | 无 | ML307R 内置 GNSS |

## 编译

```bash
source idf55 && idf.py set-target esp32s3
idf.py menuconfig  # 选择 BOARD_TYPE_MYDAZY_P30_4G
idf.py build
```
