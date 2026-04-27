---
name: xiaozhi-protocol
description: xiaozhi-esp32 协议规范（BinaryProtocol2/3、MQTT topic、WebSocket 消息、OTA、已售出设备兼容性矩阵）。场景触发词：协议、OTA、MQTT、WebSocket、绑定、topic、二进制协议、消息格式、版本兼容。自动被 network-stack-expert / firmware-debugger 加载。
---

# xiaozhi-esp32 协议规范

本 skill 由 Claude Code 在处理协议、OTA、服务端通信相关任务时自动加载。

---

## 一、协议栈概览

```
应用层:    JSON 控制消息 + 二进制音频流
         ├─ BinaryProtocol2（时间戳 + 负载类型）
         └─ BinaryProtocol3（简化版）

传输层:    ┌─ WebSocket (TLS)  ─ 主要
         └─ MQTT + UDP      ─ 备用
                ├─ MQTT: 控制消息 QoS 1
                └─ UDP: 音频流低延迟

网络层:    WiFi / 4G (ML307 Cat.1)
```

---

## 二、BinaryProtocol2 / 3（已冻结）

### 2.1 为什么冻结
- 已售出设备运行这些协议版本
- 改字段 = 断老设备 = 售后事故
- **铁律**：字段、类型、顺序都不能改
- 只能**新增**新协议版本（v4 / v5），老版本保留兼容

### 2.2 BinaryProtocol2 结构（参考）
```
┌─────────┬──────────┬──────────┬──────────┬─────────┐
│ version │ type     │ reserved │ payload  │ payload │
│ 2B      │ 2B       │ 4B       │ size 4B  │ ...     │
└─────────┴──────────┴──────────┴──────────┴─────────┘

type:
  0: 音频 opus 帧
  1: JSON 控制消息
  ...
```

### 2.3 BinaryProtocol3（简化版）
- 去掉了 reserved / 时间戳部分字段
- 新设备默认使用
- 服务端自动识别版本

### 2.4 兼容规则
- 新固件必须能解析 v2 和 v3
- 发出去的数据根据"设备注册时的 protocol version"决定用哪个
- 禁止单方面升级 → 必须服务端 + 客户端协同

---

## 三、WebSocket 消息

### 3.1 连接
- URL：从设备配置 / OTA bind 返回
- 子协议：`"xiaozhi"`
- 认证：Header `Authorization: Bearer <token>`
- TLS：必须，证书校验可选（生产建议开）

### 3.2 消息类型
- **JSON 控制**：文本帧，UTF-8
  ```json
  {"type": "hello", "version": 1, "device_id": "xxx"}
  {"type": "tts", "text": "你好"}
  {"type": "mcp", "method": "...", "params": {}}
  ```
- **音频**：二进制帧，BinaryProtocol2/3 封装
  - 上行：opus 编码的麦克风音频
  - 下行：opus 编码的 TTS 音频

### 3.3 心跳
- 应用层 ping：30s 周期
- TCP keepalive：TLS 层启用，60s
- 30s 无响应 → 判定掉线，指数退避重连

### 3.4 重连
- 指数退避：1s → 2s → 4s → 8s → ... → 60s 封顶
- 重连次数上限：连续 60 次失败 → 长休眠 5min 再试
- 成功后**必须重新 hello + 订阅**

---

## 四、MQTT + UDP（备用通道）

### 4.1 使用场景
- WebSocket 无法建立（如特殊网络环境）
- 需要 QoS 保证的控制消息

### 4.2 MQTT Topic 规范
```
xiaozhi/device/<device_id>/hello           # 上线
xiaozhi/device/<device_id>/bye             # 下线
xiaozhi/device/<device_id>/cmd             # 云→设备控制
xiaozhi/device/<device_id>/state           # 设备→云状态
xiaozhi/device/<device_id>/ota             # OTA 推送
```
- **冻结**：topic 结构已固化，改了断老设备
- 新增消息类型 → 用 `type` 字段区分，不要开新 topic

### 4.3 QoS
- 控制消息：QoS 1（至少一次）
- 状态上报：QoS 0（允许丢）
- 遗嘱消息：QoS 1，断线自动上报 offline

### 4.4 UDP 音频流
- 端口：服务端下发
- 分片：4KB / 帧
- 重传：**不重传**（实时优先，丢了就丢）
- 超时：5s 无 rx 判定通道失效

---

## 五、OTA 升级

### 5.1 流程
```
1. 设备定期（或被触发）请求 OTA server
   GET /ota?device_id=xxx&version=2.2.5
2. server 返回：
   {
     "has_update": true,
     "version": "2.2.6",
     "url": "https://...",
     "size": 1234567,
     "sha256": "..."
   }
3. 设备下载 → 写 OTA 分区 → 校验 → boot 新分区
4. 新版本启动 → 上报激活 → server 记录
```

### 5.2 分区表（冻结）
- `partitions/v2/` 已固化
- 改分区表 = 改存储布局 = 已售设备 OTA 崩溃
- 绝对禁止改

### 5.3 回滚
- 新版本启动后 **N 秒内**必须上报"启动成功"，否则 bootloader 自动回滚到上一个可用分区
- 实现：`esp_ota_mark_app_valid_cancel_rollback()`

### 5.4 差分升级
- 大包直接拉取风险大（断网 / 存储）
- 小版本用差分包（bsdiff / 自研）
- 本项目当前支持全量，差分待评估

### 5.5 OTA 过程中的 PSRAM 栈陷阱
- OTA 写 flash 时会 `spi_flash_op_lock`
- 期间 PSRAM 访问失败
- **所以 OTA task 栈**必须内部 RAM
- **同时**，所有持续循环 Core0 任务也必须内部 RAM 栈（见 CLAUDE.md）

---

## 六、设备绑定 / 激活

### 6.1 首次激活（禁区模块）
1. 设备出厂 → 读取 MAC / 唯一 ID
2. 首次联网 → POST `/activate`，server 返回 device_id + token
3. token 存 NVS
4. 之后所有通信用 token

### 6.2 用户绑定
1. 用户 APP 扫码（BluFi 返回的设备码）
2. APP 告诉 server："这个 device_id 属于我"
3. server 绑定 → 下次设备通信带 user_id 上下文

### 6.3 铁律
- 绑定 / 激活流程 = **禁区模块**（CLAUDE.md 第 7 节）
- 协议字段、token 格式、NVS key 全部冻结
- 改这里 = 老设备失联

---

## 七、已售出设备兼容性矩阵

| 版本 | 出货量 | 协议 | OTA 起点 | 备注 |
|---|---|---|---|---|
| v1.9.2 | 部分 P30 存量 | v2 | 手动烧录 | v1 分支冻结 |
| v2.2.3 | 早期 P30-4G | v2/v3 | 可 OTA 到 v2.2.x | — |
| v2.2.5 | 当前主力 | v3 | 可 OTA | 本仓库版本 |

### 兼容策略
1. **新版固件必须解析老协议**（v2 + v3）
2. **发出去用设备注册时的协议版本**
3. **NVS key 只增不改**（改名 = 老设备读不到 = 丢数据）
4. **分区表冻结**
5. **BluFi / 配网流程冻结**

---

## 八、常见协议坑

| 现象 | 根因 | 解决 |
|---|---|---|
| 升级后断线 | 改了 topic / 协议字段 | 立即回滚，恢复老字段 |
| 绑定失败 | 改了 activate 流程 | 禁区模块，恢复原实现 |
| 老设备无法 OTA | 改了分区表 / OTA URL | 禁区，恢复 |
| 音频卡顿 | BinaryProtocol 字段序搞错 | 对比 v2/v3 spec |
| MQTT 上线慢 | keepalive 太长 / QoS 1 重复 ACK | 调整参数 |

---

## 九、参考文档

- `docs/mqtt-udp.md` — MQTT+UDP 协议
- `docs/websocket.md` — WebSocket 协议
- `docs/mcp-protocol.md` — MCP 协议（设备端）
- `docs/mcp-usage.md` — MCP 用法
- `partitions/v2/README.md` — 分区表说明（如存在）

---

## 十、协议改动审批流程

**任何协议变更**（哪怕"只是加字段"）必须：
1. 先写改动说明（动什么、为什么）
2. Jack 批准
3. 后端先上线（向前兼容老设备）
4. 新设备固件灰度出货
5. 监控 1 周无异常
6. 全量发布

**禁止**：
- 设备端单方面改协议
- 为了"代码更优雅"重构协议层
- 不协商就删字段
