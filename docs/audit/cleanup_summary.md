# 量产前代码精简 — 健康检查报告

> **检查时间**：2026-04-23  
> **基线 tag**：`pre-cleanup-baseline` → `a9e9b2de`（代码改动前的起点）  
> **当前 HEAD**：`4825e750`（批次 1 已 commit）  
> **分支**：`dev`

---

## 产品经理 5 句话总结

1. **批次 1 做完了，并且三块板都真机烧录成功**——精简后的固件**已经跑起来了**，不是纸面过编译。
2. **净删 1884 行 + 22 个文件**（分区表、非 S3 芯片配置、旧文档/脚本、35 条无用组件依赖、audio_service 的死分支）；**计划中的批次 2（删 camera / video / rndis 的 ~1796 行）和批次 3（Kconfig 僵尸选项 ~235 行）都没做**。
3. **最大的技术债有两块**：一是 `websocket_baidu_protocol.cc` **1554 行未接线占位代码**（你定的保留，建议 3–6 个月内拍板接入或删）；二是批次 2/3/4 未执行 → 再压一层能腾出 ~2100 行代码 + 管住几十 MB CI 下载量。
4. **两个采坑教训已归档**（防止下次团队精简时再踩同样的坑）：① "组件名 ≠ 头文件前缀"（`esp_image_effects` 对外是 `esp_imgfx_*`）；② "transitive 载体删除前须排查"（`uart-eth-modem` 删了导致 `uart-uhci` 断链）。
5. **下一步建议**：先把批次 1 的未跟踪文件（`docs/audit_pass1_architecture.md`、两份触摸固件 `.i`）决定是否入仓，然后**要么进阶段 1 现状盘点**（把你早前打断的 `pass0_current_state.md` 做完）、**要么把批次 2 做完**——两条路不冲突，但建议先盘点、再基于盘点结论决定批次 2/3 还做不做。

---

## Part 1：跟 pre-cleanup-baseline 对比

```
$ git diff --shortstat pre-cleanup-baseline..HEAD
 26 files changed, 48 insertions(+), 1932 deletions(-)
```

| 维度 | 数量 |
|---|---|
| **净代码行变化** | **-1884 行**（-1932 删 + 48 增） |
| **删除文件数** | **22 个** |
| **修改文件数** | **4 个**（`audio_service.cc`、`idf_component.yml`、`sdkconfig`、`cleanup_plan.md`） |
| **新增文件数** | 0（批次 1 不产生新代码，只有一个 `CLAUDE.md` 在 baseline commit 里就已入仓） |

### 按类别统计删除

| 类别 | 文件数 | 代码行 |
|---|---|---|
| 分区表 v1/v2 非主用（`partitions/v1/*.csv`、`partitions/v2/{16m_c3,4m}.csv`） | 9 | -73 |
| 非 S3 芯片默认配置（`sdkconfig.defaults.{esp32,c3,c5,c6,p4}`） | 5 | -71 |
| 旧文档（`README_ja.md`、`docs/{known-issues,sdk-analysis}.md`） | 3 | -868 |
| 旧脚本（`scripts/acoustic_check/*`） | 5 | -769 |
| **合计文件删除** | **22** | **-1781** |
| yml 依赖瘦身（35 条 → +8 行注释补充） | （改动） | -89 净 |
| `audio_service.cc` 死分支（`#else` + `EspWakeWord` 实例化） | （改动） | -8 |
| `sdkconfig` 段落自然消失（IO Expander / CST816S touch / I2C Bus Options） | （改动） | -40 |
| `cleanup_plan.md` 教训更新 | （改动） | +2 净 |

---

## Part 2：各批次 Commit 列表

| 批次 | Commit | 描述 | 状态 |
|---|---|---|---|
| Commit #0（pre-cleanup） | `a9e9b2de` | `[Docs] 补充项目宪法 + cleanup 规划入仓` — `CLAUDE.md` + `docs/audit/cleanup_plan.md` | ✅ 已 commit |
| **Batch-1** | `4825e750` | `[Cleanup-Batch-1] 精简非 S3/P30/P31 遗留资产 + idf_component.yml 大瘦身` | ✅ 已 commit + 真机烧录通过 |
| **Batch-2** | — | 删 `esp32_camera.{cc,h}` + `esp_video.{cc,h}` + `camera.h` + `rndis_board.{cc,h}` + yml 联动（~1796 行 + 3 组件） | ❌ 未执行 |
| **Batch-3** | — | Kconfig 僵尸选项（~235 行）+ ❓ 依赖验证（`image_player`/`adc_battery_estimation`/`esp_new_jpeg`）+ `axp2101`/`lamp_controller` 复核 | ❌ 未执行 |
| **Batch-4** | — | 主干代码精简（规划中默认不做） | ❌ 未执行 |

### Batch-1 具体删除内容明细

**A. 非 S3 / 非主用资产（21 个 git rm，来自前会话遗留）**
- `partitions/v1/*.csv`（7 个）：本仓库固定 v2/16m.csv
- `partitions/v2/{16m_c3,4m}.csv`：C3 无目标板，4MB 非当前配置
- `sdkconfig.defaults.{esp32,c3,c5,c6}`：本仓库只支持 S3
- `README_ja.md`：无日文用户
- `docs/{known-issues,sdk-analysis}.md`：过时
- `scripts/acoustic_check/*`：旧声波检测工具

**B. 本批次新增动作**
1. `sdkconfig.defaults.esp32p4`：本仓库无 P4 目标
2. `main/idf_component.yml`：删 35 条主干零引用依赖（LCD/触摸/IO 扩展/外设/P4 板子等）
3. `main/audio/audio_service.cc`：删 S3/P4-only 仓库中永不触发的 `#else` 分支

**C. 过程中补回（教训归档）**
- `espressif/esp_image_effects`：对外头文件 `esp_imgfx_*`，命名不一致导致我 grep 没命中 → 必留
- `78/uart-uhci`：原由 `uart-eth-modem` 充当 transitive 载体，删后需显式声明（上游 `78/esp-ml307` 的 CMakeLists 把 `uart-uhci` 放在 PRIV_REQUIRES 而非 REQUIRES 的 bug）

---

## Part 3：三款目标构建状态

| 板 | 构建 | 烧录 | 说明 |
|---|---|---|---|
| `mydazy-p30-4g` | ✅ 通过 | ✅ 真机成功 | `DualNetworkBoard`（4G+WiFi）+ ES8311+ES7210 codec |
| `mydazy-p30-wifi` | ✅ 通过 | ✅ 真机成功 | `WifiBoard`（纯 WiFi）+ ES8311+ES7210 codec |
| `mydazy-p31` | ✅ 通过 | ✅ 真机成功 | `DualNetworkBoard`（4G+WiFi）+ ES7111+ES7210 codec + GPS + NFC |

> **数据来源**：用户 2026-04-23 手动确认"打包烧录成功"。`releases/` 目录当前只存着更早的 `v2.2.5_*.zip`（4 月 13/16 日），因为本次闸门 1 走的是 `idf.py flash monitor` 直接烧录路径，未必生成 zip。  
> **验证覆盖**：build 通过 + 真机烧录通过 = 链路级 smoke test 通过。**未做深度功能回归**（唤醒词、对话、OTA、4G 切 WiFi、配网等），如需量产级验证，见 Part 5 "下一阶段关注点"。

---

## Part 4：当前技术债

### D1. 规划中未执行的批次（可恢复）

| 债项 | 预估体量 | 阻塞下一批的条件 |
|---|---|---|
| **Batch-2：Camera/Video/RNDIS 源文件删除** | ~1796 行 + `esp32-camera` / `esp_video` / `iot_usbh_rndis` 三组件 + CMakeLists.txt 同步 | 无 — 可立即开工 |
| **Batch-3：Kconfig 僵尸选项 + ❓ 依赖验证** | ~235 行 Kconfig + 3 个待验证依赖 + 几个 ❓ 外设驱动（`axp2101`、`lamp_controller` 等） | 需先跑 Batch-2 的闸门 |
| **Batch-4：主干代码精简** | 小 — 规划里默认不做 | — |

### D2. 占位 / 未接线代码（长期债）

| 债项 | 体量 | 处置建议 |
|---|---|---|
| `main/protocols/websocket_baidu_protocol.{cc,h}` **未接线** | `1554 行`（cc 1380 + h 174） | 你 2026-04-23 决定"保留占位用于未来切换平台" → 建议打上 **3–6 个月硬 deadline + 明确 owner**，到期没接入就删，避免长期腐化 |
| `docs/audit/cleanup_plan.md` 标记的 ❓ 待确认项 | — | 批次 3 执行时逐项处理：`adc_battery_estimation` / `esp_new_jpeg` / `image_player` / `axp2101` / `lamp_controller` / `scripts/{sonic_wifi_config.html,download_github_runs.py}` / `ui_img_start_logo_png.c` |

### D3. 未跟踪文件悬置（需决策）

| 文件 | 我的看法 |
|---|---|
| `docs/audit_pass1_architecture.md` | 你自己在写的架构文档，由你决定何时入仓 |
| `components/esp_lcd_touch_axs5106l/AXS5106L_*_V29_07_*.i` / `V29_09_*.i` | 触摸控制器 vendor 固件预处理头（每版升级会换）。建议 **add 入仓**（否则下次构建产物不可重现） |

### D4. 工具链债（小）

- `.github/workflows/build.yml` 是 board 动态 matrix（读 `main/boards/*/config.json`），**CI 自动跟上了精简后的三板**，不需要手改 — **无债**
- `README.md` / `README_zh.md` 仍是 upstream xiaozhi 的宣传稿，不反映已精简到三板的现状 — **文档债**，建议跟 Batch-3 或单独一 commit 重写

---

## Part 5：下一阶段（阶段 1：现状盘点）关注点

> **背景**：你最早让我做的 `docs/audit/pass0_current_state.md`（五 Part 的现状盘点）**被打断了**（转向了 cleanup_plan）。cleanup Batch-1 做完后，回到那条主路径才是"下一阶段"。

### 阶段 1 应该回答的问题

1. **仓库跟 upstream 主线（`78/xiaozhi-esp32`）真正的 delta 是什么？**
   - 我们加了什么（mydazy board、UiDisplay、LiveCompanion、RemoteCmd、百度协议占位…）
   - 我们删了什么（本次 cleanup + 历史 cleanup）
   - 我们改了什么（audio 架构、配网流程、电池管理、UI 主题）
   - **无法确定 fork 点** → 需显式标注，不猜（你最初的明确要求）

2. **三板的"量产可用性"各自打几分？**
   - 板级初始化是否完整（GPIO/I2C/SPI/UART/音频 chain）
   - TODO/FIXME/占位代码扫一遍（目前已知 P31 里有 `InitializeIBeacon()` TODO）
   - 独立编译通过 ≠ 量产可用 → **要跑深度功能回归**

3. **最近 3 个月代码改动的热点在哪里？**
   - commit 热度按目录/文件排序
   - 哪些模块在频繁抖动 = 不稳定 = 量产前要重点回归测试

4. **通信协议层的契约完整度**
   - WebSocket（主用）/ MQTT（默认）/ JoyAI / 百度 — 四套协议的一致性
   - 跟服务端契约（`~/.claude/contracts/mydazy-iot/`）对齐度
   - MCP / IoT 工具注册表是否全量声明

5. **内存红线实测**
   - 三板各自的 `MALLOC_CAP_INTERNAL` 水位（压测下）
   - 关键任务栈 `uxTaskGetStackHighWaterMark` 水位
   - PSRAM 栈任务是否严格遵循 `~/GitHub/.claude/CLAUDE.md` 里的"Core 0 持续循环禁止 PSRAM 栈"规则
   - 这些数据 **只能真机跑 + 收集**，AI 静态分析给不了

### 建议的执行顺序

```
阶段 1a（AI 可做）：静态盘点（3–4 小时）
  └─ 基于 git log + 目录差异对比 + grep TODO/FIXME + 协议契约核对
     产出 pass0_current_state.md + pass1_architecture.md（你已起头）

阶段 1b（需你物理参与）：真机深度回归（1 人日）
  └─ 三板各自跑：唤醒→对话→OTA→4G/WiFi 切换→配网→长跑 24h
     产出 pass2_field_test.md + 内存/栈水位数据表

阶段 1c（AI+你决策）：基于盘点结论决定是否继续精简
  └─ 如果盘点暴露量产风险 → 优先修风险，Batch-2/3 后推
     如果盘点确认稳定 → 继续 Batch-2/3 压一压
```

---

## 附录 A：一行命令验证本报告的事实

```bash
# baseline → HEAD 的 diff 体量
git diff --shortstat pre-cleanup-baseline..HEAD
# → 26 files changed, 48 insertions(+), 1932 deletions(-)

# 批次 commit 列表
git log --oneline pre-cleanup-baseline..HEAD
# → 4825e750 [Cleanup-Batch-1] ...

# 百度协议体量
wc -l main/protocols/websocket_baidu_protocol.{cc,h}
# → 1380 cc + 174 h = 1554 行

# 百度协议未接线验证
grep -r "WebsocketBaidu\|websocket_baidu" main/application.cc main/ota.cc
# → 空输出 = 未被 Application 引用
```

---

## 附录 B：可回滚点

| 场景 | 命令 |
|---|---|
| **整个批次 1 全部回滚** | `git reset --hard pre-cleanup-baseline` ⚠️ 会丢失工作区未跟踪文件外的一切 |
| **只回滚批次 1 部分文件**（例如某个误删） | `git checkout pre-cleanup-baseline -- <path>` |
| **回到规划入仓状态（保留 cleanup_plan.md 但代码还原）** | `git reset --hard a9e9b2de` |
