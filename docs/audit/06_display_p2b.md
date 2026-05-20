# 第二轮补充-UI/widgets/资产

> 本轮聚焦 `ui_display.cc` / `control_center.cc` / `managed_timer.h` / `ui_image_manager.cc` / `assets.cc/.h`。
> 已对照第一轮 `06_display_assets.md`，**不重复**其 P0-1/P0-2/P1-1~P1-6/P2-1~P2-6/P3-1~P3-5。
> 下面均为第一轮未覆盖的新发现。判级口径同第一轮：assets 分区支持 `Assets::Download()` OTA 远程下载，故资产解析须按"半受控数据"对待。

---

## P0 必崩 / 安全漏洞

（本轮无新增 P0。第一轮已捕获资产边界 P1-2 与 GIF 越界读 P0-1；下列资产二次解析放大问题归 P1。）

---

## P1 高频崩溃 / 体验严重退化

### P1b-1 `ParseBinImage` ARGB 缓冲尺寸 `w*h*4` 整数溢出 → 堆溢出写
- **等级**：P1。判级理由：`argb_size = pixel_count * 4`，其中 `pixel_count = (uint32_t)w * h`。`w`/`h` 是 bin 头里的 `uint16_t`（攻击者/坏数据可填 0xFFFF）。`pixel_count` 最大 ~4.29e9 已接近 uint32 上界，再 `*4` 必然回绕成小值 → `lv_malloc(argb_size)` 分配过小缓冲，随后 `for(i<pixel_count)` 循环按真实大像素数写 `argb_buf[i*4+0..3]` → 堆越界写破坏。这是第一轮 P1-6（只覆盖**读** RGB 源越界）未涉及的**写端**溢出，独立漏洞。
- **文件**：`main/display/ui/resources/ui_image_manager.cc:136-145`
- **代码**：
```c
uint32_t pixel_count = (uint32_t)w * h;        // 0xFFFF*0xFFFF 溢出
uint32_t rgb_size = pixel_count * 2;
uint32_t argb_size = pixel_count * 4;          // 再 *4 回绕
uint8_t* argb_buf = static_cast<uint8_t*>(lv_malloc(argb_size));  // 过小
if (!argb_buf) { lv_free(dsc); return nullptr; }
for (uint32_t i = 0; i < pixel_count; i++) {   // 按真实大值写
    argb_buf[i * 4 + 0] = b;  ...             // 越界写
```
- **根因**：未对 `w`/`h` 上界做物理约束，乘法在 32 位域回绕；`stride/w==3` 校验只防 stride 字段，不防 w/h 自身。
- **触发条件与影响面**：损坏/篡改的 UI 图标 bin（OTA assets）。一旦 `ParseBinImage` 命中 `cf==0x0B` 且 w·h 极大即崩。
- **修复建议**：解析头后立即 `if (w == 0 || h == 0 || (uint64_t)w*h > kMaxPixels) return nullptr;`（kMaxPixels 按屏分辨率/PSRAM 上限，如 512*512）；所有尺寸运算用 `(uint64_t)` 并校验不溢出再 `lv_malloc`。

### P1b-2 `ParseBinImage` 非转换分支零拷贝指针无源长度校验
- **等级**：P1。判级理由：当 `cf != 0x0B`（或 stride/w 不满足）走 else 分支，直接 `dsc->data = src; dsc->data_size = src_size;`，把 `header->w/h/stride` 原样塞进 `lv_image_dsc_t`，**未校验 `stride * h <= src_size`**。若资产被截断或 stride/h 被篡改成大值，LVGL 渲染时按 `stride*h` 读像素 → 越界读 mmap 区外 → LoadProhibited。第一轮 P1-6 只覆盖 `cf==0x0B` 转换路径的 RGB 读，未覆盖此 else 直通路径。
- **文件**：`main/display/ui/resources/ui_image_manager.cc:163-169`
- **代码**：
```c
} else {
    dsc->header.cf = static_cast<lv_color_format_t>(header->cf);
    dsc->header.stride = header->stride;     // 未校验
    dsc->data = src;
    dsc->data_size = src_size;                // LVGL 后续按 stride*h 读，可 > src_size
}
```
- **根因**：直通路径缺 `stride*h <= src_size` 与 cf 合法性校验。
- **触发条件与影响面**：任何非 0x0B 格式的损坏/截断图标资产被 `Get()` 后交 `lv_image_set_src` 渲染。
- **修复建议**：else 分支前加 `if (header->stride == 0 || (uint64_t)header->stride * h > src_size) { lv_free(dsc); return nullptr; }`，并白名单校验 `cf` 取值。

### P1b-3 `GetAssetData` magic 校验前 `data[0]/data[1]` 读越界（offset 越界场景的具体放大点·补强第一轮）
- **等级**：P1。判级理由：第一轮 P1-2 已指出表项 offset/size 无分区边界校验。此处补强一个**独立且必经**的放大点：`mmap_root_ + asset->second.offset` 即便 offset 落在分区内但靠近末尾（`offset == partition_size-1`），`data[0]`/`data[1]` 读 2 字节即跨过 mmap 末页 → 越界读。`GetAssetData` 是字库/图像/emoji/srmodels/index.json 的**唯一入口**，每次资产访问都过这两行。第一轮修复建议提到加 `size<2` 判断，但漏了 `offset+2 <= partition_size` 的 magic 读上界本身。
- **文件**：`main/assets.cc:203-207`
- **代码**：
```c
auto data = (const char*)(mmap_root_ + asset->second.offset);
if (data[0] != 'Z' || data[1] != 'Z') {        // offset+1 可能越 mmap 末页
    ESP_LOGE(TAG, "...magic %02x%02x", data[0], data[1]);
    return false;
}
```
- **根因**：读 magic 前未保证 `offset + 2 <= partition_->size` 与 `offset + size <= partition_->size`。
- **触发条件与影响面**：表项 offset 被构造/损坏到分区尾部 2 字节内（OTA 未对齐/篡改）。
- **修复建议**：`InitializePartition` 构表时即丢弃 `offset+size > partition_->size || size < 2` 的条目（同第一轮 P1-2）；`GetAssetData` 内再补 `if (asset->second.offset + 2 > partition_->size) return false;` 双保险。注意 `partition_` 是 `Assets` 成员，`LvglStrategy` 需透过传入的 `assets` 指针访问 `assets->partition_->size`。

---

## P2 偶发 / 边缘场景

### P2b-1 `EnsureControlCenter` 的 Volume/Brightness 回调按引用捕获局部 `board`
- **等级**：P2。判级理由：`auto& board = Board::GetInstance();` 是局部引用，`SetVolumeCallback`/`SetBrightnessCallback` 的 lambda 用 `[&board]` 捕获**栈上引用**。`Board::GetInstance()` 返回的是单例，引用指向的对象生命周期 OK（进程级），所以当前不崩；但这是脆弱写法——若未来 `Board` 改为可重建/多实例，捕获的引用即悬挂。相邻的 `SetSleepCallback`/`SetNetworkCallback` 已改用 `Board::GetInstance()` 在 lambda 体内取，风格不一致。
- **文件**：`main/display/ui_display.cc:811,815-820`
- **代码**：
```c
auto& board = Board::GetInstance();
control_center_->SetVolumeCallback([&board](int v) {     // 捕获栈引用
    if (auto* codec = board.GetAudioCodec()) codec->SetOutputVolume(v);
});
control_center_->SetBrightnessCallback([&board](int v) { ... });
```
- **根因**：lambda 按引用捕获局部变量，存活期超过该局部作用域。
- **触发条件与影响面**：当前单例下不触发；属代码健壮性/可维护性隐患。
- **修复建议**：与 Sleep/Network 回调统一，lambda 体内 `auto& b = Board::GetInstance();` 取，不捕获局部引用。

### P2b-2 ControlCenter 析构顺序：`lv_obj_del(container_)` 与 `slider_timer_` 自动析构间存在悬挂回调窗口
- **等级**：P2。判级理由：`~ControlCenter()` 先 `lv_obj_del(container_)`（删除容器及子对象 `slider_container_`/`slider_`），随后成员 `slider_timer_`（ManagedTimer）析构才 `lv_timer_delete`。若析构发生时 slider 自动关闭定时器仍存活（3s 窗口内 Hide 没被调），在 `lv_obj_del` 之后、`slider_timer_` 析构前，LVGL 若处理该 timer 会触发 `OnSliderTimer → HideSlider`，访问已删除的 `slider_container_` → UAF。单线程 LVGL 下窗口极窄（析构与 timer handler 不会真正交错），故定 P2 而非 P1。
- **文件**：`main/display/ui/widgets/control_center.cc:44-49`（析构），`:334-340`（回调），`control_center.h:110`（成员声明顺序）
- **代码**：
```c
ControlCenter::~ControlCenter() {
    if (container_) {
        lv_obj_del(container_);   // 删除 slider_container_ 等子对象
        container_ = nullptr;
    }
}   // 之后 slider_timer_ 才析构 → 期间 timer 若触发访问已删子对象
```
- **根因**：未在删 UI 前显式 `StopSliderTimer()`/`slider_timer_.Delete()`。
- **触发条件与影响面**：控制中心对象销毁（理论上单例常驻不析构，但测试/重配路径存在）且滑块定时器在跑。
- **修复建议**：析构函数体首行先 `slider_timer_.Delete();` 再 `lv_obj_del(container_)`，明确生命周期顺序。

### P2b-3 `OnSliderTimer` / 各 `On*Clicked` 回调对 `self` 不判空即解引用
- **等级**：P2。判级理由：`OnSliderTimer`/`OnExitClicked`/`OnNetworkClicked` 等静态回调 `static_cast<ControlCenter*>(lv_timer_get_user_data/lv_event_get_user_data)` 后直接 `self->...`，无 `if(!self)return`。正常 user_data 恒为 `this` 不会空，但与 `ui_display.cc` 内 `OnFontExitClicked`/`OnPlayerPlayPauseClicked` 等"先判 self 再用"的防御风格不一致；若未来回调注册点改动或对象提前析构，直接空解引用崩。
- **文件**：`main/display/ui/widgets/control_center.cc:334-338,413-416,422-426` 等多处
- **代码**：`auto* self = static_cast<ControlCenter*>(...); self->HideSlider();  // 无 null 检查`
- **根因**：回调入口缺防御性空判。
- **触发条件与影响面**：边缘（对象生命周期异常）。
- **修复建议**：每个静态回调首行加 `if (!self) return;`，与项目其余 UI 回调统一。

### P2b-4 时钟/番茄钟主屏 `clock_big_font_` 缺失时大字降级到 `&g_text_font` 仍可能为空 proxy
- **等级**：P2。判级理由：`CreateClockPage`/`CreatePomodoroPage` 在 cbin 88px 字体加载失败时回退 `lv_obj_set_style_text_font(label, &g_text_font, 0)`。`g_text_font` 由 `InitTextFontProxy()` 从 `BUILTIN_TEXT_FONT` 按值复制初始化；但 `CreateClockPage` 由 `SwitchToClockMode` 触发，其内 `EnsureDisplayFonts` 之前并不保证 `InitTextFontProxy()` 已跑（仅 `SetupUI`/`LoadFallbackTextFont` 调过）。若 `CreateClockPage` 在 `SetupUI` 之前被某路径调用，`g_text_font` 是全局零初始化的空 `lv_font_t`（`get_glyph_dsc`/`line_height` 全 0）→ LVGL 取字时空回调指针崩。当前调用链有 `setup_ui_called_` 守门，故定 P2。
- **文件**：`main/display/ui_display.cc:237,1077`，`:151-158`（proxy 初始化）
- **代码**：
```c
if (!clock_big_font_) lv_obj_set_style_text_font(clock_time_label_, &g_text_font, 0);
```
- **根因**：`g_text_font` 初始化依赖 `InitTextFontProxy()` 先于任何使用点执行，未在 Create*Page 内强制保证。
- **触发条件与影响面**：异常调用序（Create*Page 早于 SetupUI）；cbin 字体未烧时叠加。
- **修复建议**：`CreateClockPage`/`CreatePomodoroPage` 开头调 `InitTextFontProxy();` 兜底，或断言 `s_text_font_proxy_initialized`。

### P2b-5 `ShowQrCode` 竖排色条 UTF-8 解析对截断多字节序列读越界
- **等级**：P2。判级理由：`make_bar` 与计数循环按首字节高位推进 `b = 1/2/3/4` 字节，随后 `memcpy(buf, p, b)`。若 `text` 末尾是被截断的多字节序列（如最后只剩 1 字节但首字节标记为 3 字节），`memcpy(buf,p,3)` 会读过字符串 NUL 终止符之后 → 越界读（buf[5] 本身够大不溢出，但源越界）。`left_label`/`right_label` 来自调用方（配网/绑定文案，多为常量，但绑定码/设备名可能拼接）。
- **文件**：`main/display/ui_display.cc:711-727`
- **代码**：
```c
int b = (*p & 0x80) == 0 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
char buf[5] = {};
memcpy(buf, p, b);          // p 后不足 b 字节时越界读
```
- **根因**：推进字节数只看首字节，未校验 `p[1..b-1]` 在串内（未判 NUL）。
- **触发条件与影响面**：传入非法/截断 UTF-8 的 label（边缘）。
- **修复建议**：memcpy 前 `for(int k=1;k<b;k++) if(!p[k]){b=k;break;}` 截到实际可读长度，或用 `strnlen` 限定。

### P2b-6 ControlCenter `OnSliderChanged` 步进对齐使音量/亮度无法到达上界 100
- **等级**：P2。判级理由：`value = (raw_value/SLIDER_STEP)*SLIDER_STEP`（STEP=5），slider range 0-100。raw=100 时 `100/5*5=100` OK；但亮度 range 是 `BRIGHTNESS_MIN(15)~100`，拖到 raw=99 → `99/5*5=95`，到 raw=100 才回 100，体验上"最后一格跳变"。更实质问题：`SetVolume`/`SetBrightness` 外部设值也做同样向下取整，外部设 73 → 显示/回调 70，与真实 codec/backlight 值产生持续偏差漂移（非崩溃，体验/一致性）。
- **文件**：`main/display/ui/widgets/control_center.cc:399-402,483`
- **代码**：`int aligned = (value / SLIDER_STEP) * SLIDER_STEP;`
- **根因**：单向向下取整，无四舍五入。
- **触发条件与影响面**：任意非 5 倍数的外部音量/亮度同步。
- **修复建议**：改四舍五入 `((value + STEP/2) / STEP) * STEP` 并 clamp 上界，或外部 Set* 不强制对齐、仅 UI 显示对齐。

---

## P3 潜在远期风险

### P3b-1 `Assets::Download` 失败提前 return 未恢复 mmap，且 `partition_` 在 OTA 后未重置时下载入口可空指针
- **等级**：P3。判级理由：`Download` 入口先 `UnApplyPartition()` 解除 mmap，之后任一失败分支（HTTP open/状态码/length/malloc 失败）直接 `return false`，**不重新 mmap**，使后续所有 `GetAssetData` 失败（字库/图像全 miss → UI 退化为空白/方框），需重启才恢复。另 `content_length > partition_->size` 比较：`content_length` 是 `size_t`，`partition_->size` 是 `uint32_t`；若 `partition_` 因极端时序为 nullptr（`FindPartition` 从未成功），此处解引用空指针。第一轮 P3-4 已提 Download 健壮性（超时/进度），但未提"失败不回滚 mmap 导致资产持续不可用"这一具体后果。
- **文件**：`main/assets.cc:432,438-457`
- **代码**：
```c
UnApplyPartition();                       // 先解除 mmap
auto http = network->CreateHttp(0);
if (!http->Open("GET", url)) { return false; }   // 失败后 mmap 未恢复
...
if (content_length > partition_->size) { ... }    // partition_ 若空则崩
```
- **根因**：失败路径不调 `InitializePartition()` 回滚；对 `partition_` 非空未先校验。
- **触发条件与影响面**：弱网/磁盘满/服务器异常导致 OTA 中断；之后设备 UI 资产全失效直到重启。
- **修复建议**：所有失败 return 前 `InitializePartition();` 重挂旧分区（旧数据仍在 flash，erase 前失败可恢复）；入口 `if (!partition_ && !FindPartition(this)) return false;`。

### P3b-2 `cbin_font_create` 返回字体的 `fallback` 链每次 `EnsureDisplayFonts` 重设，多次调用累积无害但 `g_text_font.fallback` 可被覆盖
- **等级**：P3。判级理由：`EnsureDisplayFonts` 用 `if(dst) return;` 防重复创建，但每次仍执行 `edu_main_font_->fallback = clock_text_font_` 等链接行（即便字体已存在，因为这些行在 lambda 之外）。`clock_text_font_->fallback` 在 `load()` 内被设为 `&g_text_font`；而 `LoadFallbackTextFont` 又设 `g_text_font.fallback = fallback_text_font_`。链 `edu→clock_text→g_text_font→fallback_text`。若 `LoadFallbackTextFont` 在某些路径晚于 `EnsureDisplayFonts` 才跑，存在短暂 `g_text_font.fallback==nullptr` 窗口（缺字渲染成方框，非崩溃）。第一轮 P3-3 提了 fallback 链环风险，此处补"链建立时序"问题。
- **文件**：`main/display/ui_display.cc:269,278-283`，`:180`
- **根因**：fallback 链分散在两个函数、依赖调用顺序。
- **修复建议**：集中在一处（如 SetupUI 末尾）一次性建链，并保证 `LoadFallbackTextFont` 先于 `EnsureDisplayFonts`。

### P3b-3 `ManagedTimer` 跨线程使用风险（LVGL timer 非线程安全）
- **等级**：P3。判级理由：`ManagedTimer::Create/Delete/Pause` 直接调 `lv_timer_*`，LVGL timer API 必须在持 LVGL 锁的上下文调用。`ControlCenter` 的 `StartSliderTimer`/`StopSliderTimer` 均由 LVGL 事件回调（已在 LVGL 任务内）触发，当前安全；但 `ManagedTimer` 作为通用封装无任何锁/线程断言，若未来被其他线程（如 application Schedule 外）误用即 data race 损坏 LVGL timer 链表。
- **文件**：`main/display/ui/core/managed_timer.h:54-78`
- **根因**：封装未约束/文档化"仅 LVGL 上下文调用"。
- **修复建议**：头注释明确线程约束，或在 Debug 下加 `lv_lock` 持有断言。

---

## 统计

| 等级 | 数量 |
|------|------|
| P0   | 0    |
| P1   | 3    |
| P2   | 6    |
| P3   | 3    |
| **合计** | **12** |

优先修复顺序：P1b-1（ARGB `w*h*4` 整数溢出堆写，OTA 图标可触发，第一轮只覆盖读端）、P1b-2（bin 非转换路径 stride*h 无源长度校验越界读）、P1b-3（GetAssetData magic 读上界）。三者均为"OTA 半受控资产 → 内存安全"，与第一轮 P0-1/P1-2/P1-6 同族，建议合并一次性补全资产解析的边界/溢出防护层。
