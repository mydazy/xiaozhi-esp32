# 教育卡 v8 触发机制 & 测试矩阵

> **版本**：v8 启蒙定版（2026-05-09）
> **测试范围**：MyDazy P30-4G / P30-WiFi / P31 三板
> **目标读者**：QA 测试团队 · 烧板量产前回归
> **关联文档**：[education-card-layout.html](education-card-layout.html) · [education-card-charset.md](education-card-charset.md)

---

## 一、触发源全景（4 类入口 + 主屏字体共享）

```
┌─────────────────────────────────────┐
│  云端 LLM 主动触发                  │
│   ↓ MCP tool                        │
│  ① self.education.show_card  ⭐核心 │   9 类 category
│  ② self.education.show_stroke       │   单字笔画 GIF
│  ③ self.education.set_mode          │   教学 prompt 路由
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│  ④ 聊天文本正则提取（被动触发）      │
│     application.cc word_extractor    │
│     聊天里出现 [apple-苹果] 格式自动弹卡 │
└─────────────────────────────────────┘
              ↓
        UiDisplay::ShowEduCard
              ↓
        PickEduMainFont (88/56/48 选档)
              ↓
        BuildEduCard (锚点 + LVGL 渲染)

┌─────────────────────────────────────┐
│  ⑤ 主屏字体（不属教育卡，但共享 30 px 字体源）│
│     时钟 / 日期 / 星期               │
└─────────────────────────────────────┘
```

---

## 二、入口 ① · MCP `self.education.show_card`（核心 9 类）

### 主秀字号决策表

| category | 角色 | 字号决策（self.education.show_card 内部 PickEduMainFont）|
|---|---|---|
| `letter` | 主动学习 | ⭐ ≤ 5 字符 → 88；6-10 → 56；11-12 → 48；> 12 跳过 |
| `phonics` | 主动学习 | ⭐ 同上 |
| `math` | 主动学习 | ⭐ 同上 |
| `word` | 被动触发 | ≤ 10 字符 → 56；11-12 → 48；> 12 跳过 |
| `hanzi` | 被动触发 | ≤ 4 字 → 56；> 4 字跳过 |
| `pinyin` | 被动触发 | 同 word（PY 反向布局：拼音主秀 + 汉字副）|
| `poem` | 被动触发 | 同 hanzi |
| `topic` | 被动触发 | 同 hanzi |
| `color` | 被动触发 | 同 hanzi |

### 测试用例（共 20 个 + 5 跳过）

#### 主动学习 88 主秀（验收点：字大、占屏 ≥ 25%）

| # | category | main | top | bottom | 预期主秀 | 验收点 |
|---|---|---|---|---|---|---|
| **T01** | `letter` | `Aa` | _空_ | `苹果` | **88** | 主行大小写并排，占屏 26% |
| **T02** | `letter` | `Bb` | _空_ | `book` | **88** | 同上，验证大写/小写视觉差异 |
| **T03** | `phonics` | `b` | `声母` | `爸 bà` | **88** | 单字符 88，左右极致大留白 |
| **T04** | `phonics` | `āi` | `韵母` | `爱` | **88** | 拼音含声调字符显示正确 |
| **T05** | `math` | `3+5=8` | _空_ | `三加五等于八` | **88** | 算式 5 字符 88 主秀 + 副位中文读法 |
| **T06** | `math` | `7-2=5` | _空_ | _空_ | **88** | 仅算式无副位，居中显示 |

#### 被动触发 56 主秀（验收点：节奏紧凑，间距 20）

| # | category | main | top | bottom | 预期主秀 | 验收点 |
|---|---|---|---|---|---|---|
| T07 | `word` | `apple` | `ap·ple` | `苹果` | 56 | 三层结构（顶 30 + 主 56 + 副 20）|
| T08 | `word` | `cat` | _空_ | `猫` | 56 | 短词无 Phonics，仅主+副 |
| T09 | `word` | `basketball` | _空_ | `篮球` | 56 | 10 字符仍稳停 56 |
| T10 | `hanzi` | `猫` | `māo` | _空_ | 56 | PY-mode 主秀单字（无副位）|
| T11 | `hanzi` | `书包` | `shū bāo` | _空_ | 56 | 2 字组词 |
| T12 | `hanzi` | `大山雀` | `dà shān què` | _空_ | 56 | 3 字鸟类 |
| T13 | `hanzi` | `金鱼早餐` | `jīn yú zǎo cān` | _空_ | 56 | 4 字满载（边距 21 px）|
| T14 | `pinyin` | `māo` | `单音节` | `猫` | 56 | 拼音主秀（PY 反向）|
| T15 | `poem` | `静夜思` | `jìng yè sī` | `李白·唐` | 56 | 古诗题 |
| T16 | `topic` | `蝴蝶` | `hú dié` | `昆虫` | 56 | 科普词 |
| T17 | `color` | `红色` | `red` | _空_ | 56 | 中英对照颜色 |

#### 兜底降级 48（验收点：仍主秀感，未跳过）

| # | category | main | 预期主秀 | 验收点 |
|---|---|---|---|---|
| T18 | `word` | `information` | **48** | 11 字符兜底 48，单行不换行 |
| T19 | `word` | `celebration` | **48** | 11 字符 |
| T20 | `word` | `imagination` | **48** | 11 字符 |

#### 跳过激活（验收点：屏幕保持原画面，log 输出 skipped）

| # | category | main | 跳过原因 | 验证 |
|---|---|---|---|---|
| **S01** | `word` | `communication` | 13 字符 > 12 阈值 | log: `EduCard skipped (out of activation range)` |
| **S02** | `word` | `extraordinary` | 13 字符 | 同上 |
| **S03** | `hanzi` | `学校教学楼` | 5 字 > 4 阈值 | 同上 |
| **S04** | `letter` | `ABCDEFG` | 7 字符超 88 容量 → **不跳过**，自动降到 56 | log: `main=ABCDEFG(56)` |
| **S05** | `word` | _空字符串_ | main_text 为空 | 函数早 return，不进 LVGL 渲染 |

### 手动测试方法

#### 方式 A：通过云端 LLM 服务端发 MCP 命令（生产路径）

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "self.education.show_card",
    "arguments": {
      "category": "letter",
      "main": "Aa",
      "top": "",
      "bottom": "苹果"
    }
  }
}
```

#### 方式 B：直接代码触发（开发期 quick test）

在 `main/application.cc` 的 OnIncomingJson() 或临时按键回调里加：

```cpp
auto* ui = static_cast<UiDisplay*>(Board::GetInstance().GetDisplay());
ui->ShowEduCard("letter", "Aa", nullptr, "苹果");      // T01
ui->ShowEduCard("phonics", "b", "声母", "爸 bà");      // T03
ui->ShowEduCard("math", "3+5=8", nullptr, "三加五等于八");  // T05
ui->ShowEduCard("word", "apple", "ap·ple", "苹果");    // T07
ui->ShowEduCard("hanzi", "金鱼早餐", "jīn yú zǎo cān", nullptr);  // T13
```

---

## 三、入口 ② · MCP `self.education.show_stroke`（单字笔画 GIF）

### 测试用例

| # | character | 预期 | 验收点 |
|---|---|---|---|
| ST01 | `猫` | 下载 GIF → emoji_box 显示笔画动画 | log `show_stroke: U+732B URL=...` |
| ST02 | `字` | 下载 GIF → 显示 | log `U+5B57` |
| ST03 | `饕` | URL 拼接成功，但 GIF 不存在 → 跳过 | log `GIF 校验失败` 不 crash |
| ST04 | `abc` | 早 return | LLM 收到 `请输入中文汉字` |
| ST05 | _空_ | 早 return | LLM 收到 `请输入一个汉字` |

### 边界测试

- ST06：连续触发 5 个不同字（test 内存稳定）
- ST07：触发 GIF 显示中 → 切到时钟主屏 → 验证 GIF 残留被清除（ResetFontMode）

---

## 四、入口 ③ · MCP `self.education.set_mode`（教学 prompt 路由）

| # | mode | 预期 | 验收点 |
|---|---|---|---|
| SM01 | `word` | 切英文识字 prompt | 后续对话 LLM 偏向英文教学 |
| SM02 | `hanzi` | 切汉字识字 prompt | 后续配合 show_card hanzi + show_stroke |
| SM03 | `pinyin` | 切拼音教学 prompt | 后续 show_card pinyin |
| SM04 | `reset` | 退回默认 chat 人设 | LLM 恢复普通聊天 |
| SM05 | `xxx`（无效） | 返回 `mode 仅支持 word/hanzi/pinyin/reset` | LLM 收到错误 |

---

## 五、入口 ④ · 文本正则提取（被动触发）

**触发条件**：聊天文本中匹配 `[word-中文]` 格式
**实现位置**：`main/application.cc` 的 word_extractor 类

| # | LLM 输出文本 | 提取结果 | 预期 |
|---|---|---|---|
| WE01 | `"今天学了 [apple-苹果]，很有趣"` | word=apple / bottom=苹果 | 自动弹 word 卡 |
| WE02 | `"小猫的英文是 [cat-猫]"` | word=cat / bottom=猫 | 同上 |
| WE03 | `"哈哈苹果"`（无中括号）| 无匹配 | 不弹卡 |
| WE04 | `"[apple-苹果][banana-香蕉]"` | 两个匹配 | 弹首个 word=apple 卡 |
| WE05 | `"[123-数字]"`（非英文）| 取决于正则 | 验证不 crash |

### 手动验证

让服务端 LLM 在回答末尾固定加一对 `[英文-中文]`，触发后 P30 端看 log：

```
[I][application] WordExtractor hit: word=apple top= bottom=苹果
[I][UiDisplay] EduCard v8[word] mode=EN main=apple(56) top= bottom=苹果
```

---

## 六、入口 ⑤ · 主屏字体验证（间接验证字体修复）

**不是教育卡触发**，但 30/88 字体共享，必须一起验证：

| # | 屏幕字段 | 字体 | 验收点 |
|---|---|---|---|
| HM01 | 时钟时间 `12:34` | 88 px clock_big | ✅ 完整显示 |
| HM02 | 日期 `2026年5月9日` | 30 px clock_text | ✅ **年 / 月 / 日** 中文显示（v1 缺字 bug 修复） |
| HM03 | 星期 `星期六` | 30 px clock_text | ✅ **星 / 期 / 六** 中文显示 |
| HM04 | 星期 7 个工作日轮换 | 30 px clock_text | 一二三四五六日全部不缺字 |
| HM05 | 异常时间（系统时间未同步） | 30 px clock_text | 显示 `----年--月--日` `星期--`，- 不缺字 |
| HM06 | 配网 QR 页 URL | g_text_font (20 全集) | 中文不缺字 |

### 手动测试

- 重启设备 → 看时钟主屏 5 秒
- 改系统时间到不同星期 → 看星期切换是否正常
- 清空 NTP → 看错误状态显示

---

## 七、关键日志 keyword（monitor 时筛选）

| 日志关键字 | 含义 | 触发场景 |
|---|---|---|
| `EduCard v8[letter] mode=EN main=Aa(88)` | ⭐ 88 主秀生效 | T01-T06 |
| `EduCard v8[word] mode=EN main=apple(56)` | 56 标准主秀 | T07-T09 |
| `EduCard v8[word] mode=EN main=information(48)` | 48 EN 兜底 | T18-T20 |
| `EduCard v8[hanzi] mode=PY main=猫(56)` | PY-mode 主秀 | T10-T13 |
| `EduCard skipped (out of activation range): xxx` | ⛔ 跳过激活 | S01-S03 |
| `EduCard skipped: base fonts not loaded` | 字体加载失败 | 仅开发期 |
| `show_stroke: U+732B URL=...` | 笔画下载触发 | ST01 |
| `show_stroke: GIF 校验失败` | 该字无笔画动画 | ST03 |
| `WordExtractor hit: word=...` | 文本正则触发 | WE01 |

---

## 八、烧板验证流程（推荐顺序）

### 准备

```bash
cd /Users/jack/GitHub/mydazy-p30-v32
idf.py build
idf.py flash monitor
```

确认 build 输出包含：
- `font_maru_88_4.bin` (89 KB) ⭐ 新版含字母 + 数学
- `font_maru_56_1.bin` (1.3 MB) ⭐ v8 主秀
- `font_maru_30_4.bin` (79 KB) ⭐ 含主屏字
- `font_maru_48_4.bin` (77 KB) ⭐ EN 兜底纯英文

### 验证步骤

| Step | 场景 | 用例 | 通过标准 |
|---|---|---|---|
| **1** | 主屏字体（无需 LLM）| HM01-HM06 | 时钟 / 日期 / 星期完整显示 |
| **2** | 88 主秀（最易测，先做）| T01 / T03 / T05 | 主行 88 大字效果，对比 56 视觉冲击明显 |
| **3** | 56 标准主秀 | T07 / T10-T13 | 三层结构 + 间距 20 |
| **4** | 48 兜底 | T18 | 11 字符英文单行不换行 |
| **5** | 跳过保护 | S01-S03 | log 输出 skipped + 屏保持原画面 |
| **6** | PY-mode 多字数 | T10-T13 | 1 字 / 2 字 / 3 字 / 4 字主秀都正常 |
| **7** | 笔画 GIF | ST01-ST04 | GIF 加载 + 异常字不 crash |
| **8** | 文本正则 | WE01-WE02 | 自动弹卡 |
| **9** | 跨场景切换 | 字母 → 单词 → 汉字 → 古诗 | 字体切换不 crash + 视觉一致 |
| **10** | 卡片互斥 | 显示教育卡 → 触摸 → 切时钟 | overlay 隐藏，回到 chat/clock |

---

## 九、回归用例（量产前必跑）

| # | 场景 | 验收 |
|---|---|---|
| **R1** | v1 旧路径兼容（应用仍调 word/hanzi/pinyin） | 不 crash + 显示正常 |
| **R2** | 字体缺失退化（手动删除 56 字体后重启） | 自动退到 48 主秀，不 crash |
| **R3** | 连续触发 100 次教育卡 | 内部 RAM 稳定 > 60 KB（无泄漏） |
| **R4** | 教育卡显示中触摸 → HideEduCard | overlay 隐藏，回到 chat/clock |
| **R5** | 教育卡 + QR 互斥 | 显 QR 时教育卡自动 hide |
| **R6** | 教育卡 + 时钟切换 | SwitchToClockMode 时自动 HideEduCard |
| **R7** | 三板差异（P30-4G / P30-WiFi / P31）| 三板教育卡渲染一致 |
| **R8** | OTA 升级前后 | 升级后字体加载正常，无版本不匹配 |

---

## 十、问题汇报模板（QA 反馈用）

发现 bug 时按以下格式回报：

```markdown
### 测试用例编号
T03 / S01 / HM02 / R3 等

### 输入
category=letter, main="Aa", top="", bottom="苹果"
（或 LLM 文本 / 主屏场景）

### 设备
P30-4G v32 · 固件 commit hash xxxxxxx

### 预期
主行 88 大字 "Aa" 大小写并排，占屏 26%

### 实际
（截图或 log 片段）

### 关键日志
[I][UiDisplay] EduCard v8[letter] mode=EN main=Aa(56)  ← 错误：应该 88

### 严重等级
P0 (crash / 数据丢失) / P1 (功能不可用) / P2 (视觉问题) / P3 (建议优化)

### 相关 commit / 文件
main/display/ui_display.cc:PickEduMainFont
```

---

## 十一、附录：LVGL 渲染管线视觉常量速查

| 项 | 值 | 说明 |
|---|---|---|
| 屏幕 | 284 × 240 | 1.83" 圆角矩形 R25 |
| 安全宽 | 280 | 左右各 2 px 余量 |
| 物理边框 | 48 px / 边 | 玻璃黑边 18 + 装饰圈 30 |
| 主秀色 | `#FFCA28` | 金色（被动 56 / 主动 88） |
| 副信息色 | `#A5D6A7` | 薄荷绿（顶部 + 副位） |
| 跑马灯色 | `rgba(255,255,255,0.55)` | 半透明白 |
| 黑底 | `#000000` | OLED 友好 |
| 字距（PY 主秀）| 3 px | 圆角字体 ls |
| 字距（EN 主秀）| 0 | 连读不能拆 |
| 字距（顶部拼音）| 2 px | 字母呼吸 |
| 字距（副位汉字）| 3 px | 圆角字体 ls |
| 字距（88 主秀）| 4 px | 大字加间距 |
| PY 顶部 anchor | top: 50 | 距圆角顶 25 + 25 缓冲 |
| PY 主行 anchor | top: 100 | 距顶部底 20 px |
| EN 主行 anchor | top: 80 | 居中偏上让位副位 |
| EN 副位 anchor | bottom: 64 | 距主行底 20 px |
| 88 副位 anchor | bottom: 36 | 主行底 168 → 副顶 184 间距 16 |

---

**文档版本**：v1.0 · 2026-05-09
**维护人**：Mydazy 团队
**反馈渠道**：飞书 / 项目 issue
