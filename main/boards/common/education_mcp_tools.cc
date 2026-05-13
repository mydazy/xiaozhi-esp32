#include "education_mcp_tools.h"

#include "mcp_server.h"
#include "application.h"
#include "ota.h"
#include "display/ui_display.h"
#include "board.h"
#include "dual_network_board.h"  // 4G/WiFi 模式检测 · show_stroke 4G 降级

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

static std::atomic<uint32_t> g_inflight_url_hash_{0};
static std::atomic<int64_t>  g_inflight_start_us_{0};
static std::atomic<uint32_t> g_inflight_req_id_{0};
static constexpr int64_t kInflightWindowUs = 5LL * 1000 * 1000;  // 5s · 超时视为僵尸不去重

// FNV-1a 32-bit hash · URL 平均 80B · 单次 ~80ns
static uint32_t HashUrl(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1;  // 保证非零（0 表示空 slot）
}

// 设计决策：show_stroke GIF 不缓存。
void RegisterEducationMcpTools(McpServer& mcp, UiDisplay* ui, bool include_stroke) {
    if (!ui) return;  // 仅 UiDisplay 支持 GIF 动态注入（emoji_collection + LvglGif 路径）

    if (include_stroke) {
    mcp.AddTool("self.education.show_stroke",
        "教孩子写汉字 · 屏幕播单字笔顺动画。"
        "触发：『X 怎么写 / 教我写 X / 不会写 X / 学写 X / X 这个字怎么写 / 写给我看 X』。"
        "禁用：『怎么读/拼音/组词/什么意思/翻译/英文/英语/单词/念什么』→ 改用 show_card。"
        "character 严传单个汉字（U+4E00~U+9FFF）。同音消歧：用户说『X 的 Y』格式（如『中国的国』『苹果的苹』）取被强调的字 Y。"
        "调用后立即口播『看屏幕，跟着学笔顺』，动画出来再讲笔顺要点，≤3 句每句 ≤30 字。",
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

            // ============ 4G/WiFi 网络判定 · 4G 模式降级为静态字体 ============
            auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
            bool is_4g = (dual && dual->GetNetworkType() == NetworkType::ML307);
            if (is_4g) {
                ESP_LOGI(TAG, "show_stroke: 4G 模式 · 跳过 GIF · 字体兜底显示 '%s'",
                         character.c_str());
                std::string ch = character;  // 拷贝供 Schedule lambda 用
                UiDisplay* ui_local = ui;
                Application::GetInstance().Schedule([ui_local, ch]() {
                    // 与 GIF 失败兜底路径一致：必须 Speaking 才弹卡（防 LLM 调用时机错位闪屏）
                    if (Application::GetInstance().GetDeviceState() != kDeviceStateSpeaking) return;
                    ui_local->ShowEduCard(ch.c_str(), "");
                });
                // 回执提示 LLM 主动告知用户切 WiFi（云端会把这段文本传给模型，模型自然口播）
                return std::string(
                    "OK: 当前为 4G 网络，已显示静态字体『") + character +
                    "』。动态笔画动画需要 WiFi 才能流畅播放，"
                    "请用 TTS 提示用户：『现在用的是 4G 网络看不到笔画动画哦，"
                    "你可以从控制中心切到 WiFi，就能看动画啦』。"
                    "提示后正常讲笔顺要点，不需要重复调用本工具。";
            }

            // ============ WiFi 路径：原 GIF 下载链路 ============
            uint32_t url_hash = HashUrl(url_buf);
            int64_t now_us = esp_timer_get_time();
            uint32_t prev_hash = g_inflight_url_hash_.load(std::memory_order_acquire);
            int64_t prev_start = g_inflight_start_us_.load(std::memory_order_acquire);
            if (prev_hash == url_hash && (now_us - prev_start) < kInflightWindowUs) {
                uint32_t prev_req = g_inflight_req_id_.load(std::memory_order_acquire);
                ESP_LOGI(TAG, "show_stroke: dedup · same URL in-flight req=%u (elapsed %lldms)",
                         (unsigned)prev_req, (long long)((now_us - prev_start) / 1000));
                return std::string("OK: same character already loading, reusing in-flight request");
            }

            // 入口立即翻 pending 守护位（block ShowEduCard 闪屏）+ 拿并发去重 token
            // 同轮内若 LLM 连发两次不同字 show_stroke → 旧 token 自动作废，新下载覆盖
            uint32_t req_id = ui->BeginFontPending();
            // 登记 in-flight · CAS 不必要（mcp handler 主线程串行执行）
            g_inflight_url_hash_.store(url_hash, std::memory_order_release);
            g_inflight_start_us_.store(now_us, std::memory_order_release);
            g_inflight_req_id_.store(req_id, std::memory_order_release);
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
                        // N3 修复：清 inflight · 后续重发同字立即触发新下载
                        g_inflight_url_hash_.store(0, std::memory_order_release);
                        // N4 修复 2026-05-12：放宽到 Speaking || Listening · 防偶发不出动画
                        auto state = Application::GetInstance().GetDeviceState();
                        if (state != kDeviceStateSpeaking && state != kDeviceStateListening) return;
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
                    // 主线程二次守护：Speaking || Listening 都允许装载（N4 修复 2026-05-12）
                    // 原仅 Speaking 守护过严 · 极短 TTS 场景已回 Idle → GIF 被丢
                    auto state = Application::GetInstance().GetDeviceState();
                    if (state != kDeviceStateSpeaking && state != kDeviceStateListening) {
                        ESP_LOGI(TAG, "show_stroke: state=%d drop GIF req=%u", (int)state, (unsigned)req_id);
                        heap_caps_free(gif);
                        d->CancelFontPending(req_id);
                        g_inflight_url_hash_.store(0, std::memory_order_release);  // N3 清
                        return;
                    }
                    d->FontGif(gif, gsz, req_id);   // PSRAM buffer 所有权转移给 LvglRawImage
                    g_inflight_url_hash_.store(0, std::memory_order_release);      // N3 清
                });
                delete ctx;
                vTaskDelete(NULL);
            }, "stroke_dl", 8192, ctx, 1, nullptr, 0, MALLOC_CAP_SPIRAM);

            return std::string("OK: stroke loading");
        });
    }  // include_stroke

    // 教育卡渲染（极简 · 两行布局 · 不分类）
    mcp.AddTool("self.education.show_card",
        "教育卡（屏显两行）· 拼音/组词/单词/翻译/释义时主动调用，与口播同步。"
        "① 拼音『怎么读/拼音是什么/念什么/读什么』→ main=汉字 top=带声调拼音(shū bāo)。"
        "② 组词『怎么组词/组什么词/组个词/造词』→ main=词 top=拼音。"
        "③ 单词/翻译『英语怎么说/英语是什么/单词/翻译/英文』→ main=英文 top=中文。"
        "④ 释义『什么意思/啥意思/解释』→ main=词 top=≤8 字释义。"
        "同音消歧：『X 的 Y』格式（如『中国的国』）按 Y 字处理。"
        "main ≤4 汉字或 12 字符，top ≤8 汉字或 20 字符；超长返 ERR。",
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
