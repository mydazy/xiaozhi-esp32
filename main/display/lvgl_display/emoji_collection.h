#ifndef EMOJI_COLLECTION_H
#define EMOJI_COLLECTION_H

#include "lvgl_image.h"

#include <lvgl.h>

#include <map>
#include <string>
#include <memory>


// Define interface for emoji collection
class EmojiCollection {
public:
    virtual void AddEmoji(const std::string& name, LvglImage* image);
    // 替换或新增（已存在则释放旧 LvglImage）。用于动态注入 PSRAM GIF（如识字笔画）。
    virtual void ReplaceEmoji(const std::string& name, LvglImage* image);
    virtual const LvglImage* GetEmojiImage(const char* name);
    virtual ~EmojiCollection();

private:
    std::map<std::string, LvglImage*> emoji_collection_;
};

class Twemoji32 : public EmojiCollection {
public:
    Twemoji32();
};

class Twemoji64 : public EmojiCollection {
public:
    Twemoji64();
};

#endif
