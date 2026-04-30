#include "font_awesome.h"

// 21 个 emoji 数据表（lcd/oled SetEmotion 反查 · UI 图标无需 runtime 查表）
const font_awesome_symbol_t font_awesome_symbols[] = {
    {"neutral", "\xef\x96\xa4"},
    {"happy", "\xef\x84\x98"},
    {"laughing", "\xef\x96\x9b"},
    {"funny", "\xef\x96\x88"},
    {"angry", "\xef\x95\x96"},
    {"crying", "\xef\x96\xb3"},
    {"loving", "\xef\x96\x84"},
    {"embarrassed", "\xef\x95\xb9"},
    {"winking", "\xef\x93\x9a"},
    {"kissy", "\xef\x96\x98"},
    {"sad", "\xf0\x9f\x98\xa2"},
    {"surprised", "\xf0\x9f\x98\xae"},
    {"shocked", "\xf0\x9f\x98\xb3"},
    {"thinking", "\xf0\x9f\x99\x84"},
    {"cool", "\xef\x84\x98"},
    {"relaxed", "\xf0\x9f\x98\x8a"},
    {"delicious", "\xf0\x9f\x98\x8d"},
    {"confident", "\xf0\x9f\x98\x89"},
    {"sleepy", "\xf0\x9f\x98\xab"},
    {"silly", "\xf0\x9f\x98\x9b"},
    {"confused", "\xf0\x9f\x98\xb6"},
};

const size_t font_awesome_symbol_count = sizeof(font_awesome_symbols) / sizeof(font_awesome_symbols[0]);
