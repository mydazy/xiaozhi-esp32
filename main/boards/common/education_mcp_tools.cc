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

// 设计决策：show_stroke GIF 不缓存。
//   理由：① GIF buffer 与 LVGL GIF decoder 异步渲染绑定，缓存 buffer 跨 LvglRawImage 实例生命周期复杂
//        ② PSRAM 占用收益不对等（缓存 1MB vs 重下 200-2000ms · 量产期优先稳定性）
//        ③ 单字 GIF 服务器侧可能更新笔顺修正，每次下载保证最新
//   每次调用都走完整 HTTP 下载 + 校验路径。

void RegisterEducationMcpTools(McpServer& mcp, UiDisplay* ui, bool include_stroke) {
    if (!ui) return;  // 仅 UiDisplay 支持 GIF 动态注入（emoji_collection + LvglGif 路径）

    if (include_stroke) {
    mcp.AddTool("self.education.show_stroke",
        "[识字写字核心工具 · 高优先级] 在屏幕上播放单个汉字的笔画书写 GIF 动画,孩子边看边学笔顺。\n"
        "\n"
        "## 必须调用的场景(听到下列任一类表达就立即调用,不要先问、不要犹豫):\n"
        "1) 直接问写法: 怎么写 / X怎么写 / X字怎么写 / 写法 / 笔顺 / 笔画 / 先写哪一笔 / 第一笔是什么\n"
        "2) 教学请求: 教我写 / 教我X / 教写字 / 学写字 / 我想学写字 / 学个新字 / 学X字 / 陪我练字 / 练字\n"
        "3) 表达困惑: 我不会写 / 我不会写X / 妈妈我不会 / X字我不会 / X怎么写呀 / 帮我写\n"
        "4) 看示范: 示范 / 演示 / 给我看看怎么写 / 给我画 / 写给我看 / 一起写X\n"
        "5) 间接表达: 我想看X字 / 我们写X吧 / 教孩子写X / X字长什么样 / 学习X字\n"
        "\n"
        "## 禁止调用的场景(用别的方式回复):\n"
        "× '怎么读 / X怎么读 / 读音 / 拼音' -> 不调本工具,用 [X_pīnyīn] 教育卡内联\n"
        "× '是什么字 / 这是什么 / 那个字是啥' -> 不调本工具,用 [X_pīnyīn] 识字卡\n"
        "× '什么意思 / X 是什么意思' -> 不调本工具,口语解释 + [X_pīnyīn] 内联\n"
        "× 算式 / 古诗 / 闲聊 / 故事 -> 完全不涉及\n"
        "\n"
        "## character 参数取字规则:\n"
        "- 单字明确: '好字怎么写' -> '好'; '写个山字' -> '山'\n"
        "- 多字句子取核心字(用户最关注/最难/最后提到的): '我不会写花字' -> '花'\n"
        "- 不能识别具体字时: 先用 TTS 反问'你想学哪个字?',不要瞎调\n"
        "- 严格单字,GB2312 常用字范围(U+4E00~U+9FFF)\n"
        "\n"
        "## 调用示例(对话 -> 工具调用 + 口播):\n"
        "孩子'好字怎么写?'\n"
        "  -> show_stroke('好'); 口播'好,看我画给你看。先写女字旁,再写子。组词:好吃、好看。'\n"
        "孩子'妈妈我不会写花字'\n"
        "  -> show_stroke('花'); 口播'别担心我教你。花字上面草字头,下面化。组词:花朵、鲜花。'\n"
        "孩子'教我学写字吧'\n"
        "  -> 不调,先反问'好呀,你想学哪个字?'\n"
        "孩子'山字怎么读?'\n"
        "  -> 不调 show_stroke,回复'是 [山_shān] 哦'\n"
        "\n"
        "## 同轮互斥(重要):\n"
        "本轮调用了 show_stroke -> 本轮 TTS 文本不要再出现 [main_top] 教育卡标记,会画面冲突。\n"
        "\n"
        "## 即时反馈(GIF 下载 1-2s):\n"
        "调用后立刻口播'好,看我画给你看' 让用户在加载期间有反馈,GIF 出现后再讲笔顺要点。\n"
        "全部回复 <=3 句,语音友好,不超过 30 字一句。",
        PropertyList({Property("character", kPropertyTypeString)}),
        [ui](const PropertyList& properties) -> ReturnValue {
            std::string character = properties["character"].value<std::string>();
            if (character.empty()) return std::string("ERR: character is empty");

            // UTF-8 → Unicode 码点
            const unsigned char* p = (const unsigned char*)character.c_str();
            uint32_t unicode = 0;
            int char_len = 0;
            if ((p[0] & 0x80) == 0) {
                return std::string("ERR: character must be a Chinese hanzi");
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
                return std::string("ERR: invalid UTF-8 encoding");
            }
            if (unicode < 0x4E00 || unicode > 0x9FFF) {
                return std::string("ERR: not a common Chinese hanzi (CJK Unified Ideographs)");
            }
            // 严格单字：调用方传"好字"应只取首字，多字截断会让缓存命中乱
            // 取首字后剩余字节数应为 0
            if ((int)character.length() != char_len) {
                ESP_LOGW(TAG, "show_stroke: 多字截断 '%s' → 首字 (len=%d, total=%u)",
                         character.c_str(), char_len, (unsigned)character.length());
                character = character.substr(0, char_len);
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

            // 入口立即翻 pending 守护位（block ShowEduCard 闪屏）+ 拿并发去重 token
            // 同轮内若 LLM 连发两次 show_stroke → 旧 token 自动作废，新下载覆盖
            uint32_t req_id = ui->BeginFontPending();
            ESP_LOGI(TAG, "show_stroke: U+%s req=%u URL=%s",
                     unicode_hex, (unsigned)req_id, url_buf);

            // 后台 PSRAM 任务异步下载（不阻塞 mcp 调用方）
            // ctx 带上 character 用于失败兜底（404 / 弱网 / GIF 损坏 → 弹大字卡让用户至少看到字）
            struct StrokeCtx {
                UiDisplay* ui;
                std::string url;
                std::string character;
                uint32_t req_id;
            };
            auto* ctx = new StrokeCtx{ui, std::string(url_buf), character, req_id};
            xTaskCreatePinnedToCoreWithCaps([](void* arg) {
                auto* ctx = static_cast<StrokeCtx*>(arg);
                auto* d = ctx->ui;
                const auto& url = ctx->url;
                std::string fallback_char = ctx->character;   // 失败兜底用
                uint32_t req_id = ctx->req_id;
                constexpr size_t kMaxGifSize = 512 * 1024;  // 控制单字 GIF ≤ 512KB

                int64_t t_start = esp_timer_get_time();
                uint8_t* gif = nullptr;
                size_t gsz = 0;
                bool ok = Ota::Download(url, kMaxGifSize, &gif, &gsz);
                int elapsed_ms = (int)((esp_timer_get_time() - t_start) / 1000);
                int speed_kbs = (elapsed_ms > 0)
                    ? (int)((gsz * 1000ULL) / (size_t)elapsed_ms / 1024)
                    : 0;

                // GIF 完整性校验（防 LVGL 解码崩溃 / 服务器返回 HTML 错误页 / 截断数据）：
                //   ① 下载成功 ok
                //   ② size 至少 1KB（笔画 GIF 实测 ≥30KB · 1KB 兜底拒绝空响应/4xx 错误页）
                //   ③ 头 6 字节 GIF89a/GIF87a magic
                //   ④ 末字节 0x3B = GIF Trailer（保证下载完整，未被中途截断）
                constexpr size_t kMinValidGifSize = 1024;
                bool valid = ok && gsz >= kMinValidGifSize &&
                    (memcmp(gif, "GIF89a", 6) == 0 || memcmp(gif, "GIF87a", 6) == 0) &&
                    gif[gsz - 1] == 0x3B;
                if (!valid) {
                    ESP_LOGW(TAG, "show_stroke: GIF 校验失败 bytes=%u elapsed=%dms speed=%dKB/s req=%u → 兜底弹大字卡",
                             (unsigned)gsz, elapsed_ms, speed_kbs, (unsigned)req_id);
                    if (gif) heap_caps_free(gif);
                    // 兜底：在主线程弹"大字 + 空 top"的 EduCard。
                    // 防呆：① state 必须仍是 Speaking ② 释放 pending 让卡能弹出
                    Application::GetInstance().Schedule([d, fallback_char, req_id]() {
                        d->CancelFontPending(req_id);  // 必须先释放，否则 ShowEduCard 被 pending 屏蔽
                        if (Application::GetInstance().GetDeviceState() != kDeviceStateSpeaking) return;
                        ESP_LOGI(TAG, "show_stroke fallback: ShowEduCard(\"%s\", \"\")", fallback_char.c_str());
                        d->ShowEduCard(fallback_char.c_str(), "");
                    });
                    delete ctx;
                    vTaskDelete(NULL);
                    return;
                }
                ESP_LOGI(TAG, "show_stroke: download done bytes=%u elapsed=%dms speed=%dKB/s req=%u",
                         (unsigned)gsz, elapsed_ms, speed_kbs, (unsigned)req_id);

                Application::GetInstance().Schedule([d, gif, gsz, req_id, fallback_char]() {
                    // 主线程二次守护：只在 Speaking 时装载 GIF（用户已退出 → buffer 丢弃）
                    // 同时只允许最新 token 装载（FontGif 内部再 dedup）
                    if (Application::GetInstance().GetDeviceState() != kDeviceStateSpeaking) {
                        ESP_LOGI(TAG, "show_stroke: state changed, drop GIF req=%u", (unsigned)req_id);
                        heap_caps_free(gif);
                        d->CancelFontPending(req_id);
                        return;
                    }
                    d->FontGif(gif, gsz, req_id);   // PSRAM buffer 所有权转移给 LvglRawImage
                });
                delete ctx;
                vTaskDelete(NULL);
            }, "stroke_dl", 8192, ctx, 1, nullptr, 0, MALLOC_CAP_SPIRAM);

            return std::string("OK: stroke loading");
        });
    }  // include_stroke

    // 教育卡渲染（极简 · 两行布局 · 不分类）
    //   main 自动判定 CJK/英文 · 字号 56 默认 / 48 EN 兜底
    //   top  顶部辅助：汉字配拼音 / 英文配中文释义
    //   优先级 < TTS 内联 [main_top]：本工具作"主动调用兜底"，常规场景应直接在 TTS 嵌入标记
    mcp.AddTool("self.education.show_card",
        "[兜底用 · 优先用 TTS 内联 [main_top]] 显示教育卡片(两行布局)。\n"
        "汉字组词: main=\"书包\" top=\"shū bāo\" (汉字 <=4 字, 拼音必带声调 ǎēīǒūǚ)\n"
        "英文单词: main=\"apple\" top=\"苹果\" (英文 <=12 字符)\n"
        "返回 ERR: ... 时表示参数超限,LLM 应改用更短的词重试。",
        PropertyList({
            Property("main", kPropertyTypeString),
            Property("top",  kPropertyTypeString, std::string("")),
        }),
        [ui](const PropertyList& properties) -> ReturnValue {
            std::string main_t = properties["main"].value<std::string>();
            std::string top    = properties["top"].value<std::string>();
            if (main_t.empty()) return std::string("ERR: main is empty");
            // 粗略字节数兜底：汉字 4 字 = 12 bytes / 英文 12 字 = 12 bytes，统一上限 16 bytes 留余量
            // 真正的字符集 + 布局判定由 UiDisplay::ShowEduCard 内 PickFont 完成（超范围会静默跳过）
            if (main_t.size() > 16) {
                return std::string("ERR: main too long (max 4 CJK or 12 ASCII chars)");
            }
            if (top.size() > 32) {  // 拼音 4 字 = ~20 bytes，留余量
                return std::string("ERR: top too long");
            }
            Application::GetInstance().Schedule([ui, main_t, top]() {
                ui->ShowEduCard(main_t.c_str(), top.c_str());
            });
            return std::string("OK");
        });
}
