#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <cmath>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    aec_gain_db_     = settings.GetFloat("aec_gain", aec_gain_db_);
    aec_gain_linear_ = powf(10.0f, aec_gain_db_ / 20.0f);

    ESP_LOGI(TAG, "Audio codec started · AEC gain=%.1fdB(×%.2f)", aec_gain_db_, aec_gain_linear_);
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "MIC增益=%.1fdB", input_gain_);

    Settings settings("audio", true);
    settings.SetFloat("input_gain", input_gain_);
}

void AudioCodec::SetAecGain(float db) {
    aec_gain_db_     = db;
    aec_gain_linear_ = powf(10.0f, db / 20.0f);
    ESP_LOGI(TAG, "AEC增益=%.1fdB(×%.2f)", db, aec_gain_linear_);

    Settings settings("audio", true);
    settings.SetFloat("aec_gain", db);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
