/**
 * @file es7111_audio_codec.h
 * @brief ES7111(DAC) + ES7210(ADC) 音频编解码器
 *
 * ES7111: 纯 I2S DAC，无 I2C，硬件自配置，喂数据即出声
 * ES7210: 4通道 I2C ADC，需要 I2C 初始化
 * 两颗芯片共享 I2S duplex 总线（MCLK/BCLK/WS）
 *
 * 耳机模式: USB_SW 切换模拟开关后，I2S DIN 从 ES7210 切到耳机 mic，
 *           RX 需从 TDM 重配为 Standard 模式
 */

#pragma once

#include "audio_codec.h"
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <driver/i2s_tdm.h>
#include <mutex>
#include <vector>

class Es7111AudioCodec : public AudioCodec {
public:
    Es7111AudioCodec(void* i2c_master_handle,
                           int input_sample_rate, int output_sample_rate,
                           gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                           gpio_num_t dout, gpio_num_t din,
                           gpio_num_t pa_pin,
                           uint8_t es7210_addr,
                           bool input_reference);
    virtual ~Es7111AudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void SetInputGain(float gain) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

    /// 切换耳机模式（重配 I2S RX + 增益补偿）
    void SetHeadsetMode(bool headset);

private:
    // ES7210 ADC (I2C 控制)
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;

    // ES7111 无需 codec 设备，直接 I2S 写入
    gpio_num_t pa_pin_;
    bool headset_mode_ = false;
    std::mutex data_if_mutex_;
    std::vector<int16_t> tdm_buf_;  // 4ch TDM 读取缓冲区（复用避免频繁分配）

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                              gpio_num_t dout, gpio_num_t din);

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;
};
