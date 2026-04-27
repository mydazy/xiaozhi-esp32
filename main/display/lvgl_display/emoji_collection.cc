#include "emoji_collection.h"

#include <esp_log.h>
#include <unordered_map>
#include <string>

#define TAG "EmojiCollection"

void EmojiCollection::AddEmoji(const std::string& name, LvglImage* image) {
    emoji_collection_[name] = image;
}

const LvglImage* EmojiCollection::GetEmojiImage(const char* name) {
    auto it = emoji_collection_.find(name);
    if (it != emoji_collection_.end()) {
        return it->second;
    }

    ESP_LOGW(TAG, "Emoji not found: %s", name);
    return nullptr;
}

EmojiCollection::~EmojiCollection() {
    for (auto it = emoji_collection_.begin(); it != emoji_collection_.end(); ++it) {
        delete it->second;
    }
    emoji_collection_.clear();
}

// Twemoji32 / Twemoji64 内嵌字体子类已删除（详见 emoji_collection.h 注释）。
// 项目使用 twemoji_240 PNG 资产模式（assets 分区），保留这些子类会让 LD 链接全部
// 42 个 emoji_*_32/64 数据 → 固件膨胀 ~1.27MB。dead code 死代码，删除。
