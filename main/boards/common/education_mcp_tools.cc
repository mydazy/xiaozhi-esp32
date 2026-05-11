#include "education_mcp_tools.h"

#include "mcp_server.h"
#include "application.h"
#include "ota.h"
#include "display/ui_display.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <cstring>
#include <string>
#include <utility>

#define TAG "EducationMcp"

void RegisterEducationMcpTools(McpServer& mcp, UiDisplay* ui, bool include_stroke) {
    if (!ui) return;  // 仅 UiDisplay 支持 GIF 动态注入（emoji_collection + LvglGif 路径）

    if (include_stroke) {
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

            // 后台 PSRAM 任务异步下载（不阻塞 mcp 调用方）
            auto* ctx = new std::pair<UiDisplay*, std::string>(ui, std::string(url_buf));
            xTaskCreatePinnedToCoreWithCaps([](void* arg) {
                auto* ctx = static_cast<std::pair<UiDisplay*, std::string>*>(arg);
                auto* d = ctx->first;
                const auto& url = ctx->second;
                constexpr size_t kMaxGifSize = 512 * 1024;  // 控制单字 GIF ≤ 512KB

                int64_t t_start = esp_timer_get_time();
                uint8_t* gif = nullptr;
                size_t gsz = 0;
                bool ok = Ota::Download(url, kMaxGifSize, &gif, &gsz);
                int elapsed_ms = (int)((esp_timer_get_time() - t_start) / 1000);
                int speed_kbs = (elapsed_ms > 0)
                    ? (int)((gsz * 1000ULL) / (size_t)elapsed_ms / 1024)
                    : 0;

                // GIF 完整性校验（防止 LVGL 解码器崩溃 / 服务器返回 HTML 错误页等）
                bool valid = ok && gsz >= 14 &&
                    (memcmp(gif, "GIF89a", 6) == 0 || memcmp(gif, "GIF87a", 6) == 0) &&
                    gif[gsz - 1] == 0x3B;
                if (!valid) {
                    ESP_LOGW(TAG, "show_stroke: GIF 校验失败 bytes=%u elapsed=%dms speed=%dKB/s（该字可能无笔画动画）",
                             (unsigned)gsz, elapsed_ms, speed_kbs);
                    if (gif) heap_caps_free(gif);
                    delete ctx;
                    vTaskDelete(NULL);
                    return;
                }
                ESP_LOGI(TAG, "show_stroke: download done bytes=%u elapsed=%dms speed=%dKB/s, replacing GIF",
                         (unsigned)gsz, elapsed_ms, speed_kbs);

                Application::GetInstance().Schedule([d, gif, gsz]() {
                    d->FontGif(gif, gsz);   // PSRAM buffer 所有权转移给 EmojiCollection
                });
                delete ctx;
                vTaskDelete(NULL);
            }, "stroke_dl", 8192, ctx, 1, nullptr, 0, MALLOC_CAP_SPIRAM);

            return std::string("OK,正在加载笔画动画");
        });
    }  // include_stroke

    // 教育卡 P0：动态切换 LLM system prompt（参考 BRTC SDK update_vision_prompt 协议）
    // 三档专属 prompt 在云端按 mode 路由；本地仅触发协议层 [SET]:[UPDATE_SYSTEM_PROMPT]:
    //   word    → 英文识字模式（自然拼读 + 中英对照 + 音标）
    //   hanzi   → 汉字识字模式（拼音 + 笔画 + 组词，配合 show_stroke + show_card）
    //   pinyin  → 拼音教学模式（韵母 / 声母 / 整体认读）
    //   reset   → 恢复默认 chat 人设
    // 之所以让 LLM 自己调用而不是 set_mode 简单参数：让云端 prompt 库统一管控、设备只发触发器
    mcp.AddTool("self.education.set_mode",
        "切换教学专属 system prompt(教育卡 P0)。"
        "用户提到学英语/学汉字/学拼音/教学模式时调用。"
        "mode=word: 英文识字; hanzi: 汉字识字; pinyin: 拼音; reset: 退出教学回普通聊天。"
        "调用后简短回应'好,进入XX模式'即可。",
        PropertyList({Property("mode", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            std::string mode = properties["mode"].value<std::string>();
            // 本地仅做"切档触发器" — 真正 prompt 文本走云端模板（统一可调可灰度）
            // 协议层 model_type 约定: 2=视觉理解槽 / 0=聊天槽；教学走 2 槽避免污染默认人设
            int model_type = (mode == "reset") ? 0 : 2;
            std::string prompt;  // 留空 = 由云端按 mode 字段路由 prompt 库
            if (mode != "word" && mode != "hanzi" && mode != "pinyin" && mode != "reset") {
                return std::string("mode 仅支持 word/hanzi/pinyin/reset");
            }
            // 拼一个最小 trigger（云端 update_system_prompt handler 解析 mode 字段路由模板）
            prompt = std::string("{\"edu_mode\":\"") + mode + "\"}";
            bool ok = Application::GetInstance().UpdateSystemPrompt(model_type, prompt);
            return std::string(ok ? "OK" : "切换失败");
        });

    // 教育卡渲染（9 类 category · 主动 letter/phonics/math 走 88px / 其他 56px）
    mcp.AddTool("self.education.show_card",
        "渲染教育卡片(九选一)，需要可视化展示字词/拼音/算式/古诗/科普时调用：\n"
        "【被动 56 主秀】\n"
        " word: top=自然拼读\"ap·ple\"(选填) / main=英文≤10 字母 / bottom=中文释义≤10 字；\n"
        " hanzi: top=带声调拼音\"niǎo\"(必填) / main=汉字≤4 字 / bottom=组词≤10 字；\n"
        " pinyin: top=类别\"韵母\"(必填) / main=带调字母≤8 字符 / bottom=例字≤10 字；\n"
        " poem: top=拼音(选填) / main=诗题≤4 字 / bottom=作者朝代；\n"
        " topic: top=拼音(选填) / main=主题词≤4 字 / bottom=类别(如\"昆虫\")；\n"
        " color: top=英文如\"red\"(选填) / main=中文颜色≤2 字 / bottom=英文翻译；\n"
        "【主动 88 超大主秀】\n"
        " letter: main=字母\"Aa\"(大小写并排≤5 字符) / bottom=首例词中文；\n"
        " phonics: top=类别\"声母\"(必填) / main=单声母\"b\"(≤5 字符) / bottom=例字\"爸 bà\"；\n"
        " math: main=算式\"3+5=8\"(≤5 字符) / bottom=读法\"三加五等于八\"。\n"
        "拼音必须用 ǎēīǒūǚ 等带声调字母，禁用\"hao\"无声调或\"三声\"文字描述。",
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
            // 9 类白名单：被动 6（56px） + 主动 3（88px）
            static const char* kAllowed[] = {
                "word", "hanzi", "pinyin",
                "poem", "topic", "color",
                "letter", "phonics", "math"
            };
            bool ok = false;
            for (const char* c : kAllowed) {
                if (category == c) { ok = true; break; }
            }
            if (!ok) {
                return std::string(
                    "category 仅支持 word/hanzi/pinyin/poem/topic/color/letter/phonics/math");
            }

            // 切回 main 任务（与 LVGL 渲染线程互斥，DisplayLockGuard 在 ShowEduCard 内）
            Application::GetInstance().Schedule([ui, category, main_t, top, bottom]() {
                ui->ShowEduCard(category.c_str(), main_t.c_str(),
                                top.c_str(), bottom.c_str());
            });
            return std::string("OK");
        });
}
