# B-08 display/UI 控制中心 — 对比官网 v2.2.4 识别过度优化

> 基线 v2.2.4（`5ed4b01eb`）；标尺=量产稳定；🟢必要/🔴过度/⚪扩展/🛡️红线保留；只分析不改码。

## 取证范围

| 类别 | 文件 | 行数 | 官方 v2.2.4 是否存在 |
|---|---|---|---|
| 自研深审 | main/display/ui_display.cc | 1437 | ❌ 全新 |
| 自研深审 | main/display/ui_display.h | 219 | ❌ 全新 |
| 自研深审 | main/display/text_font.h | 19 | ❌ 全新 |
| 自研深审 | main/display/ui/widgets/control_center.{cc,h} | 545+133 | ❌ 全新 |
| 自研深审 | main/display/ui/widgets/managed_timer.h | 56 | ❌ 全新（真用） |
| 自研深审 | main/display/ui/core/managed_timer.h | 159 | ❌ 全新（**死文件**） |
| 自研深审 | main/display/ui/theme/ui_config.h | 150 | ❌ 全新（**死文件**） |
| 自研深审 | main/display/ui/resources/ui_img_paths.h | 32 | ❌ 全新（**死文件**） |
| 改官方 | main/display/lcd_display.cc | +186 | ✅ 改 |
| 改官方 | main/display/display.h | +28 | ✅ 改（加 ShowQrCode 虚接口） |
| 改官方 | main/display/lcd_display.h / oled_display.h / lvgl_display/* | 少量 | ✅ 改 |

官方 `main/display/` 无 ui/ 目录、无 ui_display、无 text_font.h，已 `git ls-tree v2.2.4:main/display/` 核实。基线事实：UiDisplay 整套自研体系（时钟/聊天/音乐页/番茄钟/二维码/开机 logo/状态栏/控制中心）官方均无 → 整体 ⚪扩展，本文重点找其内部 🔴/🟢/🛡️。

---

## 🟢 必要（服务量产稳定）

| 项 | 我们怎么改 | 官方 v2.2.4 原实现 | 为何必要 | 证据 file:line |
|---|---|---|---|---|
| ManagedTimer RAII（widgets 版） | LVGL timer 封装类，析构自动 `lv_timer_del`+置 nullptr，禁拷贝、允移动 | 无（官方无控制中心，无此场景） | 防 timer 回调访问已析构对象（UAF）、防 double-free、防野指针 | ui/widgets/managed_timer.h:14-56 |
| CreateOnce 关 auto_delete | 单次 timer `set_repeat_count(1)`+`set_auto_delete(false)` | 无 | LVGL9 timer 默认 auto_delete=true，单次触发后框架自删→RAII 指针变野 double-free。两份 ManagedTimer 都加了此防护 | ui/widgets/managed_timer.h:39-42；ui/core/managed_timer.h:65-68 |
| 控制中心 timer 用 ManagedTimer 成员 | `slider_timer_`/`network_confirm_timer_` 为 ManagedTimer 成员对象，析构时按 RAII 自动 Delete | 无 | 对象销毁→成员逆序析构→timer 必先被删，回调不会再触发→堵死 UAF（见深审 §1 对 Explore 误判的纠正） | control_center.h:111-112 |
| AEC「请求-回写」防脱钩 | 点击只发 `aec_toggle_callback_()` 请求，**不预翻** `aec_on_`；真实状态由外部 `SetAecState()` 回写 | 无 | 防「UI 显示已开但 AEC 实际没切」的状态脱钩，对量产体验真问题 | control_center.cc:486-488（注释明示）；SetAecState control_center.cc:379-383 |
| 网络切换二次确认+防抖 | 单击进确认态（橙色「再点确认」），600ms 内再点判抖动连击忽略，3s 超时自动回退 | 无 | 切网=重启级操作，防触摸抖动/误触穿透直接重连（弱网现场触摸本就乱跳，见记忆 touch-jitter-4g-rf） | control_center.cc:450-468；CONFIRM_MIN_GAP_MS=600 cc:39 |
| PlayerTickCb 场景守卫 | 进度 timer 回调先判 `active_scene_ != kPlayer` 直接 return | 无 | timer pause 但未 delete，切场景后若残留触发，守卫挡住越界访问 | ui_display.cc:954-960 |
| text_font RAM 代理 | BUILTIN_TEXT_FONT 在 Flash .rodata 不可写 `.fallback`，故按值拷贝到 RAM `g_text_font`，对其写 cbin 兜底字体 | 无 | 直接写 rodata 字体的 fallback 触发 Cache 错误「Dbus write to cache rejected」必崩；代理是绕开硬件限制的正确做法 | text_font.h:2-13；InitTextFontProxy ui_display.cc:147-151 |
| cbin 字体加载全失败路径有守卫 | GetAssetData 失败/cbin_font_create 失败均 `ESP_LOGW/E`+return，不裸用 NULL 指针 | 无 | 字体资产未烧/损坏时降级为「缺字显示方框」而非崩溃，量产容错 | LoadFallbackTextFont ui_display.cc:157-174；EnsureDisplayFonts ui_display.cc:249-265 |

---

## 🔴 过度（偏离官方又不服务稳定 / 死代码 / 注释与实现不符）

| 项 | 我们怎么改 | 官方 v2.2.4 对应 | 为何判过度 | 维护成本/风险 | 证据 file:line |
|---|---|---|---|---|---|
| **死代码：FinishBootAndShowClock()** | 整个开机 logo 渐出动画函数（24 行，含 lv_anim fade_out + completed_cb→SwitchToClockMode） | 无 | **全仓零调用方**（仅 .h 声明+.cc 定义）。实际开机走 application.cc:1190 `SwitchToClockMode()` 硬切，StartBootAnimation 也只做静态显示无渐出 | 死代码常驻，误导后人以为开机有渐出动画；改开机逻辑时埋坑 | 定义 ui_display.cc:397-431；声明 ui_display.h:74；零调用方（grep 实证） |
| **注释与实现不符：开机注释** | 注释写「Idle 时由状态机触发渐出 → 切时钟」 | 无 | 实现里渐出函数(FinishBoot)是死的，状态机从不触发；StartBootAnimation 实为「静态显示·无动画」却挂在「开机 Logo」注释块下，自相矛盾 | 注释债，读码者被误导 | ui_display.cc:384（注释）vs StartBootAnimation ui_display.cc:391-395（实为静态） |
| **死文件：ui/core/managed_timer.h** | 整文件 159 行 ManagedTimer（功能比 widgets 版更全：Pause/Resume/Reset/SetPeriod 等） | 无 | **零 #include 引用**。control_center.h:6 `#include "managed_timer.h"` 按相对路径解析到同目录 `ui/widgets/managed_timer.h`（56行），core 版从无人用。两份重复实现 | 重复维护、改 bug 易改错版本；159 行死码 | ui/core/managed_timer.h 整文件；`grep core/managed_timer` 零命中 |
| **死文件：ui/theme/ui_config.h** | 整文件 150 行（ScreenConfig 命名空间：尺寸/布局/Colors/EduColors/FontSize/Animation/EduLayout 全套常量） | 无 | **零 #include 引用**，ui_display.cc 用的是自己匿名 namespace 的 `kColor*`（19 处），从不引用 `ScreenConfig::` 任何符号 | 150 行死码；且其注释 ui_config.h:29「2x2 宫格」与控制中心真实 3x2 不符，GRID_BTN_SIZE=72 与 control_center.cc BTN_SIZE=75 也对不上——配置与实现双重脱节 | ui/theme/ui_config.h 整文件；`grep ui_config.h #include` 零命中；ui_display.cc include 列表无它 |
| **死文件：ui/resources/ui_img_paths.h** | 整文件 32 行（图片资源路径常量） | 无 | **零 #include 引用**，全仓无任何文件引用 | 32 行死码 | ui/resources/ui_img_paths.h 整文件；`grep ui_img_paths` 零命中 |
| **注释与实现不符：控制中心呼出方式** | board 注释仍写「下滑唤起控制中心」「下滑/横滑」 | 无 | 实现已废弃下滑唤起：OnTouchSwipe `if (dy>=0) return;` 显式拦掉下滑，呼出改为「单击状态栏 y<36」（OnTouchClick）。注释滞后 | 注释债（轻），误导后人以为下滑仍能呼出 | 注释 mydazy_p30_board.cc:336（4g 版，wifi 版同构）；废弃逻辑 cc:384；真实呼出 cc:362-366 |

---

## ⚪ 扩展（官方没有的纯业务 UI，仅登记）

| 项 | 能力 | 证据 file:line |
|---|---|---|
| UiDisplay 多场景体系 | 时钟主屏/聊天/音乐播放/番茄钟/二维码/开机 logo/状态栏，场景互斥切换（kClock/kChat/kPlayer/kPomodoro） | ui_display.cc 全文；SwitchToClockMode cc:312、SwitchToChatMode cc:346、Player cc:1007、Pomodoro cc:1018-1169 |
| iOS 风控制中心 | 3×2 宫格：网络/打断(AEC)/休眠 // 退出/亮度/关于；亮度滑块 | control_center.h:8-11；control_center.cc 全文 |
| 亮度纯 PWM 直调 | OnSliderChanged 对齐步进后直接 `brightness_callback_(value)`，无线程绕行，实时生效 | control_center.cc:525-545 |
| 通用二维码虚接口 | display.h 加 `ShowQrCode()/HideQrCode()` 虚函数（配网/绑定/付费二维码通用），默认空实现，子类覆盖 | display.h diff（+28 行）|
| cbin 动态字体多档 | clock_big(88)/clock_text(30)/edu(48/56) PSRAM 动态加载，fallback 链 edu→clock_text→g_text_font | EnsureDisplayFonts ui_display.cc:249-279 |
| 教育模式配色/布局常量 | EduColors/EduLayout（CHILD Profile 跟读反馈/星星/PTT） | ui_config.h:65-147（注：随死文件一起未被引用，登记但实际无效） |
| 官方基础层量产适配（lcd_display.cc） | 单 buffer×24 行省 RAM、批量清屏、task_priority 1→5、image cache 2→4MB、默认主题 light→dark、删 RgbLcdDisplay 死类 | lcd_display.cc diff（+186/-，删 928 行含 jpg/） |

---

## 🛡️ 红线保留（触内存安全/并发，只标不动）

| 项 | 说明 | 证据 |
|---|---|---|
| ManagedTimer RAII（两份） | 直接服务内存安全（防 UAF/double-free/野指针），即便 core 版是死码、即便像「重复造轮子过度」，按红线规则**保留不动**，仅在 🔴 标记 core 版死文件待用户决策删除 | ui/widgets/managed_timer.h:14-56；ui/core/managed_timer.h:21-159 |
| text_font RAM 代理 + cbin 加载 | 触字体指针生命周期 + PSRAM 加载边界，是绕开「写 rodata 触发 Cache 崩溃」的内存安全机制，保留不动 | text_font.h；ui_display.cc:144-174 |
| AEC 请求-回写解耦 | 触 UI 状态与真实状态一致性（并发语义），保留不动 | control_center.cc:379-383,486-488 |

---

## 深审发现（逐点，带 file:line + 风险级）

### 1. 【纠正 Explore 误判】控制中心析构 UAF —— 实际被 RAII 兜住，**非 P 级风险**
- `~ControlCenter()`（control_center.cc:46-51）函数体只 `lv_obj_del(container_)`，**未显式停 timer**。
- 但 `slider_timer_`/`network_confirm_timer_` 是 **ManagedTimer 成员对象**（control_center.h:111-112）。C++ 规则：函数体执行完后，成员按声明逆序自动析构，ManagedTimer 析构调 `Delete()`→`lv_timer_del`。故 timer **必在对象内存释放前被删**，回调不会再触发已析构对象。
- 同线程（LVGL）同步析构，`lv_obj_del` 与成员析构之间无 timer 回调插入窗口。**UAF 不成立，RAII 设计正是为此**。→ 归 🟢，无需改。
- （若改为裸 lv_timer_t* 成员，则 Explore 的 UAF 判断才成立——现状安全正因用了 ManagedTimer。）

### 2. 【P3】player_tick_ 裸 lv_timer_create 理论泄漏，实际无害
- ui_display.cc:1007 `player_tick_ = lv_timer_create(PlayerTickCb, 200, this)`，**未用 ManagedTimer**，全程只 pause（cc:1017）从不 delete，`~UiDisplay()`（cc:109-110）为空。
- 但 UiDisplay 是**全局单例 display**，生命周期=整个固件，懒创建一次、从不反复建删→无累积泄漏。PlayerTickCb 有 `active_scene_ != kPlayer` 守卫（cc:956）防越界。→ **P3 技术债**（一致性瑕疵：同模块两套 timer 管理风格），不卡量产。

### 3. 【P3】休眠状态「立即翻转+回调」与 AEC「请求-回写」风格不一致
- OnSleepClicked（control_center.cc:491-502）：`sleep_on_ = !sleep_on_` **立即翻 UI**（cc:495-497）再 `sleep_callback_(sleep_on_)`。若回调侧实际休眠设置失败，UI 已变→理论脱钩。
- 但休眠是本地纯软策略（5 分/无），回调几乎不会失败，且 `SetSleepState()`（cc:394-398）提供外部回写通道兜底。→ **P3**：与 AEC 的防脱钩范式不统一，体验风险极低。

### 4. 字体代理内存安全 —— 干净，无裸 memcpy/裸索引
- `g_text_font = BUILTIN_TEXT_FONT`（ui_display.cc:149）是 `lv_font_t` 整结构**按值赋值**（非 memcpy 缓冲），callbacks/cmaps 仍指 Flash rodata（合法只读）。
- 所有 cbin 加载点（cc:167/257）失败均 return，写 fallback 前 const_cast 仅改 RAM proxy 的 `.fallback` 字段（rodata 主体不写）。PSRAM 分配失败由 cbin_font_create 返回 NULL，被守卫拦。→ **无内存安全缺陷**。
- 初始化幂等：`s_text_font_proxy_initialized` 守卫（cc:148），InitTextFontProxy 在 SetupUI 入口（cc:114）+ LoadFallback 内（cc:159）双重防御。

### 5. control_center.cc 缓冲区 —— 全部 snprintf+sizeof，无溢出
- 4 处格式化均 `snprintf(buf, sizeof(buf), ...)`，buf[8]/[16] 对「100%」「100」绰绰有余（Explore 已逐点核，行号 cc:254/292/408/537 量级）。无裸 strcpy/memcpy/数组索引。

### 6. 控件生命周期 —— Show/Hide 只切 FLAG，不反复 create，无长跑泄漏
- CreateUI（control_center.cc:53）仅构造时调一次；Show/Hide（cc:334+）用 `LV_OBJ_FLAG_HIDDEN` 切换，ShowSlider/HideSlider 同理。反复呼出控制中心**不累积控件**。→ 无长跑泄漏。

### 7. LVGL timer double-free 地雷核实结论
- 全 display/ 下 `lv_timer_create` 4 处：widgets/managed_timer.h、core/managed_timer.h、ui_display.cc:1007(player)、lvgl_gif.cc:62(官方)。
- 真正经 CreateOnce 的单次 timer（slider/network_confirm）**均走 widgets 版 CreateOnce，已 `set_auto_delete(false)`**（cc:457、cc:302）→ **已防护，🟢**。
- player_tick 是周期 timer（repeat 无限），auto_delete 不触发，无 double-free 风险（见 §2）。→ **本模块无 double-free 地雷未防护项**。

---

## 小结

🟢 8 项　🔴 6 项　⚪ 7 项　🛡️ 3 项

一句话结论：UiDisplay/控制中心整体是官方没有的合理业务扩展，**内存安全与 LVGL timer 防护到位**（ManagedTimer RAII 把 UAF/double-free 全堵死，字体 RAM 代理正确绕开 rodata 崩溃，二次确认+请求回写防误触防脱钩，均 🟢必要）；过度优化集中在 **4 处死代码（FinishBootAndShowClock 函数 + ui/core/managed_timer.h、ui/theme/ui_config.h、ui/resources/ui_img_paths.h 三个整死文件，共约 340 行）+ 2 处注释与实现不符**，清掉即可，**无 P0/P1，最高 P3**。
