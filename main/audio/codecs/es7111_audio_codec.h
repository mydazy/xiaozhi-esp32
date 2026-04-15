/**
 * @file es7111_audio_codec.h
 * @brief ES7111(DAC) + ES7210(ADC) 音频编解码器
 *
 * ES7111: 纯 I2S DAC，无 I2C，硬件自配置，喂数据即出声
 * ES7210: 4通道 I2C ADC，需要 I2C 初始化
 * 两颗芯片共享 I2S duplex 总线（MCLK/BCLK/WS）
 *
 * 架构：整体沿用 BoxAudioCodec 的输入侧组织方式，
 * TX 改为直接 i2s_channel_write（ES7111 无需 codec_dev），
 * RX 仍通过 codec_dev 管理 ES7210。data_if 不传 tx_handle，
 * 防止 esp_codec_dev_open(input) 重配 TX 破坏输出。
 *
 * 根据 ES7111 规格书，数字接口是标准 I2S，从左声道取单声道数据；
 * 因此保持 16-bit I2S 写入，由软件补成 L/R 两个 slot 即可。
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

    void SetHeadsetMode(bool headset);

private:
    // data_if 仅供 RX（tx_handle=NULL，隔离 TX）
    const audio_codec_data_if_t* data_if_ = nullptr;
    // ES7210 ADC (I2C 控制)
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t input_dev_ = nullptr;

    gpio_num_t pa_pin_;
    bool headset_mode_ = false;
    std::mutex data_if_mutex_;
    std::vector<int16_t> tdm_buf_;

    void ApplyInputGainLocked();
    void ReinitTxChannel();
    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                              gpio_num_t dout, gpio_num_t din);

    i2s_std_config_t tx_std_cfg_ = {};  // 保存 TX 配置，用于 EnableInput 后恢复

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;
};
