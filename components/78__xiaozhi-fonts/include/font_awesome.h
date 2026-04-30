#ifndef FONT_AWESOME_H
#define FONT_AWESOME_H

/*
 * FA 兼容宏（v3 · 2026-04-30 合并字体后）
 *
 * 21 个 emoji 来自 FA Free Regular（11 个 codepoint 重映射到可用字符）
 * 124 个 UI 图标来自 Phosphor Bold
 * 145 个字形全部编入 font_awesome_X_Y.c · 业务代码 lv_label_set_text(label, FONT_AWESOME_WIFI) 即可
 *
 * 字体路由（main/CMakeLists.txt）：
 *   BUILTIN_ICON_FONT = font_awesome_30_4 (P30 三 SKU 状态栏)
 *   large_icon_font   = font_awesome_30_4 (LCD 表情大图标)
 *   OLED 30_1 走 font_awesome_30_1（emoji + UI 单色 1bpp）
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const char* name;
    const char* utf8_string;
} font_awesome_symbol_t;

#define FONT_AWESOME_NEUTRAL "\xef\x96\xa4"
#define FONT_AWESOME_HAPPY "\xef\x84\x98"
#define FONT_AWESOME_LAUGHING "\xef\x96\x9b"
#define FONT_AWESOME_FUNNY "\xef\x96\x88"
#define FONT_AWESOME_ANGRY "\xef\x95\x96"
#define FONT_AWESOME_CRYING "\xef\x96\xb3"
#define FONT_AWESOME_LOVING "\xef\x96\x84"
#define FONT_AWESOME_EMBARRASSED "\xef\x95\xb9"
#define FONT_AWESOME_WINKING "\xef\x93\x9a"
#define FONT_AWESOME_KISSY "\xef\x96\x98"
#define FONT_AWESOME_SAD "\xf0\x9f\x98\xa2"
#define FONT_AWESOME_SURPRISED "\xf0\x9f\x98\xae"
#define FONT_AWESOME_SHOCKED "\xf0\x9f\x98\xb3"
#define FONT_AWESOME_THINKING "\xf0\x9f\x99\x84"
#define FONT_AWESOME_COOL "\xef\x84\x98"
#define FONT_AWESOME_RELAXED "\xf0\x9f\x98\x8a"
#define FONT_AWESOME_DELICIOUS "\xf0\x9f\x98\x8d"
#define FONT_AWESOME_CONFIDENT "\xf0\x9f\x98\x89"
#define FONT_AWESOME_SLEEPY "\xf0\x9f\x98\xab"
#define FONT_AWESOME_SILLY "\xf0\x9f\x98\x9b"
#define FONT_AWESOME_CONFUSED "\xf0\x9f\x98\xb6"
#define FONT_AWESOME_BATTERY_FULL "\xee\x83\x80"
#define FONT_AWESOME_BATTERY_THREE_QUARTERS "\xee\x83\x82"
#define FONT_AWESOME_BATTERY_HALF "\xee\x83\x86"
#define FONT_AWESOME_BATTERY_QUARTER "\xee\x83\x84"
#define FONT_AWESOME_BATTERY_EMPTY "\xee\x82\xbe"
#define FONT_AWESOME_BATTERY_SLASH "\xee\x83\x88"
#define FONT_AWESOME_BATTERY_BOLT "\xee\x82\xba"
#define FONT_AWESOME_WIFI "\xee\x93\xaa"
#define FONT_AWESOME_WIFI_FAIR "\xee\x93\xae"
#define FONT_AWESOME_WIFI_WEAK "\xee\x93\xac"
#define FONT_AWESOME_WIFI_SLASH "\xee\x93\xb2"
#define FONT_AWESOME_SIGNAL "\xee\x85\x82"
#define FONT_AWESOME_SIGNAL_STRONG "\xee\x85\x82"
#define FONT_AWESOME_SIGNAL_GOOD "\xee\x85\x84"
#define FONT_AWESOME_SIGNAL_FAIR "\xee\x85\x88"
#define FONT_AWESOME_SIGNAL_WEAK "\xee\x85\x86"
#define FONT_AWESOME_SIGNAL_OFF "\xee\x85\x8c"
#define FONT_AWESOME_VOLUME_HIGH "\xee\x91\x8a"
#define FONT_AWESOME_VOLUME "\xee\x91\x8c"
#define FONT_AWESOME_VOLUME_LOW "\xee\x91\x8c"
#define FONT_AWESOME_VOLUME_XMARK "\xee\x91\x9c"
#define FONT_AWESOME_MUSIC "\xee\x8c\xbc"
#define FONT_AWESOME_PLAY "\xee\x8f\x90"
#define FONT_AWESOME_PAUSE "\xee\x8e\x9e"
#define FONT_AWESOME_STOP "\xee\x91\xac"
#define FONT_AWESOME_BACKWARD_STEP "\xee\x96\xa4"
#define FONT_AWESOME_FORWARD_STEP "\xee\x96\xa6"
#define FONT_AWESOME_CHECK "\xee\x86\x82"
#define FONT_AWESOME_XMARK "\xee\x93\xb6"
#define FONT_AWESOME_POWER_OFF "\xee\x8f\x9a"
#define FONT_AWESOME_GEAR "\xee\x89\xb0"
#define FONT_AWESOME_TRASH "\xee\x92\xa6"
#define FONT_AWESOME_HOUSE "\xee\x8b\x82"
#define FONT_AWESOME_IMAGE "\xee\x8b\x8a"
#define FONT_AWESOME_PEN_TO_SQUARE "\xee\xaf\x86"
#define FONT_AWESOME_COMMENT "\xee\x85\xa8"
#define FONT_AWESOME_COMMENT_QUESTION "\xee\x85\xae"
#define FONT_AWESOME_ARROW_LEFT "\xee\x81\x98"
#define FONT_AWESOME_ARROW_RIGHT "\xee\x81\xac"
#define FONT_AWESOME_ARROW_UP "\xee\x82\x8e"
#define FONT_AWESOME_ARROW_DOWN "\xee\x80\xbe"
#define FONT_AWESOME_ANGLE_LEFT "\xee\x84\xb8"
#define FONT_AWESOME_ANGLE_RIGHT "\xee\x84\xba"
#define FONT_AWESOME_ANGLE_UP "\xee\x84\xbc"
#define FONT_AWESOME_ANGLE_DOWN "\xee\x84\xb6"
#define FONT_AWESOME_ANGLES_LEFT "\xee\x84\xa8"
#define FONT_AWESOME_ANGLES_RIGHT "\xee\x84\xaa"
#define FONT_AWESOME_ANGLES_UP "\xee\x84\xac"
#define FONT_AWESOME_ANGLES_DOWN "\xee\x84\xa6"
#define FONT_AWESOME_ARROWS_REPEAT "\xee\x82\x94"
#define FONT_AWESOME_ARROWS_ROTATE "\xee\x82\x96"
#define FONT_AWESOME_CLOUD_ARROW_DOWN "\xee\x86\xac"
#define FONT_AWESOME_CLOUD_ARROW_UP "\xee\x86\xae"
#define FONT_AWESOME_CLOUD_SLASH "\xee\xaa\x96"
#define FONT_AWESOME_SUN "\xee\x91\xb2"
#define FONT_AWESOME_MOON "\xee\x8c\xb0"
#define FONT_AWESOME_CLOUD "\xee\x86\xaa"
#define FONT_AWESOME_CLOUDS "\xee\x86\xaa"
#define FONT_AWESOME_CLOUD_SUN "\xee\x95\x80"
#define FONT_AWESOME_CLOUD_SUN_RAIN "\xee\x86\xb4"
#define FONT_AWESOME_CLOUD_MOON "\xee\x94\xbe"
#define FONT_AWESOME_CLOUD_BOLT "\xee\x86\xb2"
#define FONT_AWESOME_CLOUD_HAIL "\xee\x86\xb8"
#define FONT_AWESOME_CLOUD_SLEET "\xee\x86\xb8"
#define FONT_AWESOME_CLOUD_DRIZZLE "\xee\x86\xb4"
#define FONT_AWESOME_CLOUD_FOG "\xee\x94\xbc"
#define FONT_AWESOME_CLOUD_RAIN "\xee\x86\xb4"
#define FONT_AWESOME_CLOUD_SHOWERS "\xee\x86\xb4"
#define FONT_AWESOME_CLOUD_SHOWERS_HEAVY "\xee\x86\xb4"
#define FONT_AWESOME_SNOWFLAKE "\xee\x96\xaa"
#define FONT_AWESOME_SNOWFLAKES "\xee\x96\xaa"
#define FONT_AWESOME_SMOG "\xee\x94\xbc"
#define FONT_AWESOME_WIND "\xee\x97\x92"
#define FONT_AWESOME_HURRICANE "\xee\xa7\xba"
#define FONT_AWESOME_TORNADO "\xee\xa2\x8c"
#define FONT_AWESOME_TRIANGLE_EXCLAMATION "\xee\x93\xa0"
#define FONT_AWESOME_BELL "\xee\x83\x8e"
#define FONT_AWESOME_LOCATION_DOT "\xee\x8c\x96"
#define FONT_AWESOME_GLOBE "\xee\x8a\x88"
#define FONT_AWESOME_LOCATION_ARROW "\xee\xab\x9e"
#define FONT_AWESOME_SD_CARD "\xee\x99\xa4"
#define FONT_AWESOME_BLUETOOTH "\xee\x83\x9a"
#define FONT_AWESOME_MICROCHIP_AI "\xee\x98\x90"
#define FONT_AWESOME_GPS "\xee\xb7\x98"
#define FONT_AWESOME_GPS_FIX "\xee\xb7\x96"
#define FONT_AWESOME_GPS_SLASH "\xee\xb7\x94"
#define FONT_AWESOME_BLUETOOTH_CONNECTED "\xee\x83\x9c"
#define FONT_AWESOME_BLUETOOTH_SLASH "\xee\x83\x9e"
#define FONT_AWESOME_BLUETOOTH_X "\xee\x83\xa0"
#define FONT_AWESOME_BROADCAST "\xee\x83\xb2"
#define FONT_AWESOME_NFC "\xee\x83\xb2"
#define FONT_AWESOME_IBEACON "\xee\x83\xb2"
#define FONT_AWESOME_CELL_TOWER "\xee\xae\xaa"
#define FONT_AWESOME_USER "\xee\x93\x82"
#define FONT_AWESOME_USER_ROBOT "\xee\x9d\xa2"
#define FONT_AWESOME_DOWNLOAD "\xee\x88\x8c"
#define FONT_AWESOME_LOCK "\xee\x8b\xba"
#define FONT_AWESOME_UNLOCK "\xee\x8c\x86"
#define FONT_AWESOME_KEY "\xee\x8b\x96"
#define FONT_AWESOME_LINK "\xee\x8b\xa2"
#define FONT_AWESOME_CIRCLE_INFO "\xee\x8b\x8e"
#define FONT_AWESOME_CIRCLE_QUESTION "\xee\x8f\xa8"
#define FONT_AWESOME_CIRCLE_CHECK "\xee\x86\x84"
#define FONT_AWESOME_CIRCLE_XMARK "\xee\x93\xb8"
#define FONT_AWESOME_CLOCK "\xee\x86\x9a"
#define FONT_AWESOME_ALARM_CLOCK "\xee\x80\x86"
#define FONT_AWESOME_SPINNER "\xee\xad\x84"
#define FONT_AWESOME_TEMPERATURE_HALF "\xee\x97\x8c"
#define FONT_AWESOME_HEADPHONES "\xee\x8a\xa6"
#define FONT_AWESOME_MICROPHONE "\xee\x8c\xa6"
#define FONT_AWESOME_MICROPHONE_SLASH "\xee\x8c\xa8"
#define FONT_AWESOME_CAMERA "\xee\x84\x8e"
#define FONT_AWESOME_CALENDAR "\xee\x84\x88"
#define FONT_AWESOME_ENVELOPE "\xee\x88\x94"
#define FONT_AWESOME_BRIGHTNESS "\xee\x91\xb4"
#define FONT_AWESOME_PHONE "\xee\x8e\xb8"
#define FONT_AWESOME_COMPASS "\xee\x87\x88"
#define FONT_AWESOME_CALCULATOR "\xee\x94\xb8"
#define FONT_AWESOME_GLASSES "\xee\x9e\xba"
#define FONT_AWESOME_MAGNIFYING_GLASS "\xee\x8c\x8c"
#define FONT_AWESOME_HEART "\xee\x8a\xa8"
#define FONT_AWESOME_STAR "\xee\x91\xaa"
#define FONT_AWESOME_GAMEPAD "\xee\x89\xae"
#define FONT_AWESOME_WATCH "\xee\x93\xa6"

extern const font_awesome_symbol_t font_awesome_symbols[];
extern const size_t font_awesome_symbol_count;

// emoji 名 → UTF-8 串（lcd/oled SetEmotion 调用 · 仅查 emoji 子集）
static inline const char* font_awesome_get_utf8(const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < font_awesome_symbol_count; i++) {
        if (strcmp(font_awesome_symbols[i].name, name) == 0) {
            return font_awesome_symbols[i].utf8_string;
        }
    }
    return NULL;
}

#endif
