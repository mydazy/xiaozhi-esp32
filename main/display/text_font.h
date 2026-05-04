/*
 * RAM-resident proxy for BUILTIN_TEXT_FONT.
 *
 * BUILTIN_TEXT_FONT (e.g. font_mydazy_text_20_4) lives in Flash .rodata, so its
 * `fallback` field cannot be written at runtime — doing so triggers a Cache
 * error ("Dbus write to cache rejected"). To attach a runtime-loaded cbin
 * fallback (e.g. font_maru_common_20_4 from PSRAM) we maintain a value-copy of
 * BUILTIN_TEXT_FONT in RAM and substitute &g_text_font for &BUILTIN_TEXT_FONT
 * at every label / theme attachment site.
 *
 * Initialization is idempotent and lazy: InitTextFontProxy() is called from
 * UiDisplay::SetupUI() entry; LoadFallbackTextFont() (via cbin) writes the
 * .fallback field afterwards.
 */
#pragma once
#include <lvgl.h>

extern lv_font_t g_text_font;     // RAM proxy of BUILTIN_TEXT_FONT
void InitTextFontProxy();         // idempotent; safe to call multiple times
