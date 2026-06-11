#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <string>
#include <vector>
#include <functional>

#include <model_path.h>
#include "audio_codec.h"

class WakeWord {
public:
    virtual ~WakeWord() = default;
    
    virtual bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) = 0;
    virtual void Feed(const std::vector<int16_t>& data) = 0;
    virtual void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual size_t GetFeedSize() = 0;
    virtual void EncodeWakeWordData() = 0;
    virtual bool GetWakeWordOpus(std::vector<uint8_t>& opus) = 0;
    virtual const std::string& GetLastDetectedWakeWord() const = 0;
    virtual void Release() {}
    // 运行时调整检测灵敏度（越低越灵敏，esp-sr 合法范围 0.4~0.9999）。
    // 用途：TTS 播放期调低换打断检出率，回待机恢复防误唤醒。不支持的引擎忽略。
    virtual void SetDetectThreshold(float threshold) { (void)threshold; }
};

#endif
