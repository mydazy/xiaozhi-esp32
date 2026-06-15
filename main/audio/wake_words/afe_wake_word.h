#ifndef AFE_WAKE_WORD_H
#define AFE_WAKE_WORD_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

#include <esp_afe_sr_models.h>
#include <esp_nsn_models.h>
#include <model_path.h>

#include <deque>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "audio_codec.h"
#include "wake_word.h"

class AfeWakeWord : public WakeWord {
public:
    AfeWakeWord();
    ~AfeWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list);
    void Feed(const std::vector<int16_t>& data);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback);
    void Start();
    void Stop();
    void Release();
    bool IsInitialized() const { return afe_data_ != nullptr; }
    size_t GetFeedSize();
    void EncodeWakeWordData();
    bool GetWakeWordOpus(std::vector<uint8_t>& opus);
    const std::string& GetLastDetectedWakeWord() const { return last_detected_wake_word_; }
    void SetDetectThreshold(float threshold) override;

private:
    srmodel_list_t *models_ = nullptr;
    bool owns_models_ = false;   // true=自己 esp_srmodel_init 出来的，析构才 deinit；传入的共享列表不归我管
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    char* wakenet_model_ = NULL;
    int wn_model_count_ = 0;   // 加载的 wakenet 模型数（set_wakenet_threshold 索引 1..N）
    std::vector<std::string> wake_words_;
    EventGroupHandle_t event_group_;
    SemaphoreHandle_t detection_done_sem_ = nullptr;
    bool detection_task_created_ = false;
    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    AudioCodec* codec_ = nullptr;
    std::string last_detected_wake_word_;
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;

    TaskHandle_t wake_word_encode_task_ = nullptr;
    StaticTask_t* wake_word_encode_task_buffer_ = nullptr;
    StackType_t* wake_word_encode_task_stack_ = nullptr;
    std::atomic<bool> encode_in_progress_{false};
    // 唤醒前 2s 录音环形缓冲（PSRAM 固定块·一次分配终身复用）
    // 历史：std::deque<vector> 默认分配器吃内部 RAM 64K 且 Stop 不清 → 破 60K 红线主因
    static constexpr size_t kWakeWordRingSamples = 32000;  // 2s @ 16kHz mono
    int16_t* wake_word_ring_ = nullptr;                    // PSRAM
    size_t ring_write_ = 0;                                // 下一个写入下标
    size_t ring_filled_ = 0;                               // 有效样本数（≤容量）
    std::mutex wake_word_pcm_mutex_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;

    void StoreWakeWordData(const int16_t* data, size_t size);
    void AudioDetectionTask();
};

#endif
