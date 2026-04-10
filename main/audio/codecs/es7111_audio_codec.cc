#include "es7111_audio_codec.h"
#include "settings.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <driver/gpio.h>
#include <cmath>

#define TAG "Es7111Es7210"

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
    input_reference_ = input_reference;
    input_channels_ = input_reference ? 2 : 1;  // 有回声参考=2ch(MIC+REF)，无=1ch(双麦混合)
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    Settings settings("audio", false);
    input_gain_ = static_cast<float>(settings.GetInt("input_gain", 36));

    // 1. 创建 duplex I2S 通道（ES7111 + ES7210 共享总线）
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // 2. I2S 数据接口（供 ES7210 codec_dev 使用）
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // 3. PA GPIO 配置
    gpio_if_ = audio_codec_new_gpio();

    // 4. ES7111 DAC: 不需要任何初始化（纯 I2S，硬件自配置）
    ESP_LOGI(TAG, "ES7111 DAC: no I2C needed (hardware auto-config)");

    // 5. ES7210 ADC: I2C 初始化
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

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = in_codec_if_,
        .data_if = data_if_,
    };
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    if (input_dev_ == NULL) {
        ESP_LOGE(TAG, "Input device creation failed");
    }

    ESP_LOGI(TAG, "Initialized: ES7111(DAC,no-I2C) + ES7210(ADC,gain=%.0fdB)", input_gain_);
}

Es7111AudioCodec::~Es7111AudioCodec() {
    if (input_dev_) {
        esp_codec_dev_close(input_dev_);
        esp_codec_dev_delete(input_dev_);
    }
    if (in_codec_if_) audio_codec_delete_codec_if(in_codec_if_);
    if (in_ctrl_if_) audio_codec_delete_ctrl_if(in_ctrl_if_);
    if (gpio_if_) audio_codec_delete_gpio_if(gpio_if_);
    if (data_if_) audio_codec_delete_data_if(data_if_);
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

    // TX: Standard 模式 → ES7111 DAC
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
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

    // RX: TDM 模式 → ES7210 ADC（4通道）
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
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
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = { false, false, false },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex: TX(std)→ES7111 RX(tdm)←ES7210 mclk=%d bclk=%d ws=%d dout=%d din=%d",
             mclk, bclk, ws, dout, din);
}

// 注意：I2S duplex 模式下 TX/RX 共享控制器，不能单独删除重建 RX。
// 耳机模式保持 TDM RX 不变，耳机 mic 信号通过模拟开关(USB_SW)
// 接入 I2S DIN，会映射到 TDM slot0，Read() 中 ch0 提取即可。

void Es7111AudioCodec::SetHeadsetMode(bool headset) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (headset == headset_mode_) return;
    headset_mode_ = headset;

    // I2S RX 保持 TDM 不变（duplex 共享控制器，不能单独重建）
    // USB_SW 模拟开关已由 TypecHeadset 切换，耳机 mic 信号映射到 TDM slot0
    // Read() 中提取 ch0 即可拿到耳机 mic 数据
    ESP_LOGI(TAG, "Headset mode %s (output gain %s)", headset ? "ON" : "OFF",
             headset ? "2x" : "1x");
}

void Es7111AudioCodec::SetOutputVolume(int volume) {
    // ES7111 无音量寄存器，用软件音量控制（PA GPIO 开关）
    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_set_level(pa_pin_, volume > 0 ? 1 : 0);
    }
    AudioCodec::SetOutputVolume(volume);
}

void Es7111AudioCodec::SetInputGain(float gain) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    input_gain_ = gain;
    AudioCodec::SetInputGain(gain);
}

void Es7111AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) return;

    // I2S RX 始终是 TDM 模式（duplex 共享控制器）
    // 喇叭模式：ES7210 ADC 数据通过 TDM 4ch 进来
    // 耳机模式：耳机 mic 通过 USB_SW 模拟开关接到 I2S DIN，映射到 TDM slot0
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)
                          | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_LOGI(TAG, "EnableInput: 4ch TDM, sr=%d gain=%.0f headset=%d",
                 input_sample_rate_, input_gain_, headset_mode_);
        if (input_dev_) {
            ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
            ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(input_dev_,
                ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3), input_gain_));
        }
    } else {
        if (input_dev_) {
            ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
        }
    }
    AudioCodec::EnableInput(enable);
}

void Es7111AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) return;

    // ES7111 无 I2C 控制，直接开关 PA
    if (pa_pin_ != GPIO_NUM_NC) {
        gpio_set_level(pa_pin_, enable ? 1 : 0);
    }
    AudioCodec::EnableOutput(enable);
}

int Es7111AudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || !input_dev_) return samples;

    // 统一走 ES7210 codec dev 读 4ch TDM，提取 2ch
    // 耳机模式下 USB_SW 已切换，耳机 mic 映射到 TDM slot0 (ch0)
    int frames = samples / input_channels_;
    int tdm_samples = frames * 4;
    if ((int)tdm_buf_.size() < tdm_samples) {
        tdm_buf_.resize(tdm_samples);
    }
    int ret = esp_codec_dev_read(input_dev_, tdm_buf_.data(), tdm_samples * sizeof(int16_t));
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_read failed: %d", ret);
        return 0;
    }
    if (input_reference_) {
        // 有回声参考：2ch 输出 (ch0=mic, ch2=reference)
        for (int f = 0; f < frames; f++) {
            dest[f * 2]     = tdm_buf_[f * 4 + 0];
            dest[f * 2 + 1] = tdm_buf_[f * 4 + 2];
        }
    } else {
        // 双麦混合成单路：(ch0 + ch2) / 2，提升信噪比
        for (int f = 0; f < frames; f++) {
            int32_t mixed = ((int32_t)tdm_buf_[f * 4 + 0] + (int32_t)tdm_buf_[f * 4 + 2]) / 2;
            dest[f] = (int16_t)mixed;
        }
    }
    // 每 2 秒诊断
    static int read_count = 0;
    if (++read_count % 200 == 0) {
        int16_t ch_max[4] = {0};
        for (int f = 0; f < frames; f++) {
            for (int c = 0; c < 4; c++) {
                int16_t v = tdm_buf_[f * 4 + c];
                if (abs(v) > abs(ch_max[c])) ch_max[c] = v;
            }
        }
        ESP_LOGW(TAG, "AudioIn(%s): frames=%d ch_peak=[%d,%d,%d,%d]",
                 headset_mode_ ? "headset" : "speaker",
                 frames, ch_max[0], ch_max[1], ch_max[2], ch_max[3]);
    }
    return samples;
}

int Es7111AudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        // mono→stereo: ES7111 mono DAC，I2S stereo 配置，数据放左声道
        // 耳机模式增益 2x 补偿（耳机阻抗高，需要更大驱动）
        std::vector<int16_t> stereo(samples * 2);
        int32_t vol_factor = static_cast<int32_t>(pow(output_volume_ / 100.0, 2) * 256);
        if (headset_mode_) vol_factor *= 2; // 耳机增益补偿
        for (int i = 0; i < samples; i++) {
            int32_t val = (static_cast<int32_t>(data[i]) * vol_factor) >> 8;
            // 软限幅防爆音
            if (val > INT16_MAX) val = INT16_MAX;
            if (val < INT16_MIN) val = INT16_MIN;
            stereo[i * 2] = static_cast<int16_t>(val);
            stereo[i * 2 + 1] = 0;
        }
        size_t bytes_written;
        i2s_channel_write(tx_handle_, stereo.data(), stereo.size() * sizeof(int16_t),
                          &bytes_written, pdMS_TO_TICKS(500));
        return samples;
    }
    return samples;
}
