"""
Phosphor Bold 字体生成器（2026-04-30 用户拍板：全字号 Bold）

UI 图标主字体：替换 FA 6 Free 中的 wifi/signal/battery/volume/play/gear/...
emoji 不在 Phosphor 范围内 → 走 LVGL fallback 链回 font_awesome_X_Y。

字重选择历史：
  v1 (2026-04-30 早): Regular(14/16) + Fill(20/30) 字号路由 → 用户反馈 Regular 太细
  v2 (2026-04-30 晚): 全字号统一 Bold（保持线条风格但笔画更粗 · 用户拍板）

新增 GPS / NFC / iBeacon / 蓝牙变体（P31 NFC + P30-4G/P31 GPS 硬件支持）：
  gps / gps_fix / gps_slash · bluetooth_connected/slash/x · broadcast (NFC 用)

LVGL fallback：生成后做 sed 后处理，把 .fallback = NULL 替换成
  .fallback = (lv_font_t *)&font_awesome_X_Y
让 LVGL 渲染 emoji 字符（FA 私有区 0xe3xx/0xf1xx/...）时自动回退到 FA 字体。

调用约定：
  python3 font_phosphor.py lvgl --font-size 20 --bpp 4
  python3 font_phosphor.py generate
"""
import os
import re
import sys
import argparse

# ============================================================================
# 113 个 UI 图标 · FA name → Phosphor codepoint
# 由 docs/font/fa-to-phosphor-audit.md 拍板（用户 2026-04-30 全部 OK）
# ============================================================================
icon_mapping = {
    # battery (7)
    "battery_full":             0xe0c0,  # battery-full
    "battery_three_quarters":   0xe0c2,  # battery-high
    "battery_half":             0xe0c6,  # battery-medium
    "battery_quarter":          0xe0c4,  # battery-low
    "battery_empty":            0xe0be,  # battery-empty
    "battery_slash":            0xe0c8,  # battery-warning   ⚠语义微调：FA 禁用 → Phosphor 警告
    "battery_bolt":             0xe0ba,  # battery-charging

    # wifi (4)
    "wifi":                     0xe4ea,  # wifi-high
    "wifi_fair":                0xe4ee,  # wifi-medium
    "wifi_weak":                0xe4ec,  # wifi-low
    "wifi_slash":               0xe4f2,  # wifi-slash

    # cellular signal (6)
    "signal":                   0xe142,  # cell-signal-full （FA 0xf012 是 5 渐高 → 用 full 对齐）
    "signal_strong":            0xe142,  # cell-signal-full
    "signal_good":              0xe144,  # cell-signal-high
    "signal_fair":              0xe148,  # cell-signal-medium
    "signal_weak":              0xe146,  # cell-signal-low
    "signal_off":               0xe14c,  # cell-signal-slash

    # volume / speaker (4)
    "volume_high":              0xe44a,  # speaker-high
    "volume":                   0xe44c,  # speaker-low
    "volume_low":               0xe44c,  # speaker-low
    "volume_xmark":             0xe45c,  # speaker-x

    # media controls (5)
    "music":                    0xe33c,  # music-note
    "play":                     0xe3d0,  # play
    "pause":                    0xe39e,  # pause
    "stop":                     0xe46c,  # stop
    "backward_step":            0xe5a4,  # skip-back
    "forward_step":             0xe5a6,  # skip-forward

    # ui controls (10)
    "check":                    0xe182,  # check
    "xmark":                    0xe4f6,  # x
    "power_off":                0xe3da,  # power
    "gear":                     0xe270,  # gear
    "trash":                    0xe4a6,  # trash
    "house":                    0xe2c2,  # house
    "image":                    0xe2ca,  # image
    "pen_to_square":            0xebc6,  # pencil-simple-line
    "comment":                  0xe168,  # chat-circle
    "comment_question":         0xe16e,  # chat-circle-text  ⚠Phosphor 无问号气泡 → 文字气泡近似

    # arrows (4)
    "arrow_left":               0xe058,  # arrow-left
    "arrow_right":              0xe06c,  # arrow-right
    "arrow_up":                 0xe08e,  # arrow-up
    "arrow_down":               0xe03e,  # arrow-down

    # angles / chevrons (8)
    "angle_left":               0xe138,  # caret-left
    "angle_right":              0xe13a,  # caret-right
    "angle_up":                 0xe13c,  # caret-up
    "angle_down":               0xe136,  # caret-down
    "angles_left":              0xe128,  # caret-double-left
    "angles_right":             0xe12a,  # caret-double-right
    "angles_up":                0xe12c,  # caret-double-up
    "angles_down":              0xe126,  # caret-double-down

    # rotate (2)
    "arrows_repeat":            0xe094,  # arrows-clockwise
    "arrows_rotate":            0xe096,  # arrows-counter-clockwise

    # cloud upload/download (3)
    "cloud_arrow_down":         0xe1ac,  # cloud-arrow-down
    "cloud_arrow_up":           0xe1ae,  # cloud-arrow-up
    "cloud_slash":              0xea96,  # cloud-x

    # weather (22)
    "sun":                      0xe472,  # sun
    "moon":                     0xe330,  # moon
    "cloud":                    0xe1aa,  # cloud
    "clouds":                   0xe1aa,  # cloud           ⚠无复数 → 单云
    "cloud_sun":                0xe540,  # cloud-sun
    "cloud_sun_rain":           0xe1b4,  # cloud-rain      ⚠雨型粒度合并
    "cloud_moon":               0xe53e,  # cloud-moon
    "cloud_bolt":               0xe1b2,  # cloud-lightning
    "cloud_hail":               0xe1b8,  # cloud-snow      ⚠无冰雹 → 雪近似
    "cloud_sleet":              0xe1b8,  # cloud-snow
    "cloud_drizzle":            0xe1b4,  # cloud-rain
    "cloud_fog":                0xe53c,  # cloud-fog
    "cloud_rain":               0xe1b4,  # cloud-rain
    "cloud_showers":            0xe1b4,  # cloud-rain
    "cloud_showers_heavy":      0xe1b4,  # cloud-rain
    "snowflake":                0xe5aa,  # snowflake
    "snowflakes":               0xe5aa,  # snowflake       ⚠单复数合并
    "smog":                     0xe53c,  # cloud-fog       ⚠雾霾 → 雾近似
    "wind":                     0xe5d2,  # wind
    "hurricane":                0xe9fa,  # spiral          ⚠Phosphor 无 hurricane → spiral 近似
    "tornado":                  0xe88c,  # tornado

    # misc icons (8)
    "triangle_exclamation":     0xe4e0,  # warning
    "bell":                     0xe0ce,  # bell
    "location_dot":             0xe316,  # map-pin
    "globe":                    0xe288,  # globe
    "location_arrow":           0xeade,  # navigation-arrow
    "sd_card":                  0xe664,  # sim-card        ⚠无 sd-card → sim-card 近似（mapping 项目未调用）
    "bluetooth":                0xe0da,  # bluetooth
    "microchip_ai":             0xe610,  # cpu             ⚠无 AI 标签 → cpu 近似

    # 2026-04-30 扩展：硬件状态图标（P30-4G GPS / P31 NFC + iBeacon · 蓝牙状态变体）
    "gps":                      0xedd8,  # gps           标准 GPS 图标
    "gps_fix":                  0xedd6,  # gps-fix       已定位
    "gps_slash":                0xedd4,  # gps-slash     GPS 关闭
    "bluetooth_connected":      0xe0dc,  # bluetooth-connected  已连接（带圆点）
    "bluetooth_slash":          0xe0de,  # bluetooth-slash      已关闭
    "bluetooth_x":              0xe0e0,  # bluetooth-x          异常
    "broadcast":                0xe0f2,  # broadcast            无线广播（用作 NFC/iBeacon 标识）
    "nfc":                      0xe0f2,  # broadcast            ⚠Phosphor 无专用 NFC 字符，复用 broadcast
    "ibeacon":                  0xe0f2,  # broadcast            同上 · iBeacon 扫描标识
    "cell_tower":               0xebaa,  # cell-tower           蜂窝塔（4G 标识备用）

    # user / download (3)
    "user":                     0xe4c2,  # user
    "user_robot":               0xe762,  # robot           ⚠少 user 装饰
    "download":                 0xe20c,  # download-simple

    # added 20250827 (29)
    "lock":                     0xe2fa,  # lock
    "unlock":                   0xe306,  # lock-open
    "key":                      0xe2d6,  # key
    "link":                     0xe2e2,  # link
    "circle_info":              0xe2ce,  # info
    "circle_question":          0xe3e8,  # question
    "circle_check":             0xe184,  # check-circle
    "circle_xmark":             0xe4f8,  # x-circle
    "clock":                    0xe19a,  # clock
    "alarm_clock":              0xe006,  # alarm
    "spinner":                  0xeb44,  # circle-notch
    "temperature_half":         0xe5cc,  # thermometer-simple
    "headphones":               0xe2a6,  # headphones
    "microphone":               0xe326,  # microphone
    "microphone_slash":         0xe328,  # microphone-slash
    "camera":                   0xe10e,  # camera
    "calendar":                 0xe108,  # calendar
    "envelope":                 0xe214,  # envelope
    "brightness":               0xe474,  # sun-dim
    "phone":                    0xe3b8,  # phone
    "compass":                  0xe1c8,  # compass
    "calculator":               0xe538,  # calculator
    "glasses":                  0xe7ba,  # eyeglasses
    "magnifying_glass":         0xe30c,  # magnifying-glass
    "heart":                    0xe2a8,  # heart
    "star":                     0xe46a,  # star
    "gamepad":                  0xe26e,  # game-controller
    "watch":                    0xe4e6,  # watch
}


def parse_arguments():
    parser = argparse.ArgumentParser(description='Phosphor font converter utility')
    parser.add_argument('type', choices=['lvgl', 'dump', 'generate'], help='Output type: lvgl, dump, or generate header files')
    parser.add_argument('--font-size', type=int, default=20, help='Font size (default: 20)')
    parser.add_argument('--bpp', type=int, default=4, help='Bits per pixel (default: 4)')
    return parser.parse_args()


def get_font_file(font_size):
    """全字号统一 Phosphor Bold（v2 · 用户 2026-04-30 拍板）

    Bold 是"线条 + 笔画粗"风格，保留圆润转角的同时比 Regular 醒目。
    字号 14/16/20/30 全部用同一个 ttf，确保视觉一致。
    """
    return "./ttf/Phosphor-Bold.ttf"


def patch_fallback(c_file_path, fallback_symbol):
    """在生成的 .c 文件里把 .fallback = NULL 替换成 .fallback = (lv_font_t *)&fallback_symbol
    LVGL v9 字体声明里 .fallback 字段在 LV_VERSION_CHECK(8,2,0) 块内，需精确匹配。
    """
    with open(c_file_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # 必须先在文件头加上 fallback 字体的 extern 声明
    extern_decl = f"extern const lv_font_t {fallback_symbol};\n"
    # 紧贴 #include 块之后插入（找第一个空行 + extern）
    if extern_decl not in content:
        # 在第一个 #if ... #endif 块前插入
        match = re.search(r'(#if[ \t]+(?:LV_VERSION_CHECK|LVGL_VERSION_MAJOR))', content)
        if match:
            insert_pos = match.start()
            content = content[:insert_pos] + extern_decl + "\n" + content[insert_pos:]
        else:
            content = extern_decl + content

    # 替换 .fallback = NULL → .fallback = (lv_font_t *)&font_awesome_X_Y
    new_content, count = re.subn(
        r'\.fallback\s*=\s*NULL\s*,',
        f'.fallback = (lv_font_t *)&{fallback_symbol},',
        content,
    )
    if count == 0:
        print(f"⚠ {c_file_path}: 未找到 .fallback = NULL，跳过 fallback patch")
        return False

    with open(c_file_path, 'w', encoding='utf-8') as f:
        f.write(new_content)
    print(f"✅ {c_file_path}: fallback → {fallback_symbol}（patch {count} 处）")
    return True


def main():
    args = parse_arguments()

    if args.type == "generate":
        return 0 if generate_symbols_header_file() else 1

    symbols = list(icon_mapping.values())
    flags = "--no-compress --no-prefilter --force-fast-kern-format"
    font = get_font_file(args.font_size)

    if not os.path.exists(font):
        print(f"⚠ 字体文件缺失: {font}")
        print(f"  请把 Phosphor-Fill.ttf / Phosphor-Regular.ttf 放到 ./ttf/ 后重试")
        return 1

    symbols_str = ",".join(map(hex, symbols))

    if args.type == "lvgl":
        output = f"src/font_phosphor_{args.font_size}_{args.bpp}.c"
        cmd = f"lv_font_conv {flags} --font {font} --format lvgl --lv-include lvgl.h --bpp {args.bpp} -o {output} --size {args.font_size} -r {symbols_str}"
    else:  # dump
        output = f"./build/font_phosphor_dump"
        import shutil
        shutil.rmtree(output, ignore_errors=True)
        os.makedirs(output, exist_ok=True)
        cmd = f"lv_font_conv {flags} --font {font} --format dump --bpp {args.bpp} -o {output} --size {args.font_size} -r {symbols_str}"

    print(f"Total symbols: {len(symbols)}（去重 codepoint: {len(set(symbols))}）")
    print(f"Source font: {font}")
    print(f"Generating {output}")

    ret = os.system(cmd)
    if ret != 0:
        print(f"命令执行失败，返回码：{ret}")
        return ret
    print("命令执行成功")

    # 后处理：接 fallback 链到 font_awesome_X_Y
    if args.type == "lvgl":
        fallback_symbol = f"font_awesome_{args.font_size}_{args.bpp}"
        patch_fallback(output, fallback_symbol)

    return 0


def generate_symbols_header_file():
    """生成 include/font_phosphor.h
    内容：
      1) FONT_PHOSPHOR_<NAME> 宏（UTF-8 串）
      2) FONT_AWESOME_<NAME> alias 宏（保持业务调用点零改动）
      3) phosphor 符号查表 API（runtime 反查 utf8）
    """
    header_content = """#ifndef FONT_PHOSPHOR_H
#define FONT_PHOSPHOR_H

/*
 * Phosphor UI 图标字体宏（2026-04-30 接管 FA 6 Free 中的 UI 图标子集）
 *
 * 字体源：./ttf/Phosphor-Fill.ttf  (≥20px)
 *         ./ttf/Phosphor-Regular.ttf (≤16px)
 * LVGL 字体符号：font_phosphor_14_1 / font_phosphor_16_4 / font_phosphor_20_4 / font_phosphor_30_4
 * fallback 链：font_phosphor_X_Y -> font_awesome_X_Y（emoji 子集）
 *
 * 业务调用：保持 FONT_AWESOME_WIFI / SIGNAL_* / BATTERY_* / VOLUME_* 等宏，
 *          实际展开为 Phosphor codepoint（见末尾 alias 段）。
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    const char* name;
    const char* utf8_string;
} font_phosphor_symbol_t;

"""
    # 1) FONT_PHOSPHOR_* 宏
    for k, v in icon_mapping.items():
        ch = chr(v)
        utf8 = ''.join(f'\\x{c:02x}' for c in ch.encode("utf-8"))
        header_content += f'#define FONT_PHOSPHOR_{k.upper()} "{utf8}"\n'

    # 2) FONT_AWESOME_* alias（向后兼容业务调用点）
    header_content += "\n// FA 兼容 alias —— 业务调用点 FONT_AWESOME_WIFI 等沿用旧名，实际渲染 Phosphor 字形\n"
    for k in icon_mapping.keys():
        upper = k.upper()
        header_content += f'#define FONT_AWESOME_{upper} FONT_PHOSPHOR_{upper}\n'

    # 3) symbol table 声明 + lookup
    header_content += """
extern const font_phosphor_symbol_t font_phosphor_symbols[];
extern const size_t font_phosphor_symbol_count;

static inline const char* font_phosphor_get_utf8(const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < font_phosphor_symbol_count; i++) {
        if (strcmp(font_phosphor_symbols[i].name, name) == 0) {
            return font_phosphor_symbols[i].utf8_string;
        }
    }
    return NULL;
}

#endif
"""

    header_file = "include/font_phosphor.h"
    try:
        with open(header_file, 'w', encoding='utf-8') as f:
            f.write(header_content)
        print(f"成功生成 {header_file}")
    except Exception as e:
        print(f"生成头文件失败: {e}")
        return False

    # impl 文件
    impl_content = """#include "font_phosphor.h"

const font_phosphor_symbol_t font_phosphor_symbols[] = {
"""
    for k, v in icon_mapping.items():
        ch = chr(v)
        utf8 = ''.join(f'\\x{c:02x}' for c in ch.encode("utf-8"))
        impl_content += f'    {{"{k}", "{utf8}"}},\n'
    impl_content += """};

const size_t font_phosphor_symbol_count = sizeof(font_phosphor_symbols) / sizeof(font_phosphor_symbols[0]);
"""

    impl_file = "src/font_phosphor.c"
    try:
        with open(impl_file, 'w', encoding='utf-8') as f:
            f.write(impl_content)
        print(f"成功生成 {impl_file}")
        print(f"总共生成了 {len(icon_mapping)} 个 Phosphor 符号 + {len(icon_mapping)} 个 FA alias")
        return True
    except Exception as e:
        print(f"生成实现文件失败: {e}")
        return False


if __name__ == "__main__":
    sys.exit(main())
