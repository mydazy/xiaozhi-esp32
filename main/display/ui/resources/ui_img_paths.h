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

#endif // UI_IMG_PATHS_H
