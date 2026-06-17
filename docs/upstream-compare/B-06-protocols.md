# B-06 protocols 协议 — 对比官网 v2.2.4 识别过度优化

> 基线 v2.2.4；标尺=量产稳定；🟢必要/🔴过度/⚪扩展/🔒安全/🛡️红线保留；只分析不改码。

## 取证范围（diff 过的文件 + 深审的自研文件）

| 文件 | 类型 | 规模 | 说明 |
|---|---|---|---|
| `protocol.cc` | 改官方 | **零差异** | 实测 `git diff v2.2.4 HEAD` 无任何输出，base 协议无定制 |
| `protocol.h` | 改官方 | +12/-1 | 基类加虚接口（client_frame_duration / SendTextToTts 等）+ last_incoming_time_ 初始化 |
| `mqtt_protocol.cc` | 改官方 | +50/-10 | UDP 打洞 / memcpy 防未对齐 / cJSON 类型校验 / SendText 断连守卫 / SendTextToTts·AI |
| `mqtt_protocol.h` | 改官方 | +2/-0 | 两个虚函数声明 |
| `websocket_protocol.cc` | 改官方 | +18/-2 | 二进制 v2/v3 帧长守卫 + cJSON_ParseWithLength |
| `websocket_protocol.h` | 改官方 | 零差异 | — |
| `websocket_baidu_protocol.{cc,h}` | **自研深审** | 1533+226 行 | 百度 bcelive 协议，opus 20ms 帧，License 激活，Token 预取 |
| `websocket_joyai_protocol.{cc,h}` | **自研深审** | 514+83 行 | JoyInside 协议，业务扩展为主 |

> 多协议选型逻辑在 `application.cc:667`（属模块 7），此处仅交叉引用不重复审。

---

## 🟢 必要（服务量产稳定的偏离）

| 项 | 我们怎么改 | 官方 v2.2.4 原实现 | 为何必要 | 证据 file:line |
|---|---|---|---|---|
| MQTT 接收 nonce 未对齐读 | 用 `memcpy(&ts_raw, &data[8], 4)` 再 ntohl | `ntohl(*(uint32_t*)&data[8])` 直接强转指针解引用 | ESP32 对未对齐 32 位访问可能 LoadStoreAlignment 异常崩溃；外部网络帧偏移不保证 4 字节对齐 | mqtt_protocol.cc:265-269 |
| MQTT 发送 nonce 未对齐写 | 同上，memcpy 写入 | `*(uint16_t*)&nonce[2]=...` 直接强转写 | 同上（写侧） | mqtt_protocol.cc:178-183 |
| MQTT ServerHello cJSON 空指针 | 取 server/port/key/nonce 后逐个 `cJSON_IsString/IsNumber` 校验，缺一即 return | 直接 `cJSON_GetObjectItem(udp,"server")->valuestring` 链式解引用 | 服务器下发畸形/缺字段 hello 时官方写法 NULL 解引用必崩；弱网/异常服务端可触发 | mqtt_protocol.cc:372-384 |
| MQTT SendText 断连守卫 | Publish 前判 `mqtt_==nullptr \|\| !IsConnected()`，未连则 WARN 丢弃 | 直接 `mqtt_->Publish()` | 断网瞬间 mqtt_ 可能已析构/未连，避免空指针 | mqtt_protocol.cc:158-162 |
| WS v2/v3 二进制帧长守卫 | 解析前判 `len < sizeof(BinaryProtocolN)`，解析后判 `payload_size > len - 头长` | 直接强转 `BinaryProtocol2* bp2=(...)data` 后按 payload_size 拷贝 | 短帧/谎报 payload_size 的脏帧会越界读 → 崩溃/信息泄露；P1 弱网现场可现 | websocket_protocol.cc:116-122, 137-143 |
| WS JSON 按长度解析 | `cJSON_ParseWithLength(data, len)` | `cJSON_Parse(data)`（依赖 NUL 结尾） | WS 帧非 NUL 结尾，裸 Parse 越界读 | websocket_protocol.cc:166 |
| base 基类 last_incoming_time_ 初始化 | 声明处 `= steady_clock::now()` | 默认构造（epoch 0） | 未初始化时 IsTimeout 立即判超时，连接刚建立即被误杀 | protocol.h:100-101 |

> 百度 / joyai 自研协议内的边界守卫见「深审发现」，同属 🟢 性质但不在官方 diff 内，单列。

---

## 🔴 过度（偏离官方又不服务稳定）

| 项 | 我们怎么改 | 官方 v2.2.4 原实现 | 为何判过度 | 维护成本/风险 | 证据 file:line |
|---|---|---|---|---|---|
| 百度协议注释残留生产端点 | 注释掉的调试代码里保留完整 `wss://rtc-aiotgw.exp.bcelive.com/v1/realtime?a=apprmpwazcyzemj&...` 域名+appid | 无此文件 | AK/SK 已 REDACTED，但生产 WS 域名 + appid（`apprmpwazcyzemj`）仍明文留在注释，属内部端点信息泄露；死注释无任何调用价值 | 低（删注释即可）；信息泄露面 | websocket_baidu_protocol.cc:554-556 |

> 仅 1 项纯过度。MQTT 的 `SendTextToTts`/`SendTextToAI` 两函数体完全相同（mqtt_protocol.cc:419-427），属轻微冗余但服务于 application 层文本注入扩展，归 ⚪ 不计 🔴。

---

## ⚪ 扩展（官方没有的纯业务功能，只登记）

| 项 | 能力 | 证据 file:line |
|---|---|---|
| base 基类文本注入虚接口 | 新增 `SendTextToTts/SendTextToAI/UpdateSystemPrompt/SendRemoteMusicControl/SendRawText`，默认 return false，供子类按需实现 | protocol.h:76-82 |
| base 可变帧长虚接口 | 新增 `client_frame_duration()` 默认 60，子类可覆写（百度=20） | protocol.h:81；官方仅有 server_frame_duration |
| MQTT 文本转 TTS/AI | 拼 `{type:listen,state:detect,text:...}` JSON 下发，供唤醒/快捷指令直接喂文本 | mqtt_protocol.cc:419-427 |
| MQTT 服务端主动关闭标记 | 收到 server goodbye 时 `MarkServerInitiatedClose()` 再关通道 | mqtt_protocol.cc:122 |
| 百度 bcelive 全协议 | License 激活/防抖、会话 Token 预取(HTTP)、20ms opus 帧、ASR/LLM/FC/Custom 前缀分发、本地 AEC 配置切换 | websocket_baidu_protocol.cc 全文 |
| 百度 Function Call → MCP 桥 | 百度 FC（`__` 替 `.`）还原为 JSONRPC 2.0 tools/call 喂 MCP | websocket_baidu_protocol.cc:1095-1145 |
| JoyInside 全协议 | EVENT/ASR/AGENT/ACTIVITY/TTS/PONG 分发、base64 音频、CLIENT_* 事件、Bearer Token 鉴权 | websocket_joyai_protocol.cc 全文 |

---

## 🔒 安全项

| 项 | 说明 | 风险级 | 证据 file:line |
|---|---|---|---|
| **百度 License Key 硬编码默认值** | `license_key_ = baidu_settings.GetString("license_key", "25e5b99aac0f4e9084df236112f946c2")`——NVS 未配置 license_key 时直接落到这个写死的 32 位 key，随固件编译进二进制。这正是 commit `81a11e02b 更新百度协议默认许可证密钥` 的产物，与项目记忆「百度云密钥泄露」同源。filter-repo 只清了 git 历史里的 AK/SK，**这个 license 默认值仍活在当前 HEAD 源码中**。 | P2 潜在（凭据泄露/被复用，非崩溃） | websocket_baidu_protocol.cc:115 |
| 百度协议注释残留生产端点 | 见 🔴 表，AK/SK 已 REDACTED 但域名+appid 明文 | P3 信息泄露 | websocket_baidu_protocol.cc:555 |

> 当前 HEAD **未发现** AK/SK/password/secret 明文（grep `sk-\|ak=\|secret\|appkey\|password\|bce` 仅命中上述 license 默认值与已 REDACTED 注释）。joyai token 走 `Bearer` header 由 NVS 注入（websocket_joyai_protocol.cc:174-178），无硬编码。

---

## 🛡️ 红线保留（触及内存安全 / 脏帧守卫，默认保留只标不动）

| 项 | 说明 | 证据 |
|---|---|---|
| MQTT memcpy 防未对齐读写 | 内存安全红线，替代官方裸指针强转 | mqtt_protocol.cc:178-183, 265-269 |
| MQTT cJSON 字段类型校验 | 防 NULL 解引用，外部网络帧守卫 | mqtt_protocol.cc:372-384 |
| WS v2/v3 二进制帧长双守卫 | 脏帧越界守卫（短帧 + 谎报 payload_size 双查），与模块 9「4G 脏帧守卫」同性质 | websocket_protocol.cc:116-122, 137-143 |
| WS cJSON_ParseWithLength | 非 NUL 结尾帧越界读守卫 | websocket_protocol.cc:166 |
| 百度文本解析全程 len 守卫 | 见深审，所有前缀分支 len>N 检查 + ParseWithLength | websocket_baidu_protocol.cc:706-808 |
| 百度二进制帧按 len 构造 | vector(data, data+len) 无裸索引越界 | websocket_baidu_protocol.cc:825-827 |

---

## 深审发现（自研代码逐点，带 file:line + 风险级）

### websocket_baidu_protocol（1533 行）

| 维度 | 结论 | 风险级 | 证据 file:line |
|---|---|---|---|
| 边界·文本解析 | 🟢 干净。入口 `len < 3 return`；每个 `[X]:` 前缀分支均 `len > N` 守卫后才 substr；裸 JSON 用 `cJSON_ParseWithLength(data,len)`；未识别用 `%.*s` 限长打印。无裸索引越界 | 无 | websocket_baidu_protocol.cc:706-808 |
| 边界·二进制帧 | 🟢 干净。`vector<uint8_t>(data, data+len)` 按 len 拷贝，无强转头部/无 payload_size 字段（百度帧为纯 opus），无越界面 | 无 | websocket_baidu_protocol.cc:825-827 |
| 边界·FunctionCall JSON | 🟢 干净。`cJSON_IsString/IsArray/IsObject` 全程类型校验后才取值，工具名 `__`→`.` 用 std::string::replace 无越界 | 无 | websocket_baidu_protocol.cc:1084-1130 |
| 长跑泄漏·cJSON | 🟢 无泄漏。HandleFunctionCall 所有早退分支（content 非 string、payload parse 失败）都先 `cJSON_Delete(fc)` 再 return；主路径 fc/payload/json 三个对象均 Delete | 无 | websocket_baidu_protocol.cc:1085, 1091, 1143-1145 |
| 长跑泄漏·连接对象 | 🟢 无泄漏。析构 guard 置 false + 3 个 timer 全 Stop+Delete + websocket Close+reset + `vEventGroupDelete`；重连前 `websocket_.reset()` | 无 | websocket_baidu_protocol.cc:177-206, 604-606 |
| 多任务共享数据 | 🟢 加锁/atomic 到位。`media_ready_`/`licensed_`/`is_speaking_` 用 `std::atomic` + `exchange(acq_rel)`；析构悬空 this 用 `prevent_destroy_guard_` shared atomic + Schedule lambda 内 `guard->load()` 检查 | 无 | websocket_baidu_protocol.cc:180, 463, 625, 630 |
| 异常路径·连接重试 | 🟢 有界重试。`kMaxConnectAttempts=3` + 递增延迟 {500,1500}ms，非裸 portMAX_DELAY；License/MediaReady 等待用 10s 超时事件组 | 无 | websocket_baidu_protocol.cc:656-682 |
| 弱网断流路径 | 🟢 完整。OnDisconnected 内 `media_ready_.exchange(false)`、补发 tts stop、清 Token 缓存、`audio_send_failures_=0`、`on_audio_channel_closed_` 回调；发送失败累计阈值触发 Close | 无 | websocket_baidu_protocol.cc:623-650, 240 |
| 掉电/休眠路径 | 🟢 连接前 `SetPowerSaveLevel(PERFORMANCE)` 预热 500ms 再建 TLS，避免休眠态建连失败 | 无 | websocket_baidu_protocol.cc:652-654 |
| 继承关系 | ⚪ `public Protocol`（与官方 WebsocketProtocol 平级，均直继 Protocol），非继承官方 WS 实现。属"另起炉灶"但合理——百度帧格式/前缀分发与官方 binary v2/v3 完全不同，无可复用基类，不算堆叠复杂度 | 无 | websocket_baidu_protocol.h:76 |
| 安全 | 🔒 License 默认值硬编码 + 注释残留端点，见 🔒 表 | P2/P3 | websocket_baidu_protocol.cc:115, 555 |

### websocket_joyai_protocol（514 行）

| 维度 | 结论 | 风险级 | 证据 file:line |
|---|---|---|---|
| 边界·文本解析 | 🟢 干净。`std::string s(data,len)` 按 len 界定后 `cJSON_Parse(s.c_str())`，string 自带 NUL 终止，无越界（区别于裸指针 cJSON_Parse） | 无 | websocket_joyai_protocol.cc:370-372 |
| 边界·base64 音频解码 | 🟢 干净。缓冲按 `(in_len*3)/4+4` 预分配（base64 解码上界），`mbedtls_base64_decode` 返回 out_len 后 resize，rc 校验。无溢出 | 无 | websocket_joyai_protocol.cc:426-440 |
| 边界·二进制帧 | 🟢 干净。`vector<uint8_t>(data, data+len)` 按 len 构造 | 无 | websocket_joyai_protocol.cc:468 |
| 长跑泄漏·cJSON | 🟢 无泄漏。HandleTextMessage 各分支（parse 失败、type 命中、各 contentType）均配对 `cJSON_Delete(root)`；临时 `fake` 对象 273/283/345/414 均 Delete | 无 | websocket_joyai_protocol.cc:372-456 |
| 长跑泄漏·连接对象 | 🟢 无泄漏。CloseAudioChannel 内 ping_timer Stop + `websocket_.reset()`；OpenAudioChannel 重入前先 reset | 无 | websocket_joyai_protocol.cc:120-127 |
| 异常路径 | 🟢 HandleErrorMessage 置 error_occurred_ + MarkServerInitiatedClose + closed 回调；ping 失败计数 | 无 | websocket_joyai_protocol.cc:472-479 |
| 弱网断流路径 | 🟢 OnDisconnected 有处理（line 193）；IsAudioChannelOpened 复合判 Connected+!error+!Timeout | 无 | websocket_joyai_protocol.cc:116-118, 193 |
| 多任务共享数据 | 🟢 `interrupt_pending_`/`tts_started_` 标志 + `mid_counter_.fetch_add` atomic 自增 | 无 | websocket_joyai_protocol.cc:462, 502 |
| 继承关系 | ⚪ `public Protocol`，平级实现，理由同百度 | 无 | websocket_joyai_protocol.h |
| 安全 | 🟢 token 走 `Bearer` header 由 NVS 注入，无硬编码密钥 | 无 | websocket_joyai_protocol.cc:174-178 |

---

## 小结

🟢 必要 **7** · 🔴 过度 **1** · ⚪ 扩展 **7** · 🔒 安全 **2** · 🛡️ 红线保留 **6**

**一句话结论**：protocols 模块改动质量高——base 协议(protocol.cc)零定制，MQTT/WS 的全部偏离都是内存安全与脏帧守卫（🛡️ 红线，默认保留），两个自研协议(百度/joyai)收帧/解析/释放/弱网/泄漏六维全部干净，唯一真问题是百度协议**硬编码 License 默认值 + 注释残留生产端点**两处 🔒 安全项，需控制台轮换 license 并清注释（属信息泄露非崩溃，P2/P3）。
