#include "font_awesome.h"

// 符号数据表实现（21 个 emoji · 用于 oled_display.cc font_awesome_get_utf8）
const font_awesome_symbol_t font_awesome_symbols[] = {
    {"neutral", "\xef\x96\xa4"},
    {"happy", "\xef\x84\x98"},
    {"laughing", "\xef\x96\x9b"},
    {"funny", "\xef\x96\x88"},
    {"sad", "\xee\x8e\x84"},
    {"angry", "\xef\x95\x96"},
    {"crying", "\xef\x96\xb3"},
    {"loving", "\xef\x96\x84"},
    {"embarrassed", "\xef\x95\xb9"},
    {"surprised", "\xee\x8d\xab"},
    {"shocked", "\xee\x8d\xb5"},
    {"thinking", "\xee\x8e\x9b"},
    {"winking", "\xef\x93\x9a"},
    {"cool", "\xee\x8e\x98"},
    {"relaxed", "\xee\x8e\x92"},
    {"delicious", "\xee\x8d\xb2"},
    {"kissy", "\xef\x96\x98"},
    {"confident", "\xee\x90\x89"},
    {"sleepy", "\xee\x8e\x8d"},
    {"silly", "\xee\x8e\xa4"},
    {"confused", "\xee\x8d\xad"},
};

// 符号总数
const size_t font_awesome_symbol_count = sizeof(font_awesome_symbols) / sizeof(font_awesome_symbols[0]);
