"""
font_awesome.py — 单字体合并生成器（v3 · 2026-04-30 用户拍板：合并 phosphor + emoji）

原本是 FA 6 Free 字体生成器，重构为"FA 名 → Phosphor / FA-Free 双源合并"：
  - 21 个 emoji（informational 宏 FONT_AWESOME_HAPPY 等）→ FA Free Regular 字形
  - 124 个 UI 图标（FONT_AWESOME_WIFI / SIGNAL_* / BATTERY_* 等）→ Phosphor Bold 字形

输出：单一字体 font_awesome_X_Y.c，包含 145 个字形，业务代码 #include "font_awesome.h" 即可。
不再有 font_phosphor.* 双源结构。

emoji 重映射（11 个 FA Pro 专属字符 → FA Free 可用 codepoint）：
  sad/surprised/shocked/thinking/cool/relaxed/delicious/confident/sleepy/silly/confused
  → 全部映射到 face-* 系列（U+F119 / U+1F60x / U+1F62x），视觉略有差异但语义相近

字体源（./ttf/）：
  - Phosphor-Bold.ttf       → 124 UI 图标（codepoint 在 0xE000-0xEDFF 区段）
  - fa-regular-400.ttf      → 21 emoji（codepoint 散布在 0xF118-0xF5A4 + U+1F60x）

调用：
  python3 font_awesome.py lvgl --font-size 20 --bpp 4
  python3 font_awesome.py generate
"""
import os
import sys
import argparse

# ============================================================================
# 21 个 emoji（lcd/oled SetEmotion 用）
# 11 个 FA Pro 专属 codepoint 重映射到 FA Free 可用字符（标 [remapped]）
# ============================================================================
emoji_mapping = {
    # FA Free Regular 原生 10 个（视觉与原版一致）
    "neutral":     0xf5a4,  # face-meh
    "happy":       0xf118,  # face-smile-beam
    "laughing":    0xf59b,  # face-laugh-squint
    "funny":       0xf588,  # face-grin-tears
    "angry":       0xf556,  # face-angry
    "crying":      0xf5b3,  # face-sad-cry
    "loving":      0xf584,  # face-grin-hearts
    "embarrassed": 0xf579,  # face-flushed
    "winking":     0xf4da,  # face-grin-wink
    "kissy":       0xf598,  # face-kiss-wink-heart

    # FA Pro 专属 → FA Free 替代（11 个 · [remapped] 标记）
    "sad":         0x1f622,  # [remapped] U+E384 → face-sad-tear（流泪脸）
    "surprised":   0x1f62e,  # [remapped] U+E36B → face-surprise
    "shocked":     0x1f633,  # [remapped] U+E375 → face-flushed
    "thinking":    0x1f644,  # [remapped] U+E39B → face-rolling-eyes（翻白眼）
    "cool":        0xf118,   # [remapped] U+E398 → 复用 face-smile-beam（happy）
    "relaxed":     0x1f60a,  # [remapped] U+E392 → face-smile-beam
    "delicious":   0x1f60d,  # [remapped] U+E372 → face-grin-hearts
    "confident":   0x1f609,  # [remapped] U+E409 → face-smile-wink
    "sleepy":      0x1f62b,  # [remapped] U+E38D → face-tired
    "silly":       0x1f61b,  # [remapped] U+E3A4 → face-grin-tongue
    "confused":    0x1f636,  # [remapped] U+E36D → face-meh-blank
}

# ============================================================================
# 124 个 UI 图标 · FA name → Phosphor codepoint
# 由 docs/font/fa-to-phosphor-audit.md 拍板（用户 2026-04-30 全部 OK）
# ============================================================================
ui_icon_mapping = {
    # battery (7)
    "battery_full":             0xe0c0,
    "battery_three_quarters":   0xe0c2,
    "battery_half":             0xe0c6,
    "battery_quarter":          0xe0c4,
    "battery_empty":            0xe0be,
    "battery_slash":            0xe0c8,
    "battery_bolt":             0xe0ba,

    # wifi (4)
    "wifi":                     0xe4ea,
    "wifi_fair":                0xe4ee,
    "wifi_weak":                0xe4ec,
    "wifi_slash":               0xe4f2,

    # cellular signal (6)
    "signal":                   0xe142,
    "signal_strong":            0xe142,
    "signal_good":              0xe144,
    "signal_fair":              0xe148,
    "signal_weak":              0xe146,
    "signal_off":               0xe14c,

    # volume / speaker (4)
    "volume_high":              0xe44a,
    "volume":                   0xe44c,
    "volume_low":               0xe44c,
    "volume_xmark":             0xe45c,

    # media controls (6)
    "music":                    0xe33c,
    "play":                     0xe3d0,
    "pause":                    0xe39e,
    "stop":                     0xe46c,
    "backward_step":            0xe5a4,
    "forward_step":             0xe5a6,

    # ui controls (10)
    "check":                    0xe182,
    "xmark":                    0xe4f6,
    "power_off":                0xe3da,
    "gear":                     0xe270,
    "trash":                    0xe4a6,
    "house":                    0xe2c2,
    "image":                    0xe2ca,
    "pen_to_square":            0xebc6,
    "comment":                  0xe168,
    "comment_question":         0xe16e,

    # arrows (4)
    "arrow_left":               0xe058,
    "arrow_right":              0xe06c,
    "arrow_up":                 0xe08e,
    "arrow_down":               0xe03e,

    # angles / chevrons (8)
    "angle_left":               0xe138,
    "angle_right":              0xe13a,
    "angle_up":                 0xe13c,
    "angle_down":               0xe136,
    "angles_left":              0xe128,
    "angles_right":             0xe12a,
    "angles_up":                0xe12c,
    "angles_down":              0xe126,

    # rotate (2)
    "arrows_repeat":            0xe094,
    "arrows_rotate":            0xe096,

    # cloud upload/download (3)
    "cloud_arrow_down":         0xe1ac,
    "cloud_arrow_up":           0xe1ae,
    "cloud_slash":              0xea96,

    # weather (22)
    "sun":                      0xe472,
    "moon":                     0xe330,
    "cloud":                    0xe1aa,
    "clouds":                   0xe1aa,
    "cloud_sun":                0xe540,
    "cloud_sun_rain":           0xe1b4,
    "cloud_moon":               0xe53e,
    "cloud_bolt":               0xe1b2,
    "cloud_hail":               0xe1b8,
    "cloud_sleet":              0xe1b8,
    "cloud_drizzle":            0xe1b4,
    "cloud_fog":                0xe53c,
    "cloud_rain":               0xe1b4,
    "cloud_showers":            0xe1b4,
    "cloud_showers_heavy":      0xe1b4,
    "snowflake":                0xe5aa,
    "snowflakes":               0xe5aa,
    "smog":                     0xe53c,
    "wind":                     0xe5d2,
    "hurricane":                0xe9fa,
    "tornado":                  0xe88c,

    # misc icons (8)
    "triangle_exclamation":     0xe4e0,
    "bell":                     0xe0ce,
    "location_dot":             0xe316,
    "globe":                    0xe288,
    "location_arrow":           0xeade,
    "sd_card":                  0xe664,
    "bluetooth":                0xe0da,
    "microchip_ai":             0xe610,

    # 硬件状态扩展 · GPS / NFC / iBeacon / 蓝牙变体（P30-4G/P31）
    "gps":                      0xedd8,
    "gps_fix":                  0xedd6,
    "gps_slash":                0xedd4,
    "bluetooth_connected":      0xe0dc,
    "bluetooth_slash":          0xe0de,
    "bluetooth_x":              0xe0e0,
    "broadcast":                0xe0f2,
    "nfc":                      0xe0f2,
    "ibeacon":                  0xe0f2,
    "cell_tower":               0xebaa,

    # user / download (3)
    "user":                     0xe4c2,
    "user_robot":               0xe762,
    "download":                 0xe20c,

    # added 20250827 (29)
    "lock":                     0xe2fa,
    "unlock":                   0xe306,
    "key":                      0xe2d6,
    "link":                     0xe2e2,
    "circle_info":              0xe2ce,
    "circle_question":          0xe3e8,
    "circle_check":             0xe184,
    "circle_xmark":             0xe4f8,
    "clock":                    0xe19a,
    "alarm_clock":              0xe006,
    "spinner":                  0xeb44,
    "temperature_half":         0xe5cc,
    "headphones":               0xe2a6,
    "microphone":               0xe326,
    "microphone_slash":         0xe328,
    "camera":                   0xe10e,
    "calendar":                 0xe108,
    "envelope":                 0xe214,
    "brightness":               0xe474,
    "phone":                    0xe3b8,
    "compass":                  0xe1c8,
    "calculator":               0xe538,
    "glasses":                  0xe7ba,
    "magnifying_glass":         0xe30c,
    "heart":                    0xe2a8,
    "star":                     0xe46a,
    "gamepad":                  0xe26e,
    "watch":                    0xe4e6,
}

# 合并：业务调用所有图标都通过 FONT_AWESOME_<NAME> 宏访问
icon_mapping = {**emoji_mapping, **ui_icon_mapping}


def parse_arguments():
    parser = argparse.ArgumentParser(description='Font Awesome merged generator (Phosphor Bold + FA Free)')
    parser.add_argument('type', choices=['lvgl', 'dump', 'generate'])
    parser.add_argument('--font-size', type=int, default=20)
    parser.add_argument('--bpp', type=int, default=4)
    return parser.parse_args()


def main():
    args = parse_arguments()
    if args.type == "generate":
        return 0 if generate_symbols_header_file() else 1

    phosphor_ttf = "./ttf/Phosphor-Bold.ttf"
    fa_ttf = "./ttf/fa-regular-400.ttf"

    if not os.path.exists(phosphor_ttf):
        print(f"⚠ {phosphor_ttf} 缺失")
        return 1
    if not os.path.exists(fa_ttf):
        print(f"⚠ {fa_ttf} 缺失")
        return 1

    # 分组 codepoint
    ui_cps = sorted(set(ui_icon_mapping.values()))
    emoji_cps = sorted(set(emoji_mapping.values()))
    ui_range = ",".join(map(hex, ui_cps))
    emoji_range = ",".join(map(hex, emoji_cps))

    flags = "--no-compress --no-prefilter --force-fast-kern-format"

    if args.type == "lvgl":
        output = f"src/font_awesome_{args.font_size}_{args.bpp}.c"
        cmd = (
            f"lv_font_conv {flags}"
            f" --font {phosphor_ttf} --range {ui_range}"
            f" --font {fa_ttf} --range {emoji_range}"
            f" --format lvgl --lv-include lvgl.h --bpp {args.bpp}"
            f" -o {output} --size {args.font_size}"
        )
    else:
        output = f"./build/font_awesome_dump"
        import shutil
        shutil.rmtree(output, ignore_errors=True)
        os.makedirs(output, exist_ok=True)
        cmd = (
            f"lv_font_conv {flags}"
            f" --font {phosphor_ttf} --range {ui_range}"
            f" --font {fa_ttf} --range {emoji_range}"
            f" --format dump --bpp {args.bpp}"
            f" -o {output} --size {args.font_size}"
        )

    print(f"UI icons (Phosphor): {len(ui_cps)} codepoints")
    print(f"Emoji (FA Free):     {len(emoji_cps)} codepoints")
    print(f"Total:               {len(ui_cps) + len(emoji_cps)} codepoints")
    print(f"Generating {output}")

    ret = os.system(cmd)
    if ret != 0:
        print(f"命令执行失败，返回码：{ret}")
        return ret
    print("命令执行成功")
    return 0


def generate_symbols_header_file():
    """生成 include/font_awesome.h（145 个 FONT_AWESOME_* 宏 · emoji + UI 合并）"""
    header_content = """#ifndef FONT_AWESOME_H
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

"""
    # 全部 FONT_AWESOME_* 宏（emoji + UI · 145 个）
    for k, v in icon_mapping.items():
        ch = chr(v)
        utf8 = ''.join(f'\\x{c:02x}' for c in ch.encode("utf-8"))
        header_content += f'#define FONT_AWESOME_{k.upper()} "{utf8}"\n'

    header_content += """
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
"""

    header_file = "include/font_awesome.h"
    with open(header_file, 'w', encoding='utf-8') as f:
        f.write(header_content)
    print(f"成功生成 {header_file}")

    # 实现文件 · 21 emoji 数据表（仅供 SetEmotion 反查）
    impl_content = """#include "font_awesome.h"

// 21 个 emoji 数据表（lcd/oled SetEmotion 反查 · UI 图标无需 runtime 查表）
const font_awesome_symbol_t font_awesome_symbols[] = {
"""
    for k, v in emoji_mapping.items():
        ch = chr(v)
        utf8 = ''.join(f'\\x{c:02x}' for c in ch.encode("utf-8"))
        impl_content += f'    {{"{k}", "{utf8}"}},\n'
    impl_content += """};

const size_t font_awesome_symbol_count = sizeof(font_awesome_symbols) / sizeof(font_awesome_symbols[0]);
"""

    impl_file = "src/font_awesome.c"
    with open(impl_file, 'w', encoding='utf-8') as f:
        f.write(impl_content)
    print(f"成功生成 {impl_file}（{len(emoji_mapping)} emoji + {len(ui_icon_mapping)} UI = {len(icon_mapping)} 宏）")
    return True


if __name__ == "__main__":
    sys.exit(main())
