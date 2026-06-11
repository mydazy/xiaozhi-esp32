#include "afe_wake_word.h"
#include "audio_service.h"
#include <esp_log.h>
#include <sstream>
#include <chrono>
#include <cstring>
#include <algorithm>

#define DETECTION_RUNNING_EVENT 1
#define DETECTION_EXIT_EVENT    2

#define TAG "AfeWakeWord"

AfeWakeWord::AfeWakeWord()
    : afe_data_(nullptr),
      wake_word_opus_() {

    event_group_ = xEventGroupCreate();
    detection_done_sem_ = xSemaphoreCreateBinary();
}

AfeWakeWord::~AfeWakeWord() {
    if (detection_task_created_) {
        xEventGroupSetBits(event_group_, DETECTION_EXIT_EVENT);
        if (detection_done_sem_) {
            xSemaphoreTake(detection_done_sem_, pdMS_TO_TICKS(2000));
        }
    }
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (wake_word_ring_ != nullptr) {
        heap_caps_free(wake_word_ring_);
    }

    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }

    if (detection_done_sem_ != nullptr) {
        vSemaphoreDelete(detection_done_sem_);
    }
    vEventGroupDelete(event_group_);
}

bool AfeWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    int ref_num = codec_->input_reference() ? 1 : 0;

    if (models_list == nullptr) {
        models_ = esp_srmodel_init("model");
    } else {
        models_ = models_list;
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }
    wake_words_.clear();
    wakenet_model_ = NULL;
    wn_model_count_ = 0;
    for (int i = 0; i < models_->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        if (strstr(models_->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model_ = models_->model_name[i];
            wn_model_count_++;
            auto words = esp_srmodel_get_wake_words(models_, wakenet_model_);
            // split by ";" to get all wake words
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                wake_words_.push_back(word);
            }
        }
    }

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models_, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    
    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);
    afe_config_free(afe_config);

    SetDetectThreshold(0.6f);

    xTaskCreatePinnedToCore([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        this_->AudioDetectionTask();
        vTaskDelete(NULL);
    }, "audio_detection", 6144, this, 7, nullptr, 1);
    detection_task_created_ = true;

    return true;
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void AfeWakeWord::SetDetectThreshold(float threshold) {
    if (afe_data_ == nullptr) return;
    threshold = std::clamp(threshold, 0.4f, 0.9999f);  // esp-sr 接口合法范围
    for (int i = 1; i <= wn_model_count_ && i <= 2; i++) {
        int ret = afe_iface_->set_wakenet_threshold(afe_data_, i, threshold);
        ESP_LOGI(TAG, "wakenet%d 阈值=%.2f (ret=%d)", i, threshold, ret);
    }
}

void AfeWakeWord::Start() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
    input_buffer_.clear();
}

void AfeWakeWord::Release() {
    if (detection_task_created_) {
        xEventGroupSetBits(event_group_, DETECTION_EXIT_EVENT);
        if (detection_done_sem_) {
            xSemaphoreTake(detection_done_sem_, pdMS_TO_TICKS(2000));
        }
        detection_task_created_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
        if (afe_data_ != nullptr) {
            afe_iface_->destroy(afe_data_);
            afe_data_ = nullptr;
        }
        input_buffer_.clear();
    }
    xEventGroupClearBits(event_group_, DETECTION_EXIT_EVENT);
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!(xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT)) {
        return;
    }
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    size_t chunk_size = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
    while (input_buffer_.size() >= chunk_size) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

size_t AfeWakeWord::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::AudioDetectionTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_,
            DETECTION_RUNNING_EVENT | DETECTION_EXIT_EVENT, pdFALSE, pdFALSE, portMAX_DELAY);
        if (bits & DETECTION_EXIT_EVENT) {
            break;
        }

        auto res = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
        if (xEventGroupGetBits(event_group_) & DETECTION_EXIT_EVENT) {
            break;
        }
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;
        }

        // Store the wake word data for voice recognition, like who is speaking
        StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));

        if (res->wakeup_state == WAKENET_DETECTED) {
            Stop();
            int wake_idx = res->wakenet_model_index - 1;
            if (wake_idx < 0 || wake_idx >= (int)wake_words_.size()) {
                ESP_LOGW(TAG, "invalid wakenet_model_index %d, skip", res->wakenet_model_index);
                continue;
            }
            last_detected_wake_word_ = wake_words_[wake_idx];

            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
    }
    if (detection_done_sem_ != nullptr) {
        xSemaphoreGive(detection_done_sem_);
    }
}

void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
#if !CONFIG_SEND_WAKE_WORD_DATA
    (void)data; (void)samples;   // 不上送则完全不缓存，零分配
    return;
#else
    std::lock_guard<std::mutex> lock(wake_word_pcm_mutex_);
    if (wake_word_ring_ == nullptr) {
        wake_word_ring_ = (int16_t*)heap_caps_malloc(
            kWakeWordRingSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (wake_word_ring_ == nullptr) {
            ESP_LOGE(TAG, "wake word ring alloc failed (PSRAM)");
            return;
        }
        ESP_LOGI(TAG, "wake ring 64K @%p (0x3c..=PSRAM) · internal free=%u",
                 wake_word_ring_,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    if (samples >= kWakeWordRingSamples) {   // 超长帧只保留最后 2s
        data += samples - kWakeWordRingSamples;
        samples = kWakeWordRingSamples;
    }
    size_t first = std::min(samples, kWakeWordRingSamples - ring_write_);
    memcpy(wake_word_ring_ + ring_write_, data, first * sizeof(int16_t));
    if (samples > first) {
        memcpy(wake_word_ring_, data + first, (samples - first) * sizeof(int16_t));
    }
    ring_write_ = (ring_write_ + samples) % kWakeWordRingSamples;
    ring_filled_ = std::min(ring_filled_ + samples, kWakeWordRingSamples);
#endif
}

void AfeWakeWord::EncodeWakeWordData() {
    if (encode_in_progress_.exchange(true)) {
        ESP_LOGW(TAG, "encode busy · skip this round (双连唤醒丢弃第二段)");
        return;
    }
    const size_t stack_size = 4096 * 6;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStaticPinnedToCore([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            // Create encoder
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                this_->wake_word_cv_.notify_all();
                this_->encode_in_progress_ = false;
                return;
            }
            
            // Get frame size
            int frame_size = 0;
            int outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size = frame_size / sizeof(int16_t);
            
            // Encode all PCM data
            int packets = 0;
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};

            // 环形缓冲快照 → PSRAM 线性段（消费即清空，等价原 deque swap 语义）
            int16_t* snap = nullptr;
            size_t snap_samples = 0;
            {
                std::lock_guard<std::mutex> lock(this_->wake_word_pcm_mutex_);
                snap_samples = this_->ring_filled_;
                if (snap_samples > 0 && this_->wake_word_ring_ != nullptr) {
                    snap = (int16_t*)heap_caps_malloc(snap_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                    if (snap != nullptr) {
                        constexpr size_t cap = kWakeWordRingSamples;
                        size_t start = (this_->ring_write_ + cap - this_->ring_filled_) % cap;
                        size_t first = std::min(snap_samples, cap - start);
                        memcpy(snap, this_->wake_word_ring_ + start, first * sizeof(int16_t));
                        if (snap_samples > first) {
                            memcpy(snap + first, this_->wake_word_ring_, (snap_samples - first) * sizeof(int16_t));
                        }
                    }
                    this_->ring_write_ = 0;
                    this_->ring_filled_ = 0;
                }
            }

            for (size_t off = 0; snap != nullptr && off + (size_t)frame_size <= snap_samples; off += frame_size) {
                std::vector<uint8_t> opus_buf(outbuf_size);
                in.buffer = (uint8_t *)(snap + off);
                in.len = (uint32_t)(frame_size * sizeof(int16_t));
                out.buffer = opus_buf.data();
                out.len = outbuf_size;
                out.encoded_bytes = 0;

                ret = esp_opus_enc_process(encoder_handle, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.emplace_back(opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                    this_->wake_word_cv_.notify_all();
                    packets++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            }
            if (snap != nullptr) {
                heap_caps_free(snap);
            }

            esp_opus_enc_close(encoder_handle);
            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets, (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        this_->encode_in_progress_ = false;
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_, 1);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    if (!wake_word_cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
        return !wake_word_opus_.empty();
    })) {
        ESP_LOGW(TAG, "GetWakeWordOpus timeout(2s), give up this wake upload");
        return false;
    }
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
