# 09 板级驱动 / 传感器 / I2C 总线 / 4G 模组 子系统审计

> 审计范围：`main/boards/common/`（board/button/camera/i2c_device/dual_network_board/ml307_board/wifi_board/lamp_controller/education_mcp_tools）、
> `components/mydazy__i2c_bus_worker/`、`components/mydazy__esp_sc7a20h/`、`components/esp_lcd_touch_axs5106l/`、`components/78__esp-ml307/`、
> P30-4G / P30-WiFi 的 config + layout。
> 电源域（battery/charge/adc/分压/deep_sleep 切片）归 08 电源 agent，本文不重复判级。
> 关注点：I2C 总线竞争/并发、外设 init 失败处理、相机/触摸/加速度计/AT 帧外部输入边界、AT 模组超时与缓冲、GPIO 映射。

**已读文件数：26 个源/头/配置文件**
（i2c_bus_worker.c+.h、sc7a20h.c+.h、axs5106l_touch.c、axs5106l_upgrade.c、i2c_device.cc+.h、board.cc、button.cc、camera.h、dual_network_board.cc、ml307_board.cc、wifi_board.cc、education_mcp_tools.cc、at_uart.cc+.h、at_modem.cc、ml307_at_modem.cc、ml307_tcp.cc、ml307_udp.cc、ml307_mqtt.cc、ml307_http.cc、p30-4g/config.h+config.json+layout.json、p30-wifi/config.json）

---

## 第一遍 · 广度遍历（显性缺陷）

### 09-P0-A · MHTTPURC 回调裸索引 arguments[0]/arguments[1]，无任何 size 检查
- **等级 P0**：来自 4G 模组下行的外部 AT 帧，畸形/分包 URC 即触发 `std::vector::operator[]` 越界读 → 崩溃。OTA / 笔顺 GIF / 服务端解绑都走此路径，量产现场弱网/脏数据高发。属内存安全红线。
- **文件**：`components/78__esp-ml307/src/ml307/ml307_http.cc:13-14`
- **代码**：
```cpp
if (command == "MHTTPURC") {
    if (arguments[1].int_value == http_id_) {     // 无 size 检查
        auto& type = arguments[0].string_value;
```
- **根因**：注册的 URC 回调直接索引 `arguments[1]`、`arguments[0]`，而 `ParseResponse()` 对空字段/分包行可能产出 0~1 个 argument。同函数后续 `header`/`content` 分支虽有 `arguments.size()>=5/6` 守卫，但入口的 `[1]`/`[0]` 没有。
- **触发条件**：模组发出 `+MHTTPURC:`（冒号后无值或仅 1 段），或 RX FIFO 溢出后半行残留被解析。
- **修复**：在 `command == "MHTTPURC"` 后立即加 `if (arguments.size() < 2) return;`，再访问 `[0]/[1]`。

### 09-P0-B · MIPURC "rtcp" 分支访问 arguments[3]，但守卫只到 size>=3
- **等级 P0**：TCP（含 WS/MQTT-over-TCP 业务主链路）收数据帧时越界读，畸形 4G 帧直接崩。
- **文件**：`components/78__esp-ml307/src/ml307/ml307_tcp.cc:32-36`
- **代码**：
```cpp
} else if (command == "MIPURC" && arguments.size() >= 3) {   // 只保证 0,1,2
    if (arguments[1].int_value == tcp_id_) {
        if (arguments[0].string_value == "rtcp") {
            stream_callback_(at_uart_->DecodeHex(arguments[3].string_value)); // [3] 越界
```
- **根因**：`+MIPURC: "rtcp",<id>,<len>,<data>` 正常为 4 段（index 0-3），但守卫写成 `>= 3`，当模组只发 3 段（截断/分包）时 `arguments[3]` 越界。
- **触发条件**：rtcp 帧被分包、或 len 段后 data 段缺失。
- **修复**：将守卫改为 `arguments.size() >= 4`（或在 rtcp 分支内 `if (arguments.size() < 4) return;`）。对比 `ml307_udp.cc:34` 用的是 `== 4`，是正确写法。

### 09-P0-C · MQTTURC "conn" 分支访问 arguments[2]，守卫只到 size>=2
- **等级 P0**：MQTT 是设备与服务端的控制通道，conn URC 越界读会崩。
- **文件**：`components/78__esp-ml307/src/ml307/ml307_mqtt.cc:10-14`
- **代码**：
```cpp
if (command == "MQTTURC" && arguments.size() >= 2) {
    if (arguments[1].int_value == mqtt_id_) {
        auto type = arguments[0].string_value;
        if (type == "conn") {
            int error_code = arguments[2].int_value;   // size 可能恰为 2 → [2] 越界
```
- **根因**：`conn` 子类型需要 index 2，但外层守卫只保证 0,1 存在。
- **触发条件**：模组发 `+MQTTURC: "conn",<id>`（缺 error_code 段，弱网/固件差异常见）。
- **修复**：`conn` 分支内加 `if (arguments.size() < 3) break;`（`publish` 分支已有 `>=7` 守卫，是对的）。

### 09-P0-D · CME ERROR 分支裸读 arguments[0]，无 size 检查
- **等级 P0**：任何带空参数的 `+CME ERROR:` URC 越界读崩溃；CME ERROR 在 SIM/注册异常时高频出现。
- **文件**：`components/78__esp-ml307/src/at_modem.cc:551-555`
- **代码**：
```cpp
void AtModem::HandleUrc(...) {
    if (command == "CME ERROR") {
        cme_error_code_ = arguments[0].int_value;   // 无 size 检查
```
- **根因**：`ParseResponse` 解析 `+CME ERROR:` 后冒号无值时 arguments 可能为空。
- **触发条件**：`+CME ERROR:`（冒号后空）或非数字错误码导致 0 个 argument。
- **修复**：`if (command == "CME ERROR") { if (!arguments.empty()) cme_error_code_ = arguments[0].int_value; xEventGroupSetBits(...); return; }`

### 09-P1-E · CEREG 参数索引可越界（state_index+1/+2/+3 未充分校验）
- **等级 P1**：注册状态 URC 高频，索引算到 `state_index+2`/`+3` 时上界判断有逻辑漏洞，畸形帧偶发越界读。判 P1 而非 P0：正常运营商 CEREG 段数稳定，越界需特定畸形帧。
- **文件**：`components/78__esp-ml307/src/at_modem.cc:193-201`
- **代码**：
```cpp
cereg_state_.stat = arguments[state_index].int_value;
if (arguments.size() >= state_index + 2) {
    cereg_state_.tac = arguments[state_index + 1].string_value;
    cereg_state_.ci  = arguments[state_index + 2].string_value;   // 需 size>=state_index+3 才安全
```
- **根因**：进入块的条件是 `size() >= state_index+2`（保证到 index `state_index+1`），但块内访问了 `arguments[state_index+2]`，差 1。
- **触发条件**：CEREG 返回恰好 `state_index+2` 个参数（有 tac 无 ci）。
- **修复**：把守卫改为 `if (arguments.size() >= (size_t)state_index + 3)` 后再读 tac/ci；AcT 那层 `>= state_index+4` 已正确。

### 09-P1-F · ML307 URC 回调全部在 EventTask 中跨实例无锁访问连接状态标志
- **等级 P1**：`connected_`、`instance_active_`、`body_`、`message_payload_` 等被 EventTask（URC 回调）写、被业务线程（Send/Read/Connect）读，多数无锁或仅 `body_` 有 mutex。`std::string body_.append`（http content 回调）与主线程 `Read` 持同一 mutex 是对的，但 `connected_`/`instance_active_` 是裸 bool。
- **文件**：`ml307_tcp.cc:13/45/53`、`ml307_udp.cc:15/27`、`ml307_mqtt.cc:17/25/59`
- **根因**：URC 回调运行于 `AtUart::EventTask`（Core 0, prio 5），业务调用方在其它任务，跨任务共享裸 bool 未用 atomic（违反项目 `esp32-memory.md` "多核共享变量用 std::atomic"）。
- **触发条件**：连接/断开瞬间业务线程并发读 `connected_` 拿到撕裂/陈旧值，偶发误判已连接而发送 → 失败重试。
- **修复**：将 `connected_`/`instance_active_` 改为 `std::atomic<bool>`；或确认这些标志只在单一任务读写（当前不是）。

### 09-P2-G · I2cDevice（裸 IDF 路径）所有读写用 ESP_ERROR_CHECK，I2C 失败即 abort 重启
- **等级 P2**：`I2cDevice` 类是旧的直连 IDF 路径（非 worker），任一 I2C 事务失败直接 `ESP_ERROR_CHECK` → abort。量产中 I2C 偶发 NACK 会导致整机重启而非降级。判 P2：本项目主路径已迁移到 i2c_bus_worker，此类是否仍被实例化需确认；若仍在用则升 P1。
- **文件**：`main/boards/common/i2c_device.cc:18-19,24,29,34`
- **代码**：`ESP_ERROR_CHECK(i2c_master_transmit(...))` / 构造里 `ESP_ERROR_CHECK(i2c_master_bus_add_device(...)); assert(...)`
- **根因**：库默认实现把可恢复的总线错误当致命错误。
- **触发条件**：任何 codec/外设 I2C 单次失败。
- **修复**：改为返回 esp_err_t 让调用方决定；或在本项目里确认 `I2cDevice` 不再被任何 board 使用并移除。[待确认其是否仍被实例化]

### 09-P2-H · UART 接收链路 std::string 无上限，畸形流可致 rx_buffer_ 膨胀
- **等级 P2**：`rx_buffer_.append` 在 `MHTTPURC ind` 缺换行时会 `insert/append \r\n` 反复尝试解析；若模组持续吐无 `\r\n` 的脏流，`rx_buffer_` 可增长到 RX ring（8KB）量级反复搬移。判 P2：有 UART_BUFFER_FULL/FIFO_OVF 兜底清空，膨胀有天花板，但 CPU 抖动明显。
- **文件**：`components/78__esp-ml307/src/at_uart.cc:140-148, 267-284`
- **根因**：解析依赖 `\r\n` 边界，分包/脏数据时 `ParseResponse` 返回 false 不消费，数据滞留。
- **触发条件**：弱网 + 模组吐半行 binary/脏数据。
- **修复**：给 `rx_buffer_` 设硬上限（如 16KB），超限即 clear + 触发 reconnect（复用现有 `__UART_OVERFLOW__` 路径）。

---

## 第二遍 · 红线深挖（内存安全 / 并发 / 外部输入 / OTA）

### 09-P1-I · 笔顺 GIF 下载缓冲整段交给 LVGL，GIF 校验仅查首尾，畸形内部可崩 LVGL 解码器
- **等级 P1**：`education_mcp_tools.cc` 校验了 `GIF89a/87a` 头 6 字节 + 末字节 `0x3B` + 最小 1024B，但 GIF 内部 LZW/帧数据损坏时 LVGL gif 解码仍可能越界。属外部输入（云端 GIF）边界。判 P1：有最小校验+大小上限（512KB），降低了概率，但内部结构未验。
- **文件**：`main/boards/common/education_mcp_tools.cc:163-167, 202`
- **代码**：`bool valid = ok && gsz>=1024 && (memcmp(gif,"GIF89a",6)==0||...) && gif[gsz-1]==0x3B;`
- **根因**：首尾签名不能保证内部数据完整；下载源是公网 bcebos，可能被 CDN 篡改/截断中段。
- **触发条件**：服务端/CDN 返回头尾合法但中段损坏的 GIF。
- **修复**：已是合理工程折中，建议：① 依赖 LVGL gif 解码器自身边界（确认其对损坏数据有防御）② 或加 CRC/长度与 Content-Length 比对。保留为 P1 待办，至少在 `FontGif` 装载失败时不崩。

### 09-P2-J · axs5106l 触摸帧坐标边界校验偏宽，raw 值进 LVGL 前仅 clamp 不拒绝
- **等级 P2**：`read_touch` 拒绝了 `0xFFF/0xFFF`、`>MAX+50` 与全 0xFF，但 `raw_x` 在 (TOUCH_MAX_X, TOUCH_MAX_X+50] 区间会被 clamp 到 width-1 当成合法触摸。RF 注入的边缘坐标可能被当真实点。判 P2：有速度门限 + storm 检测 + RF debounce 多层兜底，单层宽松不致崩。
- **文件**：`components/esp_lcd_touch_axs5106l/axs5106l_touch.c:660-689`
- **根因**：容忍 +50 余量是为兼容固件偏移，但与 clamp 组合后边缘噪声不被丢弃。
- **触发条件**：4G RF 注入坐标落在 [285,334]。
- **修复**：边界外直接 return false（已被 storm/速度层大幅缓解，优先级低）。

### 09-P2-K · axs5106l firmware 烧录无写后校验，write_flash 永远返回 true
- **等级 P2**：`write_flash` 逐字节写后没有 readback 校验（注释明说"Verification omitted by default"），`do_upgrade` 后仅靠重读版本号判断成功。若中途某字节写错但版本字恰好对，会得到带损坏 firmware 的触摸芯片。判 P2：烧录仅在版本不一致时触发（首烧/换版），非常态路径；且后续读版本号有兜底。
- **文件**：`components/esp_lcd_touch_axs5106l/axs5106l_upgrade.c:227-266, 277-278`
- **根因**：为字节级慢速烧录省时间跳过校验。
- **触发条件**：烧录期间 I2C 抖动写错中段字节。
- **修复**：烧录完成后做一次整片 readback CRC 比对，失败则重烧（UPGRADE_RETRY_TIMES 当前=1，几乎无重试余量，建议同时提到 2-3）。

### 09-P2-L · i2c_bus_worker 提交超时后接受信号量 leak 以换 UAF 安全
- **等级 P2**：`submit_and_wait` 在 worker 卡死 >200ms 时主动 leak 栈上 `StaticSemaphore_t`（实际是放弃 give，因 static 不能 delete 但注释写"接受 leak"）。设计是对的（避免 UAF），但每次 worker 硬卡都漏一次同步对象，长期可耗尽。判 P2：需要 worker 真正卡死才触发，是异常路径。
- **文件**：`components/mydazy__i2c_bus_worker/i2c_bus_worker.c:259-276`
- **根因**：`StaticSemaphore_t sem_buf` 在 caller 栈上，worker 仍可能 give 已离开作用域的 sem → 这才是真 UAF 风险点（栈对象生命周期）。当前靠"再等 200ms"降低概率，但不能根除：若 worker 在 200ms 后才 give，栈早已回收 → UAF 写。
- **触发条件**：worker 被 i2c_master 硬件超时卡住 > (tick_wait + 200ms)。
- **修复**：result_sem 不要放 caller 栈（用堆分配 + worker give 后由 worker 释放，或引用计数），彻底消除栈对象 UAF 窗口。[红线·内存安全，建议升 P1 复核]

### 09-P3-M · sc7a20h init 在持 session_lock 期间多次 vTaskDelay
- **等级 P3**：`sc7a20h_init` 在 `i2c_worker_lock_session` 与 `unlock` 之间 `write_reg`（每条走 worker 同步阻塞），其中 `REG_CTRL_REG1` 后有 10ms vTaskDelay 在锁外，但整个 10 寄存器序列持会话锁，期间其它 driver（codec/touch）I2C 全部排队。init 是一次性的，影响有限。
- **文件**：`components/mydazy__esp_sc7a20h/sc7a20h.c:235-251`
- **根因**：原子寄存器序列必须持锁，合理；但若任一 write 失败无返回检查（write_reg 返回值被忽略），可能配置不完整仍上报 init ok。
- **触发条件**：init 期间某寄存器写失败。
- **修复**：检查 write_reg 返回值，失败则 unlock + 返回 NULL（当前完全忽略返回值）。

---

## 第三遍 · 反审自检 + 对抗视角补漏

### 复验结论（行号/片段/判级核对）
- 09-P0-A/B/C/D：逐一回核源码行号与守卫值，确认越界路径真实存在。对比 `ml307_udp.cc:34`（`==4`）与 `ml307_tcp.cc:32`（`>=3`），同一 rtcp/rudp 语义两处守卫不一致，进一步佐证 tcp 处是 bug 而非有意设计。**判级维持 P0**（外部网络输入 + 必崩路径 + 主链路）。
- 09-P1-E：核对 `state_index` 取值（0 或 1），块内确实访问到 `+2`，守卫差 1，维持 P1。
- 09-P2-G：`I2cDevice` 是否仍被实例化未在本范围文件确认，故维持 P2 并标 [待确认]，不拔高。
- 09-P2-L：复看后认为栈上 `StaticSemaphore_t` + worker 延迟 give 是真实 UAF 窗口，已在条目内注明建议升 P1 复核，但因触发需 worker 硬卡（低频）暂记 P2。

### 对抗视角新发现

### 09-P1-N · ml307_http MHTTPURC content 分支非 chunked EOF 判定读 arguments[3]/[2]，前置守卫仅 size>=6 或 ==特例
- **等级 P1**：content 分支在 `arguments.size()>=6` 后访问 `arguments[3].int_value`/`arguments[2].int_value`（line 64,69,73）做 EOF 与丢包判定——index 在 6 段内是安全的；但 `arguments.size()>=5 && arguments[4].int_value==0`（line 44）的 cur_len=0 分支之后，line 60-76 的 `eof_`/`body_offset_` 逻辑仍会执行并读 `arguments[3]/[2]`，此时 size 可能仅为 5（index 0-4 合法，[3][2] 合法）→ 实际安全。**复核后认定为非越界**，撤回越界判定；但 line 64 `arguments[3] >= arguments[2]`（cur vs sum）在分包乱序时逻辑可提前置 eof 截断 body → 数据截断而非崩溃。降为 P1 数据完整性问题。
- **文件**：`ml307_http.cc:60-76`
- **修复**：EOF 判定增加 `response_chunked_` 与 content-length 双重确认，避免分包乱序提前 EOF。

### 09-P1-O · MIPURC 在 tcp/udp 中 stream/message callback 内调 DecodeHex，未限制 data 段长度
- **等级 P1**：`DecodeHex` 对外部 `arguments[3].string_value`（4G 下行 HEX）无长度上限，单帧超大可瞬时分配大 std::string。判 P1：模组单 URC 有固有长度限制（受 RX ring 8KB 约束），但 binary 模式下可更大。
- **文件**：`ml307_tcp.cc:36`、`ml307_udp.cc:38`
- **根因**：解码缓冲随外部输入大小线性增长，无封顶（违反 `esp32-memory.md` 队列/容器需 size 上限）。
- **修复**：DecodeHex 前检查 `arguments[3].string_value.size()` 上限（如 ≤ 4KB），超限丢帧并触发重连。

### 09-P2-P · DISPLAY_LCD_TE 标注"预留未使用"但代码启用 TE 同步
- **等级 P2**：`config.h:67` 注释 `DISPLAY_LCD_TE GPIO_NUM_40 // ...预留未使用`，但 `mydazy_p30_board.cc:663` 调用 `EnableTearingEffectSync(DISPLAY_LCD_TE)`。若硬件 GPIO40 实际未接 TE 信号，TE 同步会等不到信号 → 刷新阻塞或撕裂。注释与代码矛盾，量产前必须实测确认 GPIO40 是否接 TE。
- **文件**：`main/boards/mydazy-p30-4g/config.h:67` vs `mydazy_p30_board.cc:663`
- **修复**：核对原理图，更新注释或关闭 TE 同步。[待确认硬件连接]

### 09-P3-Q · ml307 NetworkTask 注册失败后无回退，wifi 配网超时进深睡但 4G 无对称兜底
- **等级 P3**：`Ml307Board::NetworkTask` 在 `NETWORK_REG_MAX_RETRIES`(6×10s=60s) 后若仍未注册，仅打 LOG 返回，不进配网/不重试/不深睡，设备停在"无网"状态空耗电。判 P3：远期体验问题，非崩溃。
- **文件**：`main/boards/common/ml307_board.cc:107-126`
- **修复**：注册彻底失败后给用户提示 + 进入低功耗或重启重试策略。

### 09-P3-R · button.cc OnLongPress/OnMultipleClick 每次注册 new CallbackContext 无释放
- **等级 P3**：`new CallbackContext{...}` 传给 iot_button 作 usr_data，Button 析构时未 delete 这些 context（iot_button_delete 不负责释放用户 data）→ 每注册一次泄漏一个小对象。判 P3：注册是一次性初始化，泄漏量固定且小，但违反 `esp32-memory.md` "所有 new 有对应释放"。
- **文件**：`main/boards/common/button.cc:81, 146`
- **修复**：在 Button 析构里记录并 delete 这些 context，或改用成员存储替代裸 new。

---

## 统计（最终口径，以此为准）

| 等级 | 数量 | 编号 |
|------|------|------|
| P0 | 4 | A, B, C, D |
| P1 | 5 | E, F, I, N, O |
| P2 | 6 | G, H, J, K, L, P |
| P3 | 4 | M, Q, R, （Q/R/M）|
| **合计** | **19** | A–R |

唯一问题编号 A–R 共 **19 个**（A B C D E F G H I J K L M N O P Q R = 18 字母，其中 M、Q、R、P 等均为独立条目；按字母去重 = 18，下方按发现遍次列明）。

- **P0 = 4**：A（HTTP MHTTPURC 裸索引）、B（TCP rtcp 越界 size>=3 读[3]）、C（MQTT conn 越界 size>=2 读[2]）、D（CME ERROR 裸读[0]）
- **P1 = 5**：E（CEREG 索引差 1）、F（连接标志非 atomic 跨任务）、I（GIF 内部未校验）、N（分包乱序提前 EOF 截断 body）、O（DecodeHex 无长度上限）
- **P2 = 6**：G（I2cDevice ESP_ERROR_CHECK abort）、H（rx_buffer 无上限）、J（触摸边界偏宽 +50 后 clamp）、K（firmware 无写后校验）、L（worker 栈 semaphore 延迟 give UAF 窗口）、P（TE 引脚注释/代码矛盾）
- **P3 = 3**：M（sc7a20h init 忽略 write_reg 返回值）、Q（4G 注册失败无兜底）、R（button CallbackContext 泄漏）

**合计 = 18 个唯一问题（P0=4 / P1=5 / P2=6 / P3=3）**

### 三遍新增分布
- **第一遍（广度遍历）新增 8 个**：A, B, C, D, E, F, G, H
- **第二遍（红线深挖）新增 5 个**：I, J, K, L, M
- **第三遍（反审 + 对抗）新增 5 个**：N, O, P, Q, R（其中 N 经复核由"疑似越界 P0"撤回为"数据完整性 P1"，删 1 处误报）
- 8 + 5 + 5 = **18 条独立条目**

---

## 出货门禁建议（本子系统）
1. **P0 必须出货前清零**：A/B/C/D 都是一行守卫即可修（加 `arguments.size()` 检查），是离崩溃最近、改动最小的一组，**优先于一切**。四处都是 4G 下行外部帧越界读，弱网/脏数据现场必现。
2. P1 中 F（atomic）、O（DecodeHex 封顶）属红线（并发 + 外部输入容器封顶），建议与 P0 同批处理。
3. P2-L（worker 栈 semaphore UAF 窗口）建议单独实测复核，确认是否需升级。
