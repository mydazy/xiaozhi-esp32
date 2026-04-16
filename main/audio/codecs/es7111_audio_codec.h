/**
 * @file es7111_audio_codec.h
 * @brief ES7111(DAC) + ES7210(ADC) 音频编解码器
 *
 * ES7111: 纯 I2S DAC，无 I2C，硬件自配置，标准 I2S Philips 格式
 * ES7210: 4通道 I2C ADC，TDM 格式
 * 两颗芯片共享 I2S duplex 总线（MCLK/BCLK/WS）
 *
 * TX 和 RX 都使用 TDM 模式（ESP-IDF duplex 要求同模式）：
 *   4 slots × 16-bit = 64 bits/frame, BCLK = sr × 64
 *   TX: 音频放 slot0，ES7111 从 WS=low 相位读 slot0
 *   RX: ES7210 输出 4 通道到 slot0-3
 *
 * 绕过 esp_codec_dev（避免 paired data 机制干扰 TX）。
 * ES7210 通过 codec_if 直接 I2C 控制。
 *
 * 软件回采 AEC：
 *   ES7111 无硬件环回，ES7210 4路 MIC 全接物理麦克风。
 *   Write() 将播放 PCM 写入环形缓冲区，Read() 从中取出作为 AEC 参考信号。
 *   输出格式：channel 0 = MIC1+MIC3 混合，channel 1 = TX 回采参考。
 *   AFE pipeline 用 "MR" 格式启用 AEC，实现任意打断。
 */

#pragma once

#include "audio_codec.h"
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <driver/i2s_tdm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
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

    void SetHeadsetMode(bool headset);

private:
    // ES7210 ADC (I2C 控制，直接通过 codec_if，不经过 esp_codec_dev)
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;

    gpio_num_t pa_pin_;
    bool headset_mode_ = false;
    bool codec_opened_ = false;
    std::mutex mutex_;
    std::vector<int16_t> tdm_buf_;
    std::vector<int16_t> write_buf_;

    // 软件回采环形缓冲区（Write→Read 传递 TX PCM 作为 AEC 参考）
    RingbufHandle_t ref_ringbuf_ = nullptr;
    static constexpr int kRefBufSamples = 4800;  // 200ms @24kHz

    void ApplyInputGainLocked();
    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                              gpio_num_t dout, gpio_num_t din);

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;
};
