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

// N3 修复 2026-05-12：show_stroke 同字 URL in-flight 去重
//   场景：t1 下载中孩子又说同字 → t2 触发 → t1 完成时 req_id 过期被 free · 浪费流量
//   方案：file-static 状态记录正在下载的 URL hash + req_id · 5s in-flight 窗口内同 URL 立即返回旧 req_id
//   原子三元组：URL hash · 启动时戳 · req_id · 失效条件：hash=0 表示无 in-flight
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
//   理由：① GIF buffer 与 LVGL GIF decoder 异步渲染绑定，缓存 buffer 跨 LvglRawImage 实例生命周期复杂
//        ② PSRAM 占用收益不对等（缓存 1MB vs 重下 200-2000ms · 量产期优先稳定性）
//        ③ 单字 GIF 服务器侧可能更新笔顺修正，每次下载保证最新
//   每次调用都走完整 HTTP 下载 + 校验路径。

void RegisterEducationMcpTools(McpServer& mcp, UiDisplay* ui, bool include_stroke) {
    if (!ui) return;  // 仅 UiDisplay 支持 GIF 动态注入（emoji_collection + LvglGif 路径）

    if (include_stroke) {
    mcp.AddTool("self.education.show_stroke",
        "[识字核心 · 高优先级] 屏幕播放单个汉字笔顺 GIF,孩子看动画学写字。\n"
        "\n"
        "## 必调场景(任一即触发,不要先问):\n"
        "- 问写法: 怎么写 / X怎么写 / 写法 / 笔顺 / 笔画 / 先写哪笔\n"
        "- 求教学: 教我写 / 教我X / 教写字 / 学写字 / 学X字 / 陪我练字\n"
        "- 表困惑: 不会写 / X我不会 / 帮我写 / 怎么写呀\n"
        "- 求示范: 示范 / 演示 / 给我看 / 写给我看 / 一起写X\n"
        "- 间接说: 想看X字 / 我们写X / X字长什么样 / 学习X字\n"
        "\n"
        "## 禁调场景(改用其他):\n"
        "- '怎么读 / 读音 / 拼音' → 不调,用 [X_pīnyīn] 卡\n"
        "- '是什么字 / 这是啥' → 不调,用 [X_pīnyīn] 识字\n"
        "- '什么意思' → 不调,口语解释 + [X_pīnyīn]\n"
        "- 算式/古诗/闲聊 → 不涉及\n"
        "\n"
        "## character 取字:\n"
        "- 严格单字 · U+4E00~U+9FFF\n"
        "- 多字句取核心字('不会写花字' → '花')\n"
        "- 字不明确 → 先反问'想学哪个字',别瞎调\n"
        "\n"
        "调用后立即口播'好,画给你看',加载期间有反馈,出图后再讲笔顺。回复 ≤3 句,每句 ≤30 字。",
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
            // 决策（2026-05-12）：show_stroke GIF 30-80KB · 4G 下载 1-3s 流量贵且慢
            //   双网板（P30-4G/P31）跑在 ML307 模式时跳过下载，仅显示字体 + 提示用户切 WiFi
            //   WifiBoard（P30-WiFi）dynamic_cast 失败 → is_4g=false → 正常下载
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
            // N3 去重：同 URL 5s 内 in-flight 直接复用现有 req_id · 不重发 HTTP
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
