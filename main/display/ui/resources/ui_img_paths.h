#ifndef UI_IMG_PATHS_H
#define UI_IMG_PATHS_H

// ============================================================================
// UI 图片文件名定义
// 对应 assets 分区中的 .bin 文件名（由 build_default_assets.py 打包）
// 运行时通过 UiImageManager 从 mmap assets 加载
// ============================================================================

// 菜单图标
#define IMG_FILE_APP_CHAT           "app_chat.bin"
#define IMG_FILE_APP_TIME_ROOM      "app_time_room.bin"
#define IMG_FILE_APP_PHOTO          "app_photo.bin"
#define IMG_FILE_APP_LOGO           "app_logo.bin"
#define IMG_FILE_APP_POMODORO       "app_pomodoro.bin"
#define IMG_FILE_APP_ALARM          "app_alarm.bin"
#define IMG_FILE_APP_CLOCK          "app_clock.bin"
#define IMG_FILE_APP_TODO           "app_todo.bin"

// 功能图标
#define IMG_FILE_ICON_BACK          "icon_back.bin"
#define IMG_FILE_ICON_POMODORO_START "icon_pomodoro_start.bin"
#define IMG_FILE_ICON_POMODORO_STOP  "icon_pomodoro_stop.bin"
#define IMG_FILE_ICON_CHECKBOX       "icon_checkbox.bin"
#define IMG_FILE_ICON_CHECKBOX_CHECKED "icon_checkbox_checked.bin"
#define IMG_FILE_ICON_ALARM         "icon_alarm.bin"

// 照片墙导航按钮
#define IMG_FILE_BUTTON_LEFT        "button_left.bin"
#define IMG_FILE_BUTTON_RIGHT       "button_right.bin"

// 电池图标（按电量等级映射到实际文件名）
#define IMG_FILE_ICON_BATTERY_0     "icon_battery_10.bin"   // 0-20%
#define IMG_FILE_ICON_BATTERY_1     "icon_battery_20.bin"   // 20-40%
#define IMG_FILE_ICON_BATTERY_2     "icon_battery_40.bin"   // 40-60%
#define IMG_FILE_ICON_BATTERY_3     "icon_battery_60.bin"   // 60-80%
#define IMG_FILE_ICON_BATTERY_4     "icon_battery.bin"      // 80-100%
#define IMG_FILE_ICON_BATTERY_CHARGE "icon_battery_80.bin"  // 充电中
#define IMG_FILE_ICON_BELL          "icon_bell.bin"

// 信号图标
#define IMG_FILE_SIGNAL_WIFI        "icon_signal_wifi.bin"
#define IMG_FILE_SIGNAL_WIFI_0      "icon_signal_wifi_0.bin"
#define IMG_FILE_SIGNAL_WIFI_1      "icon_signal_wifi_1.bin"
#define IMG_FILE_SIGNAL_WIFI_2      "icon_signal_wifi_2.bin"
#define IMG_FILE_SIGNAL_4G          "icon_signal_4g.bin"
#define IMG_FILE_SIGNAL_4G_1        "icon_signal_4g_1.bin"
#define IMG_FILE_SIGNAL_4G_2        "icon_signal_4g_2.bin"
#define IMG_FILE_SIGNAL_4G_3        "icon_signal_4g_3.bin"
#define IMG_FILE_SIGNAL_4G_4        "icon_signal_4g_4.bin"

#endif // UI_IMG_PATHS_H
