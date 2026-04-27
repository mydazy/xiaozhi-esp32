---
description: P0 bug 修复完整流程。强制先走诊断、再出方案、再经审查、最后要求多设备验证。未通过诊断的禁止跳到修复。
argument-hint: "<bug ID 或描述>"
allowed-tools: Task, Read, Grep, Glob, Edit, Bash
---

# /fix-p0 — P0 Bug 修复闭环

用户输入: $ARGUMENTS

---

## 执行步骤（严格按顺序）

### Step 1：检查是否已完成 /diagnose-bug

**在进入修复前**，确认已经有诊断报告（有可疑根因、已拿到真实日志）。
如果没有或证据不足，**回退到 `/diagnose-bug` 流程**，不要在无证据的情况下往下走。

输出：
```
Step 1: 诊断状态检查
- 是否有诊断报告：<是/否>
- 是否拿到真实日志：<是/否>
- 根因是否确定：<是/否>

如任一为"否" → 停下，转 /diagnose-bug，拒绝继续。
```

### Step 2：调用对应修复 agent 出补丁

根据诊断报告的类型：
- UI 类 → `lvgl-ui-fixer`
- RTOS 类 → `rtos-task-auditor` 定位 + `firmware-debugger` 出补丁
- 网络类 → `network-stack-expert`
- 通用 → `firmware-debugger`

派发时 prompt 强调：
- 必须遵循 `CLAUDE.md` 所有约束
- diff ≤ 80 行
- 不能触及禁区
- 必须显式标注"这是 P0 修复"

### Step 3：强制走 code-reviewer-p0 审查

**无条件**调用 `code-reviewer-p0` agent，把 Step 2 的补丁交给它审。
- 通过 → 进入 Step 4
- 打回 → 返回 Step 2 让修复 agent 按审查意见改，最多 3 轮
- 3 轮仍打不回 → 停下告诉 Jack："此补丁无法通过 P0 审查，需要人工介入"

### Step 4：输出最终补丁 + 验证清单

格式：

```
# ✅ P0 修复方案（已过 code-reviewer-p0）

## 1. Bug ID / 描述
$ARGUMENTS

## 2. 根因（来自诊断）
<一句话>

## 3. 修复 diff（<n> 行，已过审查）
<unified diff>

## 4. ⚠️ Jack 必须完成的验证（在进量产前）

**强制**：
- [ ] 至少 5 台设备烧录验证
  - P30-4G: <n> 台
  - P30-WiFi: <n> 台
  - P31: <n> 台
- [ ] 覆盖场景：
  - 正常流程复现 bug 原场景 × 3 次
  - 极端场景：断电/重启、弱网、快速切页面
  - 回归：禁区模块（BluFi、WiFi 配网、下拉切换、基础绑定）
- [ ] 连续运行 ≥ 24h 无异常
- [ ] 更新 `docs/known-issues.md`：标记 bug 状态 "已修复 - <commit>"

## 5. Commit 信息模板

[模块] <简述>

根因：<...>
改动点：<...>
影响范围：<...>
验证：5 台设备 24h 稳定

关联 bug: #<id>
SKU 影响: <...>

## 6. 下一步
- 进入 release 分支前，跑 `/pre-flash-check`
- 不要绕过 production-readiness
```

---

## 硬性约束

1. **无诊断报告 / 无真实日志 → 拒绝进入修复**
2. **code-reviewer-p0 必须通过**，否则不出最终补丁
3. **禁止自己决定"这个 P0 够小了，跳过审查"** —— 没有这个权限
4. **禁止触及禁区**，遇到立即停下请 Jack 决策
5. 多设备验证不是可选，是量产入口
6. 如果 Jack 在 /fix-p0 里写了 "override-review"，仅此一次放行，但必须记录到 `docs/release-overrides.md`
