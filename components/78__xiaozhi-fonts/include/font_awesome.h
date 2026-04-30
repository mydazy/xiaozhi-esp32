#ifndef FONT_AWESOME_H
#define FONT_AWESOME_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// 符号结构体
typedef struct {
    const char* name;
    const char* utf8_string;
} font_awesome_symbol_t;

#define FONT_AWESOME_NEUTRAL "\xef\x96\xa4"
#define FONT_AWESOME_HAPPY "\xef\x84\x98"
#define FONT_AWESOME_LAUGHING "\xef\x96\x9b"
#define FONT_AWESOME_FUNNY "\xef\x96\x88"
#define FONT_AWESOME_SAD "\xee\x8e\x84"
#define FONT_AWESOME_ANGRY "\xef\x95\x96"
#define FONT_AWESOME_CRYING "\xef\x96\xb3"
#define FONT_AWESOME_LOVING "\xef\x96\x84"
#define FONT_AWESOME_EMBARRASSED "\xef\x95\xb9"
#define FONT_AWESOME_SURPRISED "\xee\x8d\xab"
#define FONT_AWESOME_SHOCKED "\xee\x8d\xb5"
#define FONT_AWESOME_THINKING "\xee\x8e\x9b"
#define FONT_AWESOME_WINKING "\xef\x93\x9a"
#define FONT_AWESOME_COOL "\xee\x8e\x98"
#define FONT_AWESOME_RELAXED "\xee\x8e\x92"
#define FONT_AWESOME_DELICIOUS "\xee\x8d\xb2"
#define FONT_AWESOME_KISSY "\xef\x96\x98"
#define FONT_AWESOME_CONFIDENT "\xee\x90\x89"
#define FONT_AWESOME_SLEEPY "\xee\x8e\x8d"
#define FONT_AWESOME_SILLY "\xee\x8e\xa4"
#define FONT_AWESOME_CONFUSED "\xee\x8d\xad"

// 符号数据表声明
extern const font_awesome_symbol_t font_awesome_symbols[];
extern const size_t font_awesome_symbol_count;

// 内联函数实现（emoji 名 → UTF-8 串 · 仅查 21 个 emoji 表）
static inline const char* font_awesome_get_utf8(const char* name) {
    if (!name) return NULL;

    for (size_t i = 0; i < font_awesome_symbol_count; i++) {
        if (strcmp(font_awesome_symbols[i].name, name) == 0) {
            return font_awesome_symbols[i].utf8_string;
        }
    }
    return NULL;
}

// UI 图标宏（FONT_AWESOME_WIFI / SIGNAL_* / BATTERY_* / VOLUME_* / PLAY / PAUSE / GEAR / ...）
// 由 font_phosphor.h 提供 alias，调用点 #include "font_awesome.h" 即可
#include "font_phosphor.h"

#endif
