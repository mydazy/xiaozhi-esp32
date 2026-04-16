#include "es7111_audio_codec.h"
#include "settings.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <driver/gpio.h>
#include <cmath>

#define TAG "Es7111AudioCodec"

namespace {
constexpr int kEs7210SlotCount = 4;
constexpr int kPrimaryMicSlot = 0;
constexpr int kSecondaryMicSlot = 2;
constexpr uint32_t kEs7210ActiveMicMask =
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(kPrimaryMicSlot) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(kSecondaryMicSlot);
}  // namespace

Es7111AudioCodec::Es7111AudioCodec(
    void* i2c_master_handle,
    int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
    gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin,
    uint8_t es7210_addr,
    bool input_reference)
    : pa_pin_(pa_pin) {

    duplex_ = true;
    // 强制启用 input_reference：软件回采 AEC
    // ES7111 无硬件环回，用 Write() 的 TX 数据做参考信号
    input_reference_ = true;
    input_channels_ = 2;  // channel 0 = mic, channel 1 = TX reference
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    Settings settings("audio", false);
    input_gain_ = static_cast<float>(settings.GetInt("input_gain", 36));

    // 1. 创建 duplex I2S 通道
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // 2. ES7111 DAC: 纯 I2S，无 I2C，硬件自配置
    ESP_LOGI(TAG, "ES7111 DAC: no I2C needed (hardware auto-config)");

    // 3. ES7210 ADC: 仅 I2C 控制，不创建 esp_codec_dev
    //    es7210_codec_new 内部已调用 es7210_open（完整 I2C 初始化）
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es7210_addr,
        .bus_handle = i2c_master_handle,
    };
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (in_ctrl_if_ == NULL) {
        ESP_LOGE(TAG, "ES7210 I2C ctrl init failed");
    }

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    if (in_codec_if_ == NULL) {
        ESP_LOGE(TAG, "ES7210 codec init failed");
    }

    // 创建软件回采环形缓冲区（PSRAM，NoSplit 类型保证原子读写）
    ref_ringbuf_ = xRingbufferCreateWithCaps(
        kRefBufSamples * sizeof(int16_t), RINGBUF_TYPE_BYTEBUF,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ref_ringbuf_) {
        ESP_LOGE(TAG, "Failed to create ref ringbuf, AEC disabled");
        input_reference_ = false;
        input_channels_ = 1;
    }

    ESP_LOGI(TAG, "Initialized: ES7111(DAC) + ES7210(ADC) gain=%.0fdB ref=%s",
             input_gain_, input_reference_ ? "software-loopback" : "off");
}

Es7111AudioCodec::~Es7111AudioCodec() {
    if (ref_ringbuf_) vRingbufferDelete(ref_ringbuf_);
    if (codec_opened_ && in_codec_if_ && in_codec_if_->close) {
        in_codec_if_->close(in_codec_if_);
    }
    if (in_codec_if_) audio_codec_delete_codec_if(in_codec_if_);
    if (in_ctrl_if_) audio_codec_delete_ctrl_if(in_ctrl_if_);
}

void Es7111AudioCodec::CreateDuplexChannels(
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
    gpio_num_t dout, gpio_num_t din) {

    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    // TX + RX 都用 TDM 模式（ESP-IDF duplex 要求 TX/RX 同模式）
    // 4 slots × 16-bit = 64 bits/frame
    // BCLK = 24000 × 64 = 1,536,000 Hz
    // WS half-period = 32 BCLK = 2 slots，ES7111 从 WS=low 相位读 slot0 的 16-bit MSB
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };

    // RX: 与 TX 相同的 TDM 配置，仅 GPIO 不同
    i2s_tdm_config_t rx_tdm_cfg = tdm_cfg;
    rx_tdm_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    rx_tdm_cfg.gpio_cfg.din = din;

    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(tx_handle_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &rx_tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex TDM: TX→ES7111(slot0) RX←ES7210(slot0-3) 4×16bit BCLK=%.0fkHz",
             output_sample_rate_ * 4.0 * 16 / 1000);
}

void Es7111AudioCodec::SetHeadsetMode(bool headset) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (headset == headset_mode_) return;
    headset_mode_ = headset;
    ESP_LOGI(TAG, "Headset mode %s (output gain %s)", headset ? "ON" : "OFF",
             headset ? "2x" : "1x");
}

void Es7111AudioCodec::SetOutputVolume(int volume) {
    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_set_level(pa_pin_, volume > 0 ? 1 : 0);
    }
    AudioCodec::SetOutputVolume(volume);
}

void Es7111AudioCodec::SetInputGain(float gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    input_gain_ = gain;
    ApplyInputGainLocked();
    AudioCodec::SetInputGain(gain);
}

void Es7111AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enable == input_enabled_) return;

    if (enable) {
        // 通过 codec_if->set_fs 配置 ES7210 采样参数（仅 I2C，不碰 I2S）
        if (in_codec_if_) {
            esp_codec_dev_sample_info_t fs = {
                .bits_per_sample = 16,
                .channel = kEs7210SlotCount,
                .channel_mask = 0,
                .sample_rate = (uint32_t)input_sample_rate_,
                .mclk_multiple = 0,
            };
            if (in_codec_if_->set_fs) {
                in_codec_if_->set_fs(in_codec_if_, &fs);
            }
            if (in_codec_if_->enable) {
                in_codec_if_->enable(in_codec_if_, true);
            }
            codec_opened_ = true;
        }
        ApplyInputGainLocked();
        ESP_LOGI(TAG, "Input enabled: sr=%d gain=%.0f", input_sample_rate_, input_gain_);
    } else {
        if (in_codec_if_ && in_codec_if_->enable) {
            in_codec_if_->enable(in_codec_if_, false);
        }
        ESP_LOGI(TAG, "Input disabled");
    }
    AudioCodec::EnableInput(enable);
}

void Es7111AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enable == output_enabled_) return;

    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_set_level(pa_pin_, enable ? 1 : 0);
    }
    AudioCodec::EnableOutput(enable);
    ESP_LOGI(TAG, "Output %s (PA=%d)", enable ? "enabled" : "disabled", enable ? 1 : 0);
}

int Es7111AudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_) return samples;

    // input_channels_ = 2 时 samples = frames * 2
    int frames = samples / input_channels_;
    int tdm_samples = frames * kEs7210SlotCount;
    if ((int)tdm_buf_.size() < tdm_samples) {
        tdm_buf_.resize(tdm_samples);
    }

    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_handle_, tdm_buf_.data(),
                                      tdm_samples * sizeof(int16_t),
                                      &bytes_read, pdMS_TO_TICKS(500));
    if (ret != ESP_OK || bytes_read == 0) {
        ESP_LOGE(TAG, "i2s_channel_read failed: %s bytes=%zu", esp_err_to_name(ret), bytes_read);
        return 0;
    }

    if (input_reference_ && ref_ringbuf_) {
        // 软件回采 AEC 模式：
        //   channel 0 = MIC1 + MIC3 混合（双麦信噪比更好）
        //   channel 1 = TX 回采参考（从环形缓冲区读取）
        size_t ref_bytes_needed = frames * sizeof(int16_t);
        size_t ref_bytes_got = 0;
        int16_t* ref_data = (int16_t*)xRingbufferReceiveUpTo(
            ref_ringbuf_, &ref_bytes_got, 0, ref_bytes_needed);

        int ref_frames = ref_data ? (int)(ref_bytes_got / sizeof(int16_t)) : 0;

        for (int f = 0; f < frames; f++) {
            // Channel 0: 双麦混合
            int32_t mixed = ((int32_t)tdm_buf_[f * kEs7210SlotCount + kPrimaryMicSlot]
                           + (int32_t)tdm_buf_[f * kEs7210SlotCount + kSecondaryMicSlot]) / 2;
            dest[f * 2] = (int16_t)mixed;
            // Channel 1: TX 回采参考（不足部分填 0 = 静音）
            dest[f * 2 + 1] = (f < ref_frames) ? ref_data[f] : 0;
        }

        if (ref_data) {
            vRingbufferReturnItem(ref_ringbuf_, ref_data);
        }
    } else {
        // 无 AEC 回退：单声道混合
        for (int f = 0; f < frames; f++) {
            int32_t mixed = ((int32_t)tdm_buf_[f * kEs7210SlotCount + kPrimaryMicSlot]
                           + (int32_t)tdm_buf_[f * kEs7210SlotCount + kSecondaryMicSlot]) / 2;
            dest[f] = (int16_t)mixed;
        }
    }
    return samples;
}

void Es7111AudioCodec::ApplyInputGainLocked() {
    if (!in_codec_if_ || !codec_opened_ || !input_enabled_) {
        return;
    }
    if (in_codec_if_->set_mic_channel_gain) {
        in_codec_if_->set_mic_channel_gain(in_codec_if_, kEs7210ActiveMicMask, input_gain_);
    } else if (in_codec_if_->set_mic_gain) {
        in_codec_if_->set_mic_gain(in_codec_if_, input_gain_);
    }
}

int Es7111AudioCodec::Write(const int16_t* data, int samples) {
    // 软件回采：将原始 PCM 写入环形缓冲区（不含音量缩放，AEC 需要原始信号）
    if (ref_ringbuf_) {
        xRingbufferSend(ref_ringbuf_, data, samples * sizeof(int16_t), 0);
    }

    if (!output_enabled_) return samples;

    // TDM 4-slot × 16-bit: 音频放 slot0，slot1-3 静音
    int tdm_samples = samples * kEs7210SlotCount;
    if ((int)write_buf_.size() < tdm_samples) {
        write_buf_.resize(tdm_samples);
    }

    int32_t vol_factor = static_cast<int32_t>(pow(output_volume_ / 100.0, 2) * 256);
    if (headset_mode_) vol_factor *= 2;
    for (int i = 0; i < samples; i++) {
        int32_t val = (static_cast<int32_t>(data[i]) * vol_factor) >> 8;
        if (val > INT16_MAX) val = INT16_MAX;
        if (val < INT16_MIN) val = INT16_MIN;
        int base = i * kEs7210SlotCount;
        write_buf_[base]     = (int16_t)val;
        write_buf_[base + 1] = 0;
        write_buf_[base + 2] = 0;
        write_buf_[base + 3] = 0;
    }

    size_t bytes_written = 0;
    i2s_channel_write(tx_handle_, write_buf_.data(),
                      tdm_samples * sizeof(int16_t),
                      &bytes_written, pdMS_TO_TICKS(500));
    return samples;
}
