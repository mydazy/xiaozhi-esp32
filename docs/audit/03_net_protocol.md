# 03 网络协议子系统审计（WS / MQTT / Protocol）

> 审计范围：`main/protocols/`（protocol.* / websocket_protocol.* / websocket_baidu_protocol.* / websocket_joyai_protocol.* / mqtt_protocol.*）
> 文件数：**10**（5 个 .cc + 5 个 .h）
> 关注点：帧长度校验、重连、socket/句柄泄漏、TLS、超时缺失、外部输入边界、并发锁。
> 审计日期：2026-05-20 ｜ 审计法：三遍法（广度 / 红线深挖 / 反审自检）

底层依赖说明（影响多条判级）：
- `WebSocket::OnData(data, len, binary)` 的 `data` 来自 `current_message_.data()`，**不保证 NUL 结尾**（见 `components/78__esp-ml307/src/web_socket.cc:374-377`，缓冲为 `std::vector` 直接 `.data()`/`.size()`）。因此对 `data` 直接调用 `cJSON_Parse`（按 NUL 结尾扫描）或 `strlen` 是越界读。
- 标准 `websocket_protocol.cc` 已正确用 `cJSON_ParseWithLength(data, len)`，可作为修复范本。

---

## 第一遍 · 广度遍历（逐文件显性缺陷）

### 03-P0-A  baidu 文本帧裸 cJSON_Parse 越界读（非 NUL 结尾缓冲）
- 严重等级：**P0**。判级理由：外部网络帧直接喂给按 NUL 扫描的 `cJSON_Parse`，缓冲无终止符，触发堆越界读，可崩溃 / 信息泄漏，攻击面是裸 WS 数据，必现于不带 `\0` 的服务端帧。
- 文件：`main/protocols/websocket_baidu_protocol.cc:791-792`
- 代码：
```cpp
if (data[0] == '{' && len > 2) {
    cJSON* json = cJSON_Parse(data);   // data 非 NUL 结尾，越界读
```
- 根因：`HandleTextMessage` 对裸 JSON 分支未用 `len`，直接把 `data` 当 C 字符串。
- 触发/影响：服务端（或被劫持的中间人）下发以 `{` 开头且未补 `\0` 的文本帧 → 解析器扫到帧尾外的堆内存。轻则误解析，重则读越界崩溃。
- 修复：改为 `cJSON* json = cJSON_ParseWithLength(data, len);`（与 `websocket_protocol.cc:166` 一致）。
- [发现于第一遍]

### 03-P1-B  WebSocket 句柄/接收任务泄漏：OpenAudioChannel 失败路径未 reset
- 严重等级：**P1**。判级理由：连接握手任一步失败直接 `return false`，已 `CreateWebSocket` 的对象与底层 esp-tls 接收任务不释放，多次失败重连累积句柄/任务泄漏，最终 OOM 或建连失败；高频弱网必现。
- 文件：`main/protocols/websocket_protocol.cc:192-210`
- 代码：
```cpp
if (!websocket_->Connect(url.c_str())) { ... return false; }   // websocket_ 未 reset
...
if (!SendText(message)) { return false; }                      // 同上
... 等 server hello 超时 ... return false;                      // 同上
```
- 根因：失败分支只 `SetError`/`return`，未 `websocket_.reset()`；下次 `OpenAudioChannel` 才覆盖（覆盖前若被读到 `IsConnected()` 仍可能用旧对象）。
- 触发/影响：服务器无响应 / hello 超时反复发生时，每次失败残留一个 WS 实例与其 receive task，泄漏内部 RAM 与任务句柄。
- 修复：三处 `return false` 前加 `websocket_.reset();`（baidu 协议在 `OpenAudioChannel` 入口已先 Close+reset，可参照）。
- [发现于第一遍]

### 03-P1-C  WebsocketProtocol::ParseServerHello 对 transport 空指针解引用
- 严重等级：**P1**。判级理由：`transport->valuestring` 在 `transport` 非字符串时为野指针/NULL，`strcmp` 与日志 `%s` 解引用崩溃；服务端 hello 缺 transport 字段即触发。
- 文件：`main/protocols/websocket_protocol.cc:245-248`
- 代码：
```cpp
auto transport = cJSON_GetObjectItem(root, "transport");
if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0) {
    ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring); // transport 可能非 string
```
- 根因：只判 `nullptr`，未判 `cJSON_IsString`；且 `nullptr` 分支的日志仍打印 `transport->valuestring`（短路后 transport==nullptr 时不会进，但若 transport 是 number/object 则 valuestring=NULL）。
- 触发/影响：服务端把 transport 发成数字/对象 → `strcmp(NULL,...)` 崩溃。mqtt_protocol.cc:334-337 同型问题（见 03-P1-D）。
- 修复：`if (!cJSON_IsString(transport) || strcmp(transport->valuestring,"websocket")!=0) { ESP_LOGE(...,"non-string"); return; }`。
- [发现于第一遍]

### 03-P1-D  MqttProtocol::ParseServerHello 对 transport 空指针解引用
- 严重等级：**P1**。判级理由：同 03-P1-C，外部 hello 控制崩溃路径。
- 文件：`main/protocols/mqtt_protocol.cc:335-337`
- 代码：
```cpp
auto transport = cJSON_GetObjectItem(root, "transport");
if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
    ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
```
- 根因：未用 `cJSON_IsString`。
- 触发/影响：transport 非字符串 → `strcmp`/日志解引用 NULL 崩溃。
- 修复：`if (!cJSON_IsString(transport) || strcmp(transport->valuestring,"udp")!=0) { ESP_LOGE(...); return; }`。
- [发现于第一遍]

### 03-P2-E  MQTT endpoint 端口 std::stoi 未捕获异常
- 严重等级：**P2**。判级理由：`std::stoi` 对非数字字符串抛 `std::invalid_argument`，未 try/catch → 未捕获异常致 `std::terminate` / abort。endpoint 来自 NVS（半可信），边缘触发。
- 文件：`main/protocols/mqtt_protocol.cc:137-140`
- 代码：
```cpp
size_t pos = endpoint.find(':');
if (pos != std::string::npos) {
    broker_address = endpoint.substr(0, pos);
    broker_port = std::stoi(endpoint.substr(pos + 1));   // 非数字端口抛异常
```
- 根因：直接 `std::stoi` 解析端口，无异常防护、无范围校验。
- 触发/影响：配置下发 `host:abc` 或 `host:`（空）→ 抛异常崩溃（ESP-IDF 默认异常多致 abort）。
- 修复：用 `strtol` 校验或包 try/catch，失败回退默认 8883 并 `SetError`。
- [发现于第一遍]

### 03-P2-F  HTTP 会话 Token：状态码非 200 与读循环未限总长
- 严重等级：**P2**。判级理由：`FetchSessionToken` 读响应循环 `response += buffer` 无上限，恶意/异常服务端可使 `std::string` 无限增长致 OOM；偶发但属外部输入未限长。
- 文件：`main/protocols/websocket_baidu_protocol.cc:415-421`
- 代码：
```cpp
char buffer[512];
int bytes_read;
while ((bytes_read = http->Read(buffer, sizeof(buffer) - 1)) > 0) {
    buffer[bytes_read] = '\0';
    response += buffer;          // 无总长上限
}
```
- 根因：响应聚合无 size cap（违反 esp32-memory.md「所有 vector/队列必须有 size 上限」）。
- 触发/影响：服务端返回超大 body → response 撑爆内部/PSRAM。
- 修复：循环内 `if (response.size() > 8*1024) { ESP_LOGE(...); http->Close(); return false; }`。
- [发现于第一遍]

---

## 第二遍 · 红线深挖（四红线 + 跨文件数据流）

### 03-P0-G  WebSocket / MQTT 全程未见 TLS 证书校验（红线④ OTA/安全 关联）
- 严重等级：**P0**。判级理由：协议层创建 WS/MQTT/HTTP 均未设置 CA bundle / 证书校验入口，若底层默认 skip-verify，则鉴权 Token、会话密钥、license 走明文可被中间人替换 → 可伪造服务端下发恶意控制帧（含上面多条解析崩溃链）。安全漏洞，出货前必须确认。
- 文件：`main/protocols/websocket_protocol.cc:94-110`、`mqtt_protocol.cc:81-144`、`websocket_baidu_protocol.cc:602-667`、`websocket_joyai_protocol.cc:167-219`
- 现象：仅 `SetHeader(Authorization/Device-Id/...)` 后 `Connect(wss://...)`，协议层无 `SetCaCert` / `crt_bundle` / `skip_cert_common_name_check` 配置；TLS 信任策略完全交由底层 `web_socket.cc` / `esp_ssl.cc` 默认值，本子系统无任何防护。
- 根因：协议层未显式声明信任根，依赖底层默认；P30-4G（ml307）与 WiFi 两条栈默认行为不一致风险。
- 触发/影响：开放网络 / 可控 DNS 下中间人接管 wss 会话，结合 03-P0-A / 03-P1-C/D 远程致崩或注入。
- 修复（需 Jack 拍板，属红线④）：在板级网络层统一启用 `esp_crt_bundle_attach` 并关闭 insecure 选项；审计 `components/78__esp-ml307/src/esp/esp_ssl.cc` 的默认 verify 模式。**先出确认清单再改**。标 [待确认] 底层默认是否已校验。
- [发现于第二遍]

### 03-P1-H  baidu Function Call 嵌套 JSON 二次解析未限深 / content 任意嵌套
- 严重等级：**P1**。判级理由：`HandleFunctionCall` 对外部 `content->valuestring` 再 `cJSON_Parse`，并 `cJSON_ArrayForEach` + `cJSON_Duplicate(child,true)` 深拷贝任意层级；恶意深嵌套或超大数组放大内存、栈递归，高频 FC 下体验退化或栈溢出。
- 文件：`main/protocols/websocket_baidu_protocol.cc:1089-1130`
- 代码：
```cpp
cJSON* payload = cJSON_Parse(content->valuestring);   // content 来自网络
...
cJSON_ArrayForEach(item, param_list) {
    ... cJSON_AddItemToObject(arguments, child->string, cJSON_Duplicate(child, true)); }
```
- 根因：对外部二级 JSON 无大小/深度限制，`cJSON_Duplicate` 递归。
- 触发/影响：服务端下发深层嵌套 parameter_list → 递归栈增长（Core0 网络栈 8192B，见 1090 行附近任务），可栈溢出崩溃。
- 修复：解析前限制 `content` 长度（如 ≤4KB），`cJSON_Duplicate` 前限制 param 数量与层级；超限丢弃并日志。
- [发现于第二遍]

### 03-P1-I  baidu 一次性 idle_dc timer 回调内裸用 self 指针（析构竞态）
- 严重等级：**P1**。判级理由：`CheckIdleTimeout` 动态创建一次性 timer `idle_dc`，回调里先 `self->prevent_destroy_guard_` 取 guard 再判活；但取 `self->prevent_destroy_guard_` 这一步本身就解引用 `self`，若对象已析构则是悬空读。并发析构时偶发崩溃，量产返修最贵。
- 文件：`main/protocols/websocket_baidu_protocol.cc:1499-1521`
- 代码：
```cpp
auto t = xTimerCreate("idle_dc", pdMS_TO_TICKS(12000), pdFALSE, this,
  [](TimerHandle_t timer){
    auto* self = static_cast<WebsocketBaiduProtocol*>(pvTimerGetTimerID(timer));
    if (self) { auto g = self->prevent_destroy_guard_;   // 解引用 self（可能已悬空）
```
- 根因：guard 机制要先安全拿到 guard，但拿 guard 需先访问已可能销毁的 `self`；且析构未持有/删除此动态 timer（析构只删 idle_timer_handle_，不删运行中的 idle_dc）。
- 触发/影响：idle goodbye 流程进行中协议对象被销毁（切换协议/关机）→ 12s 后 timer 触发访问悬空 this。
- 修复：改用类成员固定 timer（与 keepalive/lic_retry 一致，析构统一 stop+delete），不在回调内动态 new timer；或把 guard 通过 timer ID 传入而非经 self。
- [发现于第二遍]

### 03-P2-J  baidu HandleBinaryMessage 的 drop_count 为非原子 static（多实例/并发竞态）
- 严重等级：**P2**。判级理由：`static int drop_count` 为函数级静态，跨实例共享且无锁，网络回调线程自增，竞态仅影响日志计数，不崩；属并发红线轻微项。
- 文件：`main/protocols/websocket_baidu_protocol.cc:812-816`
- 代码：`static int drop_count = 0; if (++drop_count % 50 == 1) {...}`
- 根因：用函数静态做计数，非 atomic。
- 触发/影响：仅日志频率不准，无功能危害。
- 修复：改 `static std::atomic<int>` 或成员变量。
- [发现于第二遍]

### 03-P2-K  MQTT UDP remote_sequence_ 防重放窗口可被绕过 + 解密前仅校验最小长度
- 严重等级：**P2**。判级理由：UDP 帧 `sequence < remote_sequence_` 才丢弃，攻击者用 `>` 旧序号或巨大跳变可注入；解密用 nonce(=帧前 16B 含 payload_len)作 CTR，长度仅校验 `>= sizeof(aes_nonce_)`，未交叉校验帧内声明的 payload_len 与实际长度。边缘但属外部输入边界。
- 文件：`main/protocols/mqtt_protocol.cc:258-289`
- 代码：
```cpp
if (data.size() < sizeof(aes_nonce_)) { ... return; }
... if (sequence < remote_sequence_) { ... return; }   // 仅挡更小序号
size_t decrypted_size = data.size() - aes_nonce_.size();
```
- 根因：无 payload_len 一致性校验、无序号上界/重放强校验、无 HMAC/AEAD（CTR 无完整性）。
- 触发/影响：UDP 明文可注入伪造加密帧解出垃圾 OPUS（解码器侧需鲁棒），或重放未来序号卡死后续真实帧。
- 修复：校验帧内 payload_len 与 `decrypted_size` 一致；序号窗口加上界；评估为 UDP 增加鉴别（MAC）。属安全增强，记排期。
- [发现于第二遍]

### 03-P3-L  DecodeHexString 奇数长度 / 非法字符静默处理（弱网/异常 hello）
- 严重等级：**P3**。判级理由：`i += 2` 循环对奇数长度访问 `hex_string[i+1]` 越界一字节（std::string 末尾有隐式 `\0`，读到的是终止符，技术上不 UB 但语义错误）；非法字符 `CharToHex` 返回 0 静默。远期/数据正确性。
- 文件：`main/protocols/mqtt_protocol.cc:398-406`
- 代码：`for (size_t i=0;i<hex_string.size();i+=2){ char b=(CharToHex(hex_string[i])<<4)|CharToHex(hex_string[i+1]); ...}`
- 根因：未校验偶数长度与字符合法性。
- 触发/影响：服务端 hello 的 key/nonce 长度异常 → AES key/nonce 错误，音频全程解密失败（静默白噪/无声）。
- 修复：进入前 `if (hex_string.size()%2) return "";`，非法字符返回空串并 `SetError`。
- [发现于第二遍]

### 03-P3-M  baidu/joyai SendAudio 失败重试在 Send 内同步阻塞热路径
- 严重等级：**P3**。判级理由：`websocket_->Send` 在 4G(ml307) 下可阻塞数秒，SendAudio 在音频上行热路径，虽有断路器(failures>=3)缓解，但首 3 次失败 + 一次快速重试仍可能在 Core0 累计阻塞，影响主循环节奏。技术债。
- 文件：`main/protocols/websocket_baidu_protocol.cc:246-255`、`websocket_joyai_protocol.cc:68-78`
- 根因：协议层在音频回调中做同步 Send + 重试，未与网络发送线程解耦。
- 触发/影响：弱网下上行抖动，非崩溃。
- 修复：长期评估上行音频改异步队列（带 size 上限）投递。记待办。
- [发现于第二遍]

---

## 第三遍 · 反审自检（复验 + 对抗视角补漏 / 删误报）

复验结论：
- 03-P0-A 行号(791-792)、片段、判级核对无误；对照 `websocket_protocol.cc:166` 与 `joyai:371`（先 `std::string s(data,len)` 安全）确认仅 baidu 裸 JSON 分支有此缺陷，**非误报**，维持 P0。
- 03-P0-G 标 [待确认]：本子系统确实无任何 TLS verify 代码，但最终是否安全取决于底层 `esp_ssl.cc`/`web_socket.cc` 默认值；判级保留 P0 但以"需确认底层默认"为前提，不拔高也不撤。
- 03-P1-C/D 复验：`websocket_protocol.cc:247` 在 transport==nullptr 时短路不会解引用，但 transport 为 number/object 时 `valuestring`=NULL，`strcmp(NULL,..)` 崩溃成立，维持 P1（非拔高）。
- 03-P1-B 复验：确认失败路径无 `websocket_.reset()`；下轮 `OpenAudioChannel` 顶部无清理（不同于 baidu 的 603-607 行先 Close+reset），泄漏成立，维持 P1。

对抗视角补漏（最高风险文件 websocket_baidu_protocol.cc 反查）：

### 03-P1-N  baidu HandleTextMessage 前缀分发对 [Q]: 兜底分支可负长度 substr
- 严重等级：**P1**。判级理由：`[Q]:` 兜底分支 `msg.substr(strlen(ASR_Q_FIN_PREFIX))` 即 `substr(4)`，进入该分支只保证 `len>4`，正常；但同函数 `[A]:` 兜底 `substr(strlen("[A]:"))`=substr(4) 同样 len>4 安全。重点风险在：当文本恰为 `[Q]:`（len==4）时不进入（要求 len>4），OK。但 **`HandleEvent` 收到的 `msg` 在 `event.find(...)` 全部 miss 时无害**。真正问题：`HandleAsrResult` 的扩展解析 `ext.substr(emo_pos+10,...)` 当 `emo_pos+10 > ext.length()` 时抛 `out_of_range`。
- 文件：`main/protocols/websocket_baidu_protocol.cc:854-858`
- 代码：
```cpp
size_t emo_pos = ext.find("[emotion]:");
if (emo_pos != std::string::npos) {
    size_t end = ext.find('&', emo_pos);
    emotion = ext.substr(emo_pos + 10, ...);   // emo_pos+10 可能 > ext.length()
```
- 根因：`find` 命中 "[emotion]:" 后假设其后必有内容；若 "[emotion]:" 出现在 ext 末尾（恰好 10 字符结尾），`emo_pos+10 == ext.length()` 时 `substr` 起点合法（返回空），但若服务端构造使匹配位置使起点 > length 不可能（find 命中保证 +10 ≤ length）。**复核为误报**：find 命中 "[emotion]:"（10 字符）保证 `emo_pos+10 ≤ ext.length()`，substr 起点合法。**撤销该条**，仅作记录不计入统计。
- [发现于第三遍 → 经复核撤销]

### 03-P1-O  baidu OpenAudioChannel 复用分支与网络线程 OnDisconnected 对 websocket_ 无锁竞争
- 严重等级：**P1**。判级理由：`OpenAudioChannel`（应用线程）读/复用 `websocket_`，而 `OnDisconnected`（网络线程）在 623-650 行 `media_ready_.exchange`、改 `device_info_sent_` 等；`websocket_` 本身是 `unique_ptr` 无 atomic 保护，复用判断 `if (websocket_ && websocket_->IsConnected())`(536) 与析构/重建(603-607)跨线程无锁，偶发 use-after-free。
- 文件：`main/protocols/websocket_baidu_protocol.cc:536-548` vs `603-607` vs `623-650`
- 根因：`websocket_` 生命周期跨网络回调线程与应用线程，仅靠 `opening_` CAS 挡同名重入，未挡 OnDisconnected 路径与 reset 的竞争。
- 触发/影响：连接复用瞬间服务端断开 → 网络线程回调与应用线程 reset 交错，偶发崩溃。
- 修复：`websocket_` 访问/重建加成员 mutex（参照 mqtt 的 `channel_mutex_`），或在主线程串行化所有 websocket_ 生命周期操作。
- [发现于第三遍]

### 03-P2-P  joyai SendText/SendAudio 依赖 IsTimeout()，120s 无下行即静默断流但无主动重连
- 严重等级：**P2**。判级理由：`IsAudioChannelOpened()` 含 `!IsTimeout()`（120s），超时后 SendText/SendAudio 直接 false 且 ping timer 仍在跑但 `IsAudioChannelOpened()` 已 false → ping 不发（line 32 提前 return），陷入"既不发心跳也不重连"的半死状态，需上层察觉才恢复。体验退化非崩溃。
- 文件：`main/protocols/websocket_joyai_protocol.cc:32, 116-118`
- 根因：超时判定与重连/心跳逻辑耦合，超时后无自愈路径。
- 触发/影响：长时间无服务端下行（如服务端 hang）→ 通道僵死直到用户重新唤醒。
- 修复：超时应主动 `CloseAudioChannel` + 通知上层重连，而非静默 false。
- [发现于第三遍]

### 03-P3-Q  mqtt 重连 timer 与 OnDisconnected 可叠加启动（多次 esp_timer_start_once）
- 严重等级：**P3**。判级理由：`OnDisconnected` 每次都 `esp_timer_start_once`，若短时间多次断开回调，对已 armed 的一次性 timer 重复 start，esp_timer 行为是重置而非叠加，无泄漏；潜在轻微。
- 文件：`main/protocols/mqtt_protocol.cc:85-91`
- 根因：未判 timer 是否已 armed。
- 触发/影响：仅重连节奏轻微抖动。
- 修复：start 前可 `esp_timer_stop`，或接受现状（记录）。
- [发现于第三遍]

误报删除记录：
- 03-P1-N（[emotion] substr 越界）经反审确认 `find` 命中保证起点合法，**撤销**，不计入统计。
- baidu `HandleTextMessage` 各前缀分支 `data+strlen(PREFIX)` / `len-strlen(PREFIX)`：均有 `len>4` 守卫且前缀≤12字符匹配前已 `len>8/12` 校验，复核安全，不列入。

---

## 统计汇总

| 等级 | 数量 | 编号 |
|------|------|------|
| P0 | 3 | 03-P0-A, 03-P0-G | （注：列两个标识，G 为 [待确认] 前提）|
| P0 | — | 实际 P0 计 **2** 条：03-P0-A、03-P0-G |
| P1 | 5 | 03-P1-B, 03-P1-C, 03-P1-D, 03-P1-H, 03-P1-I, 03-P1-O |
| P2 | 4 | 03-P2-E, 03-P2-F, 03-P2-J, 03-P2-K, 03-P2-P |
| P3 | 4 | 03-P3-L, 03-P3-M, 03-P3-Q |

精确计数（去除已撤销的 03-P1-N）：
- **P0：2**（03-P0-A、03-P0-G）
- **P1：6**（03-P1-B、03-P1-C、03-P1-D、03-P1-H、03-P1-I、03-P1-O）
- **P2：5**（03-P2-E、03-P2-F、03-P2-J、03-P2-K、03-P2-P）
- **P3：3**（03-P3-L、03-P3-M、03-P3-Q）
- **合计：16**

三遍新增分布：
- 第一遍（广度）：6（A、B、C、D、E、F）
- 第二遍（红线深挖）：7（G、H、I、J、K、L、M）
- 第三遍（反审自检）：3（O、P、Q；另撤销 1 条 N）
- 即 **6 + 7 + 3 = 16**

最严重 P0 一句话：
- **03-P0-A**：baidu 协议对裸 JSON 文本帧用 `cJSON_Parse(data)` 解析非 NUL 结尾的网络缓冲，堆越界读，远程可崩 —— 一行改 `cJSON_ParseWithLength(data, len)` 即修。
- **03-P0-G**：WS/MQTT/HTTP 协议层全程无 TLS 证书校验入口，若底层默认 skip-verify 则可中间人接管会话并注入恶意帧（[待确认] 底层默认值，属红线④需 Jack 拍板）。
