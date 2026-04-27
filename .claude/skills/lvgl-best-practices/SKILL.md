---
name: lvgl-best-practices
description: LVGL v8/v9 在 ESP32-S3 上的最佳实践与避坑手册。场景触发词：LVGL、UI、页面、lv_obj、lv_timer、触屏、圆屏、刷新率、tearing、双缓冲、字体、滚动。自动被 lvgl-ui-fixer 加载。
---

# LVGL 在 ESP32-S3 上的最佳实践

本 skill 由 Claude Code 在处理 LVGL / UI 相关任务时自动加载。

---

## 一、任务上下文规则（铁律）

### 1.1 LVGL 不是线程安全的
- 所有 `lv_*` API 必须在 **LVGL 任务**上下文调用
- LVGL 任务通常是 `lvgl_port_task` 或自建的 "lvgl" 任务
- 跨任务改 UI → **必须**用 `lv_async_call(cb, user_data)`

### 1.2 常见违规
```cpp
// ❌ 错：WebSocket 回调（网络任务）里直接改 UI
void on_ws_message(char* msg) {
    lv_label_set_text(label, msg);  // 崩溃或渲染异常
}

// ✅ 对：扔到 LVGL 任务
struct UpdateData { char msg[128]; };
void on_ws_message(char* msg) {
    auto* d = new UpdateData;
    strncpy(d->msg, msg, sizeof(d->msg));
    lv_async_call([](void* p) {
        auto* d = (UpdateData*)p;
        lv_label_set_text(label, d->msg);
        delete d;
    }, d);
}
```

### 1.3 中断 / 定时器回调
- **ISR** 里绝对禁止调 `lv_*`
- `esp_timer_cb_t` 跑在专用 timer 任务，**也不是** LVGL 任务，同样要 `lv_async_call`
- `lv_timer_t`（LVGL 自己的 timer）跑在 LVGL 任务，可以直接用 lv API

---

## 二、对象生命周期（bug 重灾区）

### 2.1 核心规则
- `lv_obj_del(root)` 会**级联删除子对象**
- 但**关联的 lv_timer、lv_anim、外部持有的指针不会自动清理**
- 必须在 del 前主动清理，否则野指针 / 内存泄漏

### 2.2 模板（正确的页面生命周期）
```cpp
class PomodoroPage : public PageBase {
    lv_obj_t* container_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_timer_t* update_timer_ = nullptr;
    lv_anim_t tomato_anim_;
    bool anim_running_ = false;

    void OnEnter() override {
        container_ = lv_obj_create(lv_scr_act());
        time_label_ = lv_label_create(container_);

        update_timer_ = lv_timer_create(OnTimerCb, 1000, this);

        lv_anim_init(&tomato_anim_);
        lv_anim_set_var(&tomato_anim_, container_);
        lv_anim_set_values(&tomato_anim_, 0, 100);
        lv_anim_set_exec_cb(&tomato_anim_, SetAlphaCb);
        lv_anim_set_time(&tomato_anim_, 500);
        lv_anim_start(&tomato_anim_);
        anim_running_ = true;
    }

    void OnExit() override {
        if (update_timer_) {
            lv_timer_del(update_timer_);
            update_timer_ = nullptr;
        }
        if (anim_running_) {
            lv_anim_del(container_, SetAlphaCb);
            anim_running_ = false;
        }
        if (container_) {
            lv_obj_del(container_);
            container_ = nullptr;
            time_label_ = nullptr;
        }
    }

    static void OnTimerCb(lv_timer_t* t) {
        auto* self = (PomodoroPage*)lv_timer_get_user_data(t);
        self->UpdateTime();
    }
};
```

### 2.3 常见泄漏
- `lv_timer_create` 没有 `lv_timer_del`
- `lv_anim_start` 没有 cancel
- `lv_obj_add_event_cb` 注册了静态全局回调，指向已 del 页面的 user_data

---

## 三、性能优化

### 3.1 双缓冲配置
```c
// ESP-IDF LVGL port 示例
static lv_color_t buf1[SCREEN_W * 40];
static lv_color_t buf2[SCREEN_W * 40];
lv_disp_draw_buf_init(&draw_buf, buf1, buf2, SCREEN_W * 40);
```
- 缓冲大小：屏宽 × 40 ~ 80 行，平衡内存和流畅度
- 部分刷新模式（partial）比全屏刷新快 3-5 倍
- 双缓冲 buf 放内部 RAM + DMA 能力

### 3.2 刷新率
- LVGL 默认 30ms tick（~33 FPS）
- 想达到 60 FPS：`LV_DISP_DEF_REFR_PERIOD = 16`
- 但要保证屏幕 IC 能支持（有些 ST7789/GC9A01 bit rate 不够）

### 3.3 避免昂贵操作
- `lv_obj_del + create`（切页面时）比 `lv_obj_clean` + 复用慢
- 避免在 tick 里做 `lv_obj_set_style_*`（会触发重绘）
- 文本用 LV_FONT_DECLARE 宏，别每次算字体

---

## 四、触屏防抖 / 事件处理

### 4.1 快速返回
```cpp
// ❌ 错：event_cb 里做重活
static void on_button_click(lv_event_t* e) {
    http_request(...);  // 阻塞 5 秒
}

// ✅ 对：只做标志位 / 扔任务
static void on_button_click(lv_event_t* e) {
    xQueueSendFromISR(cmd_queue, &cmd, NULL);  // 快速返回
}
```

### 4.2 防抖
- LVGL 内置 indev 有 `LV_INDEV_DEF_READ_PERIOD`（默认 30ms）
- 双击 / 长按 用 `LV_EVENT_SHORT_CLICKED` / `LV_EVENT_LONG_PRESSED`
- 避免自己在 `LV_EVENT_PRESSED` 里写防抖逻辑

### 4.3 与下拉切换冲突
- 下拉切换是**已验证 OK 的禁区手势**（CLAUDE.md 第 7 节）
- 新加的手势 / 滑动必须**不与下拉切换冲突**
- 测试：新页面下拉必须能正常触发切换

---

## 五、圆屏适配

### 5.1 Safe Area
1.69" 圆屏（如 284×240）的**可视区域是内切圆**，四角会被裁：
```
可视半径 ≈ min(W, H) / 2 - margin
safe_area_x = (W - 2r) / 2
safe_area_y = (H - 2r) / 2
```

### 5.2 文本 / 图标定位
- 重要内容放中心 60% 区域
- 状态栏 / 底边避免贴边
- 长文本走**环形滚动**或 **淡出 mask**，不要硬裁

### 5.3 Mask Gradient
```cpp
// 顶部/底部淡出效果
lv_obj_t* mask = lv_obj_create(container);
// 配置 gradient, LV_OPA_COVER at center, LV_OPA_0 at edges
```

---

## 六、内存规划

### 6.1 LVGL 堆
- `LV_MEM_SIZE` 推荐 ≥ 64KB（`sdkconfig` 或 `lv_conf.h`）
- 建议放 PSRAM（`CONFIG_LV_MEM_CUSTOM=y` + custom malloc 指向 `MALLOC_CAP_SPIRAM`）

### 6.2 字体
- 内置字体（如 Chinese 20）放 Flash，不占 RAM
- 动态字体（cbin_font）解压到 PSRAM
- 多字号字体占用差异大：20px ~ 100KB，32px ~ 250KB

### 6.3 图片
- PNG 解码需 PSRAM buffer（约 W×H×4 字节）
- 推荐用 C array（`lv_img_dsc_t`）编译时处理
- 大图用 binary + LV_IMG_CF_TRUE_COLOR_ALPHA

---

## 七、xiaozhi-esp32 本项目特定约束

### 7.1 页面架构
- 查看 `main/display/ui/` 现有 UI 代码
- 是否已有 PageBase？如果没有 → **建议创建**（但新建 PageBase 本身是架构级改动，需 Jack 批准）
- 路由器 `page_router_switch()` 是否存在？

### 7.2 显示硬件
- LCD IC：多种（见 `main/display/lcd_display.cc`）
- 触屏 IC：AXS5106L（见 `components/esp_lcd_touch_axs5106l/`）
- 驱动细节见 `main/boards/<sku>/config.h`

### 7.3 禁区
- **下拉切换手势**不要动
- **首页状态栏 / 导航逻辑**不要重构

---

## 八、常见 bug 快查表

| 现象 | 根因 | 解决 |
|---|---|---|
| 页面切换偶发崩溃 | OnExit 漏释放资源 | 补齐 timer/anim 清理 |
| 文本显示乱码 | 字体未加载 / 编码错 | 检查 UTF-8、lv_font_declare |
| 触屏不灵敏 | event_cb 阻塞 | event_cb 只做转发 |
| 动画卡顿 | 刷新率低 / PSRAM 缓冲 | 双缓冲 + 内部 RAM buf |
| 内存泄漏 | timer 没清 / 对象循环引用 | 每个页面过 checklist |
| 跨页面音频不停 | TTS handle 未 stop | OnExit 必须 stop |

---

## 参考

- LVGL 官方文档：https://docs.lvgl.io/
- ESP-IDF LVGL port：`esp_lvgl_port` component
- 本项目 DESIGN.md（如存在）必须优先遵循
