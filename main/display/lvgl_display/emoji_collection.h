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
    virtual const LvglImage* GetEmojiImage(const char* name);
    virtual ~EmojiCollection();

private:
    std::map<std::string, LvglImage*> emoji_collection_;
};

// Twemoji32/Twemoji64 内嵌 emoji 字体子类已删除：
// 项目用 DEFAULT_EMOJI_COLLECTION=twemoji_240（PNG 资产挂在 assets 分区），不使用嵌入式 emoji。
// 保留这两个子类会让 LD 在同 .o 链接时拉入全部 42 个 emoji_*_32/64，固件膨胀 ~1.27MB。
// 未来如需嵌入式 emoji 字体，从上游 xiaozhi-esp32 同步回来即可。

#endif
