---
description: 量产前最后一道闸。跑完整 production-readiness checklist（12 项），输出放行/拒绝报告。烧录批量设备前必跑。
argument-hint: "[版本号如 v2.2.6]"
allowed-tools: Task, Read, Grep, Glob, Bash
---

# /pre-flash-check — 量产放行检查

版本：$ARGUMENTS

---

## 执行步骤

### Step 1：调用 production-readiness agent

用 Task 工具，`subagent_type=production-readiness`。prompt：

```
Jack 准备烧录量产批次，版本 $ARGUMENTS。
请按你的 12 项 checklist 逐条检查，输出放行或拒绝报告。

关键检查点：
1. 多设备验证（≥5 台，覆盖 P30-4G/P30-WiFi/P31）
2. 断电重启、弱网、禁区回归
3. known-issues.md / CHANGELOG 更新
4. 全 SKU 编译通过
5. OTA 向前兼容
6. 回滚方案就绪

我会补充收集证据帮你核实。禁止放低标准。
```

### Step 2：主动收集证据帮 agent

并行执行：
- `git log --oneline -20` — 看最近改动
- `git diff <上一个 release tag>..HEAD --stat` — 看累计改动量
- 读 `CMakeLists.txt` — 确认 PROJECT_VER 已升
- 读 `docs/known-issues.md` — 确认本版本条目已补
- 读 `CHANGELOG.md`（如存在） — 确认条目完整
- 读 `docs/release-overrides.md`（如存在） — 看历史 override 记录

把这些证据塞给 agent，让它有依据判断。

### Step 3：输出最终放行报告

格式严格按 `production-readiness` agent 的"情况 A / 情况 B"模板。

放行时额外追加：

```
## 🚀 烧录操作建议

1. 抽检 3 台设备再次验证
2. 首批出货限量：建议 <200 台
3. 监控 72 小时：
   - 后端崩溃率（目标 < 0.1%）
   - 连接成功率（目标 > 99%）
   - 心跳丢失率
4. 若异常：按回滚预案立即执行

## 📜 审计记录

审计人：Claude Code /pre-flash-check
审计时间：<当前日期>
版本：$ARGUMENTS
决策：<放行 / 拒绝>

此报告已写入 docs/release-reports/<version>.md（如需要，用户手动保存）
```

---

## 硬性约束

1. **12 项全过才放行**，少一项都是 ❌
2. **禁止帮 Jack 补漏** —— 你是审查，不是执行。缺了让 Jack 自己补
3. 不受"紧急"、"等货"、"今天就要出"影响
4. 即使 Jack override，必须记录到 `docs/release-overrides.md`
5. 禁止自己改版本号 / CHANGELOG —— 那是 Jack 的决策
6. 此命令的输出必须是 **最终决策**，不是建议
