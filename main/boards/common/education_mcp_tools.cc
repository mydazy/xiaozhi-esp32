#include "education_mcp_tools.h"

#include "mcp_server.h"
#include "application.h"
#include "ota.h"
#include "display/ui_display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <cstring>
#include <string>
#include <utility>

#define TAG "EducationMcp"

void RegisterEducationMcpTools(McpServer& mcp, UiDisplay* ui) {
    if (!ui) return;  // 仅 UiDisplay 支持 GIF 动态注入（emoji_collection + LvglGif 路径）

    mcp.AddTool("self.education.show_stroke",
        "显示汉字笔画书写动画。"
        "当用户提到任何字的写法时必须调用，包括：怎么写、笔画、笔顺、写法、"
        "写个X字、教我写、写一下、拼写、练字、学写字。"
        "character=单个汉字,多字取首个。"
        "调用后先说'好的'，再简要说明笔顺，最后给出两个热门组词。"
        "回复要简短，适合语音播报，不超过3句话。",
        PropertyList({Property("character", kPropertyTypeString)}),
        [ui](const PropertyList& properties) -> ReturnValue {
            std::string character = properties["character"].value<std::string>();
            if (character.empty()) return std::string("请输入一个汉字");

            // UTF-8 → Unicode 码点
            const unsigned char* p = (const unsigned char*)character.c_str();
            uint32_t unicode = 0;
            int char_len = 0;
            if ((p[0] & 0x80) == 0) {
                return std::string("请输入中文汉字");
            } else if ((p[0] & 0xE0) == 0xC0 && character.length() >= 2) {
                unicode = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
                char_len = 2;
            } else if ((p[0] & 0xF0) == 0xE0 && character.length() >= 3) {
                unicode = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                char_len = 3;
            } else if ((p[0] & 0xF8) == 0xF0 && character.length() >= 4) {
                unicode = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
                char_len = 4;
            } else {
                return std::string("无法识别的字符编码");
            }
            if (unicode < 0x4E00 || unicode > 0x9FFF) {
                return std::string("请输入常用汉字");
            }

            // URL 拼接：https://aiagent.bj.bcebos.com/dict/stroke/<percent_encoded_utf8>_<unicode_hex>.gif
            char unicode_hex[8];
            snprintf(unicode_hex, sizeof(unicode_hex), "%04X", (unsigned)unicode);
            std::string encoded_char;
            for (int i = 0; i < char_len; i++) {
                char hex[4];
                snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)character[i]);
                encoded_char += hex;
            }
            char url_buf[256];
            snprintf(url_buf, sizeof(url_buf),
                     "https://aiagent.bj.bcebos.com/dict/stroke/%s_%s.gif",
                     encoded_char.c_str(), unicode_hex);
            ESP_LOGI(TAG, "show_stroke: U+%s URL=%s", unicode_hex, url_buf);

            // 后台 PSRAM 任务异步下载（不阻塞 mcp 调用方 + 与 LLM/TTS 并行）
            auto* ctx = new std::pair<UiDisplay*, std::string>(ui, std::string(url_buf));
            xTaskCreatePinnedToCoreWithCaps([](void* arg) {
                auto* ctx = static_cast<std::pair<UiDisplay*, std::string>*>(arg);
                auto* d = ctx->first;
                const auto& url = ctx->second;
                constexpr size_t kMaxGifSize = 512 * 1024;  // 4G 带宽下控制单字 GIF ≤ 512KB

                uint8_t* gif = nullptr;
                size_t gsz = 0;
                bool ok = Ota::Download(url, kMaxGifSize, &gif, &gsz);

                // GIF 完整性校验（防止 LVGL 解码器崩溃 / 服务器返回 HTML 错误页等）
                bool valid = ok && gsz >= 14 &&
                    (memcmp(gif, "GIF89a", 6) == 0 || memcmp(gif, "GIF87a", 6) == 0) &&
                    gif[gsz - 1] == 0x3B;
                if (!valid) {
                    ESP_LOGW(TAG, "show_stroke: GIF 校验失败（该字可能无笔画动画）");
                    if (gif) heap_caps_free(gif);
                    delete ctx;
                    vTaskDelete(NULL);
                    return;
                }

                // 切回 main 任务：UpdateFontGif 内部加 DisplayLockGuard，与 LVGL 渲染线程互斥
                Application::GetInstance().Schedule([d, gif, gsz]() {
                    d->UpdateFontGif(gif, gsz);   // PSRAM buffer 所有权转移给 EmojiCollection
                    d->SetEmotion("font");
                });
                delete ctx;
                vTaskDelete(NULL);
            }, "stroke_dl", 8192, ctx, 1, nullptr, 0, MALLOC_CAP_SPIRAM);

            return std::string("OK,正在加载笔画动画");
        });

    // 教育卡（统一接口，category 切换排版）— overlay 显示，触屏点击退出
    // 三行布局：[top 30px 浅橙] + main 48px 金黄 + bottom 48px 绿色（CJK fallback 30px）
    //
    // category 必填："word" | "hanzi" | "pinyin"
    //   word   英文单词：top=自然拼读 / main=英文 / bottom=中文释义
    //   hanzi  汉字注音：top=拼音(带声调) / main=汉字 / bottom=组词       ★ "X字怎么读/拼音"用此
    //   pinyin 拼音教学：top=类别 / main=声韵母字母 / bottom=例字          ★ 仅"学韵母/声母/整体认读"用此
    mcp.AddTool("self.education.show_card",
        "显示小学教育学习卡片(英文单词/汉字注音/拼音 三选一)。"
        "用户问字词意思/怎么读/怎么写/拼写/spell/X字组词/学拼音 时调用。"
        ""
        "★类别选择路由（重要！按用户问题决定 category）★"
        "用户问'X字怎么读/拼音'→category=hanzi(top=拼音 main=汉字 bottom=组词);"
        "用户问'X字组词'→category=hanzi(top=拼音 main=汉字 bottom=组词);"
        "用户问'X单词意思/怎么拼/spell'→category=word(top=自然拼读 main=英文 bottom=中文);"
        "用户问'学韵母/学声母/整体认读/拼读'→category=pinyin(top=类别 main=带声调字母 bottom=例字);"
        ""
        "category=类别('word'英文|'hanzi'汉字|'pinyin'声韵母教学);"
        ""
        "top=上行标注 30px 浅橙(hanzi/pinyin 必填，word 选填):"
            "★word填自然拼读≤12字符如\"c-a-t\"\"ban-an-a\"，禁止 IPA 国际音标;"
            "★hanzi填拼音带声调≤6字符如\"niǎo\"\"hǎo\"\"nǐ hǎo\"，必须用ǎēīǒū等字母;"
            "★pinyin填类别如\"韵母\"\"声母\"\"整体认读\"≤4汉字;"
        ""
        "main=主体大字 48px 金黄:"
            "★word填英文≤9字母全小写如\"apple\";"
            "★hanzi填汉字单字或词组≤4字如\"鸟\"\"好\"\"你好\";"
            "★pinyin填带声调拼音字母≤8字符如\"ang\"\"b\"\"ying\";"
        ""
        "bottom=下行说明 48px 绿色 ≤4 字(可空):"
            "★word填中文释义如\"苹果\";"
            "★hanzi填组词或释义如\"小鸟\"\"美好\"\"问候\";"
            "★pinyin填例字如\"昂浪\"\"波\"\"英应\";"
        ""
        "字符上限严格遵守，超长会被裁切。"
        "拼音必须用带声调字母 ā á ǎ à ē é ě è ī í ǐ ì ō ó ǒ ò ū ú ǔ ù ǖ ǘ ǚ ǜ，"
        "禁用不带声调如\"hao\"，禁用文字描述声调如\"三声\"。"
        "调用后用语气词回应+朗读 main 带声调发音+说 bottom+1 句巩固。"
        "回复温暖简短，适合 3-10 岁儿童语音播报，≤3 句。",
        PropertyList({
            Property("category", kPropertyTypeString),
            Property("main",     kPropertyTypeString),
            Property("top",      kPropertyTypeString, std::string("")),
            Property("bottom",   kPropertyTypeString, std::string("")),
        }),
        [ui](const PropertyList& properties) -> ReturnValue {
            std::string category = properties["category"].value<std::string>();
            std::string main_t   = properties["main"].value<std::string>();
            std::string top      = properties["top"].value<std::string>();
            std::string bottom   = properties["bottom"].value<std::string>();
            if (main_t.empty()) return std::string("请输入要展示的内容");
            if (category != "word" && category != "hanzi" && category != "pinyin") {
                return std::string("category 仅支持 word/hanzi/pinyin");
            }

            // 切回 main 任务（与 LVGL 渲染线程互斥，DisplayLockGuard 在 ShowEduCard 内）
            Application::GetInstance().Schedule([ui, category, main_t, top, bottom]() {
                ui->ShowEduCard(category.c_str(), main_t.c_str(),
                                top.c_str(), bottom.c_str());
            });
            return std::string("OK");
        });
}
