---
description: 系统化诊断固件 bug。依次调用 firmware-debugger 做初步分析，并根据 bug 类型派发到 lvgl-ui-fixer / rtos-task-auditor / network-stack-expert，输出统一格式的诊断报告。
argument-hint: "<bug 现象描述>"
allowed-tools: Task, Read, Grep, Glob
---

# /diagnose-bug — 固件 bug 系统诊断

用户输入: $ARGUMENTS

---

## 执行步骤（按顺序，不许跳）

### Step 1：初步分类

基于用户描述的现象，判断 bug **主类型**：

| 现象关键词 | 派发 agent |
|---|---|
| 页面 / UI / 触屏 / 黑屏 / 卡顿 / 文字 / 动画 / 渲染 | `lvgl-ui-fixer` |
| 死机 / 重启 / 崩溃 / 栈溢出 / watchdog / 优先级 / 死锁 | `rtos-task-auditor` |
| WiFi / 4G / BLE / MQTT / WS / 连接 / 断网 / DNS / TLS | `network-stack-expert` |
| 其他 / 边界不清 | `firmware-debugger` 先做初筛 |

如果归类不清，**先调 `firmware-debugger` 做初步定位**，它会判断要不要再派发。

### Step 2：调用对应 agent

用 Task 工具，`subagent_type` 对应上面的 agent。prompt 要包含：
1. 完整复述 `$ARGUMENTS`
2. 告诉 agent "这是 /diagnose-bug 流程，按你的输出格式完整走一遍"
3. 强调："不许猜着改，先出假设 + 加日志方案，等 Jack 的真实日志"

**可能需要并行调用多个 agent**：如现象同时涉及 UI 和任务（"切页面时卡死"），就同时调 `lvgl-ui-fixer` + `rtos-task-auditor`，合并输出。

### Step 3：合并输出统一诊断报告

格式如下：

```
# 🔍 诊断报告

## 0. Bug 摘要
- 现象：<一句话>
- 涉及 SKU：<...>
- 优先级：<P0 / P1 / P2，由 agent 判断>
- 类型：<UI / RTOS / 网络 / 其他>

## 1. 主 agent 分析
<firmware-debugger 或对应专家的输出>

## 2. 辅助 agent 补充（如有）
<其他 agent 的输出>

## 3. 可疑根因（合并去重）
A. ... (<概率>)
B. ...
C. ...

## 4. 下一步：加日志方案
<具体 diff，可直接 copy 烧录>
<烧录指令 / 复现步骤 / 需要收集什么 log>

## 5. 后续流程指引
- 拿到真实日志后 → `/fix-p0 <bug描述>` 进入修复流程
- 如果现象升级为"量产阻断"，立即转 P0 处理
- 如果涉及禁区模块，Jack 必须显式批准才能推进
```

---

## 硬性约束

1. **不许跳过 agent 直接自己分析** —— 即使你"觉得明显"
2. **不许出修复补丁**，这一步只做诊断
3. 涉及多层问题 → **一次派发多个 agent 并行**，不要串行浪费时间
4. 报告必须遵循本命令的输出格式
5. 所有 agent 的铁律在此命令中同样生效
