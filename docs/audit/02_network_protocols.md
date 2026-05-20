# 网络协议子系统缺陷审计报告

> 审计对象：P30-v31 固件 · 网络协议子系统
> 审计范围：`main/protocols/`（10 文件）+ `main/ota.{cc,h}` + `main/remote_cmd.{cc,h}`，共 14 文件 / 约 4861 行
> 审计日期：2026-05-20
> 判级标准：P0 必崩（烧硬件/砖机/安全漏洞/必然崩溃）｜P1 高频崩溃或体验严重退化｜P2 偶发/边缘场景｜P3 潜在远期风险

---

## 子系统概述

本子系统负责设备与云端的全部网络通信，包含 5 套并存的协议实现：

- **`protocol.{cc,h}`**：抽象基类，定义 hello/listen/abort/mcp 等通用 JSON 消息与 120s 通道超时。
- **`mqtt_protocol`**：MQTT 控制面 + UDP（AES-CTR 加密）音频面，小智 v3 协议。
- **`websocket_protocol`**：通用 WebSocket，二进制 BinaryProtocol2/3 帧承载 OPUS。
- **`websocket_joyai_protocol`**：京东 JoyInside 协议，文本 JSON + base64/二进制音频，15s PING 心跳。
- **`websocket_baidu_protocol`**：百度 RTC 大模型协议（量产主力，1532 行），WS 保活复用 + License 激活 + 远程音乐 + Function Call。
- **`ota`**：版本检查 / 固件下载烧写（带断点续传）/ 设备激活（HMAC）/ 状态上报。
- **`remote_cmd`**：云端下行远程命令分发（reboot/ota/sleep/volume/music/prompt 等 20+ 命令）。

公共风险面：所有协议运行在 Core 0 网络栈，多处在网络回调中调用 `Application::Schedule` 跨线程派发；OTA 直接写 Flash 引导分区；remote_cmd 在无鉴权前提下执行重启/休眠/OTA 等高危动作。

整体工程质量较高（已有 `prevent_destroy_guard_` 析构保护、电路断路器、断点续传、ASR 去重等防御设计）。但仍存在若干安全与崩溃风险，分级如下。

---

## P0 — 必崩 / 安全漏洞

### P0-1　WebSocket 二进制音频帧解析无长度校验，越界读 + 巨量分配
- **判级理由**：服务端或中间人可下发短/伪造二进制帧，触发堆越界读（信息泄露/崩溃）或按伪造 `payload_size` 申请超大 vector（OOM 必崩）。属"必然崩溃 + 安全漏洞"。
- **文件**：`main/protocols/websocket_protocol.cc:112-146`
- **问题代码**：
```cpp
if (version_ == 2) {
    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;   // 未校验 len >= sizeof(BinaryProtocol2)
    ...
    bp2->payload_size = ntohl(bp2->payload_size);    // 取自网络的长度
    auto payload = (uint8_t*)bp2->payload;
    ... std::vector<uint8_t>(payload, payload + bp2->payload_size)  // 越界读 + 巨量分配
```
- **根因**：直接把 `data` 强转为协议结构体指针，未校验 `len` 至少为头部大小，也未校验 `payload_size <= len - sizeof(header)`。`payload_size` 完全来自对端，可被构造为任意大值。
- **触发条件与影响面**：连接到通用 WebSocket 服务端（`version_==2/3`）。一个长度为 1 字节的二进制帧即触发头部越界读；一个声明 `payload_size=0xFFFFFFFF` 的帧触发 4GB vector 分配 → OOM abort。设备端无法防御恶意/异常服务端，且 4G/公网链路上中间人可注入。
- **修复建议**：解析前校验 `if (len < sizeof(BinaryProtocol2)) return;`；读出 `payload_size` 后校验 `if (payload_size > len - sizeof(BinaryProtocol2)) return;`，version 3 同理。joyai 二进制帧（`websocket_joyai_protocol.cc:459`）和裸 payload 分支同样按 `len` 兜底（这两处已用 `len`，相对安全）。

### P0-2　MQTT ParseServerHello 对 udp 子字段解引用空指针
- **判级理由**：服务端 hello 缺少 `server`/`port`/`key`/`nonce` 任一字段即解引用 NULL，`->valuestring` 必崩。OpenAudioChannel 路径每次唤醒都会走到，属必崩。
- **文件**：`main/protocols/mqtt_protocol.cc:358-361`
- **问题代码**：
```cpp
udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;   // 缺字段→NULL->valuestring
udp_port_   = cJSON_GetObjectItem(udp, "port")->valueint;
auto key    = cJSON_GetObjectItem(udp, "key")->valuestring;
auto nonce  = cJSON_GetObjectItem(udp, "nonce")->valuestring;
```
- **根因**：`cJSON_GetObjectItem` 返回 nullptr 时未做 `cJSON_IsString` 校验直接解引用，违反本文件其它处（如 transport/session_id）已有的校验范式。
- **触发条件与影响面**：MQTT 协议被启用且服务端 hello 的 udp 段不完整/被篡改时崩溃。MQTT 当前非量产主力，但仍是可选协议；公网 MQTT broker 异常或攻击均可触发。
- **修复建议**：每个字段用 `cJSON_IsString/IsNumber` 校验后再取值，任一缺失则 `ESP_LOGE` 并 return，不进入 setkey/UDP 流程。

### P0-3　OTA 固件无应用层验签 + Secure Boot 未开启，可被刷入任意固件（砖机/植入）
- **判级理由**：`sdkconfig` 中 `CONFIG_SECURE_BOOT is not set`、未启用 `SECURE_SIGNED_APPS`。`Ota::Upgrade` 仅依赖 `esp_ota_end()` 的结构化哈希校验（防传输损坏），不校验固件签名/来源。固件 URL 来自 OTA 服务器 JSON（`firmware.url`）或远程命令，攻击者控制该 URL 即可刷入任意固件 → 永久植入 / 砖机。属安全漏洞 P0。
- **文件**：`main/ota.cc:352-536`（`Upgrade`）；`main/ota.cc:235-238`（`firmware_url_` 取自 JSON）；`sdkconfig:494-495`
- **问题代码**：
```cpp
err = esp_ota_end(update_handle);                 // 仅校验镜像结构/SHA，不校验签名
...
err = esp_ota_set_boot_partition(update_partition);  // 直接设为引导分区
```
- **根因**：未开启 Secure Boot V2（硬件已支持，见 `CONFIG_SECURE_BOOT_V2_RSA_SUPPORTED=y`），也无应用层固件签名校验；固件来源 URL 可被服务端/链路控制（HTTP 明文下载，见 P1-3）。
- **触发条件与影响面**：能影响 OTA 响应或诱导设备访问恶意 `firmware.url` 的攻击者，可向全部设备推送任意固件。量产设备的根本性安全风险。
- **修复建议**：量产固件启用 Secure Boot V2 + Flash Encryption；至少启用 `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`（应用层 RSA 验签，`esp_ota_end` 会校验签名）；固件下载强制 HTTPS 且校验证书；OTA URL 域名做白名单。

### P0-4　远程命令通道无鉴权，云端/链路可远程重启/休眠/刷机/改 STT 回传
- **判级理由**：`RemoteCmd::Handle` 对下行命令不做任何来源鉴权或签名校验，直接执行 `reboot`/`sleep`(深睡解绑)/`ota`/`reload`/`stt_url`(把用户语音 ASR 转发到任意 URL) 等高危动作。任何能注入一条下行 WS/MQTT 消息的实体即可远程控制或窃听设备。属安全漏洞 P0。
- **文件**：`main/remote_cmd.cc:70-122`（分发）；`main/remote_cmd.cc:370-395`（`OnSttUrl`）；`main/remote_cmd.cc:397-480`（`PostSttText` 把 STT 文本 POST 到任意 `stt_url`）
- **问题代码**：
```cpp
else if (strcmp(type, "stt_url") == 0) OnSttUrl(msg);   // 设置任意回传地址
...
void RemoteCmd::OnSttUrl(const cJSON* msg) {
    std::string url = url_item->valuestring;
    stt_url_ = url;
    Settings s("remote_cmd", true);
    s.SetString("stt_url", url);                          // 持久化，重启仍生效
}
```
- **根因**：命令承载在已建立的协议通道内，依赖"通道本身可信"的隐含假设；但通道明文/弱链路（4G AT、未强制证书校验，见 P1-3）下该假设不成立。`stt_url` 允许把用户每句语音识别结果持久转发到任意外部 URL（窃听）。
- **触发条件与影响面**：服务端被攻破、消息被注入、或链路中间人，均可远程刷机/休眠/窃听全部设备。
- **修复建议**：高危命令（reboot/sleep/ota/reload/stt_url/download）增加服务端签名校验或一次性 nonce；`stt_url` 限制为可信域名白名单；强制全链路 HTTPS/WSS 证书校验。

---

## P1 — 高频崩溃 / 体验严重退化

### P1-1　`flow` 远程命令永不命中（dispatch key 与协议文档/字段不一致）
- **判级理由**：`remote_cmd.h` 文档与 `OnFlow` 解析的字段均为 `{"type":"flow","action":...}`，但分发表用 `strcmp(type, "live_companion")` 触发 `OnFlow`。下发文档约定的 `flow` 命令会落入 "未知命令" 分支，直播伴侣远程启停功能整体失效。属功能严重退化（B 验证项"达人运营/直播伴侣"核心通路断裂）。
- **文件**：`main/remote_cmd.cc:106`；对照 `main/remote_cmd.h:34`（`flow` 文档）
- **问题代码**：
```cpp
else if (strcmp(type, "live_companion") == 0) OnFlow(msg);   // 文档/字段都是 "flow"
```
- **根因**：命令名重命名（flow ↔ live_companion）后分发表与文档/调用方未同步。
- **触发条件与影响面**：任何按文档下发 `{"type":"flow",...}` 的服务端调用均无效。若服务端实际发的是 `live_companion` 则相反——以哪边为准需 **[待确认]**（建议核对服务端实际下发字段）。无论如何两端不一致。
- **修复建议**：统一命令名；保险起见同时接受 `flow` 与 `live_companion`：`if (strcmp(type,"flow")==0 || strcmp(type,"live_companion")==0) OnFlow(msg);`。

### P1-2　MQTT 收发 nonce/数据使用未对齐指针强转 16/32 位读写
- **判级理由**：`SendAudio`/UDP `OnMessage` 中对 `std::string` 缓冲做 `*(uint16_t*)&nonce[2]`、`*(uint32_t*)&data[8]` 等未对齐访问。ESP32-S3（Xtensa）对部分未对齐 32 位访问会触发 LoadStoreAlignment 异常或读到错值。音频通道每帧触发，属高频风险。
- **文件**：`main/protocols/mqtt_protocol.cc:178-180`、`262-263`
- **问题代码**：
```cpp
*(uint16_t*)&nonce[2]  = htons(packet->payload.size());
*(uint32_t*)&nonce[8]  = htonl(packet->timestamp);
*(uint32_t*)&nonce[12] = htonl(++local_sequence_);
...
uint32_t timestamp = ntohl(*(uint32_t*)&data[8]);   // data 来自 std::string，偏移 8/12 未必 4 字节对齐
uint32_t sequence  = ntohl(*(uint32_t*)&data[12]);
```
- **根因**：`std::string::data()` 起始地址未保证 4 字节对齐，`+8/+12` 偏移上的 32 位访问可能未对齐。是否崩溃取决于编译器是否生成对齐敏感指令 **[待确认 是否实测崩溃]**，但属确定性隐患。
- **触发条件与影响面**：MQTT+UDP 音频通道启用时，每个音频包收发都走该路径。
- **修复建议**：用 `memcpy` 做字节搬运替代指针强转：`uint32_t v; memcpy(&v, data.data()+8, 4); timestamp = ntohl(v);` 写侧同理。

### P1-3　4G(ML307) 链路 TLS 证书校验路径未在范围内确认 / OTA 与会话 Token 走明文 HTTP 风险
- **判级理由**：WiFi/esp 路径的 WS/MQTT 已 `crt_bundle_attach`（`components/78__esp-ml307/src/esp/esp_ssl.cc:38`、`esp_mqtt.cc:29`），但 OTA、`FetchSessionToken`、`PostToOta`、`PostSttText` 等 HTTP 调用是否强制 HTTPS + 证书校验，取决于 URL scheme 与 4G AT 通道的 TLS 实现，未在本审计范围内验证。若任一走明文 HTTP，则配置（含 MQTT 密码、WS token、百度 license/token）与固件 URL 可被链路中间人窃取/篡改，放大 P0-3/P0-4。
- **文件**：`main/ota.cc:108-145`（CheckVersion，URL 来自 NVS `ota_url`/`CONFIG_OTA_URL`）；`main/protocols/websocket_baidu_protocol.cc:375-457`（FetchSessionToken HTTP GET）
- **根因**：URL scheme 与底层 4G TLS 校验由网络组件层决定，协议层未强制。
- **触发条件与影响面**：4G 弱网为量产主用链路；若明文则凭据/固件全程可被窃听篡改。
- **修复建议**：审计 `ota_url`/`websocket.url`/`mqtt.endpoint` 是否一律 https/wss/mqtts；4G ML307 HTTP 客户端确认启用证书校验（与 esp 路径对齐）。**[待确认 网络组件层实现]**

### P1-4　`OnFlow` status 分支 `state_names` 数组越界
- **判级理由**：`state_names[]` 仅 4 项，索引来自 `lc->GetState()` 强转 int，若枚举新增第 5 态即越界读 → 崩溃或乱码。flow 状态查询路径触发。
- **文件**：`main/remote_cmd.cc:333-338`
- **问题代码**：
```cpp
const char* state_names[] = {"空闲", "播放中", "等待中", "暂停中"};   // 4 项
int state_idx = static_cast<int>(lc->GetState());
snprintf(buf, sizeof(buf), "状态: %s ...", state_names[state_idx], ...);  // 无边界检查
```
- **根因**：枚举到字符串映射未做范围保护。
- **触发条件与影响面**：FlowEngine 状态枚举扩展后触发；当前 4 态时安全，属边缘但确定性隐患。
- **修复建议**：`if (state_idx < 0 || state_idx >= 4) state_idx = 0;` 或加 `static_assert` 绑定枚举数量。

---

## P2 — 偶发 / 边缘场景

### P2-1　`WebsocketProtocol::OpenAudioChannel` 收到非 JSON 帧时 cJSON 空指针解引用
- **判级理由**：`cJSON_ParseWithLength` 失败返回 nullptr，随后直接 `cJSON_GetObjectItem(root, "type")`。cJSON 对 root==NULL 通常返回 NULL（不崩），但 `ESP_LOGE(... std::string(data,len)...)` 在二进制误判为文本时仍正常；崩溃风险低，但缺失对解析失败的显式处理，行为不确定。
- **文件**：`main/protocols/websocket_protocol.cc:150-163`
- **问题代码**：
```cpp
auto root = cJSON_ParseWithLength(data, len);
auto type = cJSON_GetObjectItem(root, "type");   // root 可能为 nullptr
...
cJSON_Delete(root);
```
- **根因**：未检查 `root == nullptr`。
- **触发条件与影响面**：服务端发非法 JSON 文本帧。
- **修复建议**：`if (!root) { ESP_LOGE(...); return; }` 解析后立即校验，与 mqtt/joyai 范式对齐。

### P2-2　Baidu 协议字符串前缀解析用 `find(...)==0` + `substr`，可被构造前缀导致空 substr/误判
- **判级理由**：`HandleTextMessage` 用 `len > 8` 等魔法长度配合 `msg.find(PREFIX)==0` 判断，分支兜底（else）直接 `substr(strlen(ASR_Q_FIN_PREFIX))`；若消息恰为 `[Q]:` 无内容则 substr 越界抛 `out_of_range`（std::string::substr 在 pos>size 时抛异常，未捕获 → abort）。
- **文件**：`main/protocols/websocket_baidu_protocol.cc:727-744`
- **问题代码**：
```cpp
if (prefix_char == 'Q' && len > 4 && ...) {
    std::string msg(data, len);
    ...
    else { HandleAsrResult(msg.substr(strlen(ASR_Q_FIN_PREFIX)), true); }  // 若 len==4 ("[Q]:") → substr(4) ok=空；len<4 已被 len>4 挡住，边界恰好
```
- **根因**：依赖 `len > 4` 守卫，`[Q]:` 正好 4 字节被挡，`substr(4)` 对 len>4 安全；但 `[A]:`/`[F]:` 等分支用 `len > 4` 而非 `len >= strlen(prefix)`，对追加前缀（如 `[Q]:[M]:[C]:` strlen=12）的短消息存在 `substr` 越界可能性极低但非零。实测影响小，列 P2。
- **触发条件与影响面**：服务端发出畸形前缀消息。
- **修复建议**：统一用 `if (msg.size() >= strlen(PREFIX))` 后再 substr；或集中一个安全 `safe_substr`。

### P2-3　Joyai base64 解码缓冲大小估算偏小风险（边界）
- **判级理由**：`payload((in_len*3)/4 + 4)` 作为 base64 解码上限缓冲，正常足够，但未对 `in_len` 上限做限制，超长 `audioBase64` 字段会在 PSRAM/堆上分配 ~0.75×字符串大小，叠加 cJSON 已持有的原串，单帧峰值内存翻倍。边缘下可触发 OOM。
- **文件**：`main/protocols/websocket_joyai_protocol.cc:427-440`
- **根因**：无单帧大小上限（违反 CLAUDE 规范"所有 vector 必须有 size 上限检查"）。
- **触发条件与影响面**：服务端下发超大 base64 音频帧。
- **修复建议**：加 `if (in_len > kMaxB64) return;` 上限（如 64KB）。

### P2-4　多处 `std::stoi` 未捕获异常（端口/版本解析）
- **判级理由**：`MqttProtocol::StartMqttClient` 用 `std::stoi(endpoint.substr(pos+1))` 解析端口，`Ota::ParseVersion` 用 `std::stoi(segment)`。非数字输入抛 `std::invalid_argument`/`out_of_range` 未捕获 → abort。endpoint 来自 NVS（OTA 下发），version 来自固件 JSON。
- **文件**：`main/protocols/mqtt_protocol.cc:140`；`main/ota.cc:616`
- **问题代码**：
```cpp
broker_port = std::stoi(endpoint.substr(pos + 1));   // endpoint 含非数字端口 → 抛异常
...
versionNumbers.push_back(std::stoi(segment));        // version "1.0.x" → 抛异常
```
- **根因**：信任服务端下发字符串格式。
- **触发条件与影响面**：OTA 配置/版本字符串异常时崩溃。joyai 协议注释（`websocket_joyai_protocol.cc:201`）专门转义了 `:` 来规避底层 stoi 抛异常，说明该问题已知存在于栈中。
- **修复建议**：包 try/catch 或先用 `strtol` 校验后转换。

### P2-5　Baidu `OnDisconnected` 后不自动重连，依赖用户唤醒（设计如此，但弱网体验退化）
- **判级理由**：百度协议断连后仅 `PrefetchSessionTokenAsync`（HTTP 预取 + 条件 auto-connect），不主动重建音频通道；若预取条件（URL 含 `&id=&t=`）不满足，则需用户再次唤醒才恢复。4G 弱网频繁断连下体验退化，但有兜底，非崩溃。
- **文件**：`main/protocols/websocket_baidu_protocol.cc:623-650`
- **根因**：设计取舍（保活 + 按需）。
- **触发条件与影响面**：方式二（AK/SK）URL 无 id/t 时断连不自动恢复。
- **修复建议**：确认方式二是否也需自动重连，或在 UI 明确提示需唤醒。**[待确认 产品预期]**

---

## P3 — 潜在远期风险

### P3-1　百度 license_key 硬编码默认值落在源码
- **判级理由**：`license_key_` 默认值 `"759877c9b68b4aa082cc05390be0cea9"` 硬编码在构造函数，随固件分发。虽可被 NVS 覆盖，但默认值泄露在仓库/二进制中，属凭据管理远期风险。
- **文件**：`main/protocols/websocket_baidu_protocol.cc:115`
- **根因**：默认凭据入源码。
- **修复建议**：默认置空，强制由 OTA 下发；或最低限度从编译期 secret 注入而非明文常量。

### P3-2　`websocket_protocol.cc:117-119` 直接修改入站 buffer（`bp2->version = ntohs(...)`）
- **判级理由**：对 `const char* data` 强转后原地 `ntoh` 写回，修改了底层接收缓冲。若该 buffer 被上层复用/只读，存在写入只读内存或破坏后续解析的隐患。当前未观察到崩溃，列 P3。
- **文件**：`main/protocols/websocket_protocol.cc:116-131`
- **修复建议**：把头部字段拷到本地变量再做字节序转换，不改原 buffer。

### P3-3　OTA `Download` 流式读循环以 `n<=0` 退出，无法区分"读完"与"中途失败"，可能返回截断数据
- **判级理由**：`while(total_read<max_size){ n=Read(); if(n<=0) break; }` 后仅用 `expected>0 && total_read<expected` 校验完整性；若服务端不返回 Content-Length（`expected==0`）则任何中途断流都被当成功返回截断 buffer。该 buffer 用于动态图片/小资源，损坏数据影响有限，列 P3。
- **文件**：`main/ota.cc:578-602`
- **修复建议**：`expected==0` 时也应有最小长度/魔数校验，或要求服务端必带 Content-Length。

### P3-4　`Protocol::IsTimeout` 固定 120s，与 Baidu 自定义 idle_timeout(300s)/joyai PING(15s) 三套超时并存
- **判级理由**：基类 120s 超时与子类各自的保活/idle 机制语义重叠，`IsAudioChannelOpened` 同时依赖 `IsTimeout()`，可能在 Baidu 正常保活（300s idle）期间被基类 120s 提前判超时关通道。需确认是否每条入站都 `UpdateLastIncomingTime`（Baidu OnData 有调用，line 620），当前看似安全但耦合脆弱。
- **文件**：`main/protocols/protocol.cc:81-90`；`websocket_baidu_protocol.cc:307-310`
- **修复建议**：明确单一超时权威；子类若自管超时则 override `IsTimeout` 或不在 `IsAudioChannelOpened` 复用基类超时。**[待确认 实际入站频率]**

### P3-5　remote_cmd `download` 命令未实现却返回"下载中"
- **判级理由**：`OnDownload` 仅打印 "File sync not yet implemented in V2" 并删 copy，但先弹 "下载中..." Alert，误导用户/服务端以为成功。功能缺失，非崩溃。
- **文件**：`main/remote_cmd.cc:262-279`
- **修复建议**：未实现则明确返回失败提示，或移除该命令分发。

---

## 统计

| 等级 | 数量 | 条目 |
|------|------|------|
| P0   | 4    | P0-1 WS 二进制帧越界/巨量分配 · P0-2 MQTT hello 空指针 · P0-3 OTA 无验签+Secure Boot 关 · P0-4 远程命令无鉴权 |
| P1   | 4    | P1-1 flow 命令永不命中 · P1-2 MQTT 未对齐指针读写 · P1-3 4G TLS 校验未确认/明文风险 · P1-4 flow status 数组越界 |
| P2   | 5    | P2-1 WS 非 JSON 空指针 · P2-2 Baidu 前缀 substr 边界 · P2-3 joyai base64 无上限 · P2-4 stoi 未捕获异常 · P2-5 Baidu 断连不自动重连 |
| P3   | 5    | P3-1 license_key 硬编码 · P3-2 改写入站 buffer · P3-3 Download 截断 · P3-4 三套超时耦合 · P3-5 download 命令未实现 |
| **合计** | **18** | |

> 标 **[待确认]** 项需结合服务端实际下发字段、4G 网络组件层 TLS 实现、产品预期进一步核实，未在本子系统范围内编造结论。
