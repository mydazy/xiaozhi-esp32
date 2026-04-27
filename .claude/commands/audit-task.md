---
description: 一键跑 FreeRTOS 全量任务审计。输出任务清单表（栈大小/水位/优先级/core/状态）、队列锁清单、高危项、建议动作。
allowed-tools: Task, Read, Grep, Glob, Bash
---

# /audit-task — FreeRTOS 任务全量审计

---

## 执行步骤

### Step 1：调用 rtos-task-auditor agent

用 Task 工具，`subagent_type=rtos-task-auditor`。prompt 为：

```
请跑一次 xiaozhi-esp32 的全量任务审计。

目标：
1. 列出所有 xTaskCreate / xTaskCreatePinnedToCore / xTaskCreateWithCaps 的任务
2. 对每个任务记录：name、创建位置（文件:行）、栈大小、优先级、core、栈类型（internal/PSRAM）
3. 对照 CLAUDE.md 第 3.1 节的任务优先级表，检查是否违反
4. 列出所有 xQueueCreate、xSemaphoreCreate、portENTER_CRITICAL
5. 标记高危项（PSRAM 栈 + Core0 持续循环、portMAX_DELAY 裸用、中断里调 LVGL 等）
6. 如果需要运行时数据（实测栈水位），明确告诉我需要 Jack 烧录什么命令

输出你的标准"任务清单表 + 高危项报告"格式。
```

### Step 2：补充结构化分析

在 agent 输出之上补充：

```
## 📊 审计总览
- 总任务数：<n>
- 高危项（🔴）：<n>
- 中危项（⚠️）：<n>
- PSRAM 栈任务：<n>
- 内部 RAM 栈任务：<n>
- 违反 CLAUDE.md 优先级表的：<n>

## 🔍 核心发现
<前 3 个最需要关注的问题，具体到文件行号>

## 🛠️ 建议动作

### 立即（本次迭代必须处理）
<🔴 项修复方案概要>

### 短期（下次 sprint）
<⚠️ 项整改>

### 长期（架构级调整，需 Jack 决策）
<跨模块结构优化>

## 📝 后续流程
- 高危项应该立即派发到 /fix-p0 流程
- 中危项进迭代池
- 如需实测数据 → 请烧录后运行 "task-stat" 命令（如未实现，先在 main 里加一个）
```

---

## 硬性约束

1. **仅做审计，不改代码** —— 改动走 /fix-p0
2. 必须覆盖：任务、队列、锁、临界区、watchdog
3. 涉及禁区任务（BluFi、WiFi 配网）发现问题 → 单独标记 "需 Jack 决策"
4. 如果无法静态分析（需要运行时数据），明确列出需要的采集指令
5. 报告必须精确到文件:行号，不要"大概在 main 目录"
