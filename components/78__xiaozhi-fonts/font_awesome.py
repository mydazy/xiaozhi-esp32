import os
import sys
import argparse

# ============================================================================
# Font Awesome 字体生成器（emoji 子集 · 2026-04-30 重构）
#
# 历史：原本管 134 个图标（21 emoji + 113 UI 图标）。
# 重构后：UI 图标全部迁移到 Phosphor Fill / Regular（见 font_phosphor.py），
#         FA 字体仅保留 21 个 emoji，专用作 LVGL fallback 字体。
# 渲染链：业务 set_text → font_phosphor_X_Y（主）→ fallback → font_awesome_X_Y（emoji）
#
# 重生成需要本地放 OTF 字体到 ./ttf/：
#   - fa-regular-400.otf  （FA Free，14/16/20 字号）
#   - fa-light-300.otf    （FA Pro，30 字号）
# 没有 OTF 时 lv_font_conv 会报错——保留现有 .c 不重生成是允许的退路（字形冗余但视觉不变）。
# ============================================================================

icon_mapping = {
    # emojis（21 个 · LCD/OLED 情绪表情专用）
    "neutral": 0xf5a4,  # 0xf5a4 [blank]  0xf11a 😐
    "happy": 0xf118,    # 😊
    "laughing": 0xf59b, # 😆
    "funny": 0xf588,    # 😂
    "sad": 0xe384,      # 😔
    "angry": 0xf556,    # 😡
    "crying": 0xf5b3,   # 😭
    "loving": 0xf584,   # 😍
    "embarrassed": 0xf579, # 😳
    "surprised": 0xe36b,   # 😲
    "shocked": 0xe375,     # 😱
    "thinking": 0xe39b,    # 🤔
    "winking": 0xf4da,     # 😉
    "cool": 0xe398,        # 😎
    "relaxed": 0xe392,     # 😌
    "delicious": 0xe372,   # 😋
    "kissy": 0xf598,       # 😗
    "confident": 0xe409,   # 😏
    "sleepy": 0xe38d,      # 😴
    "silly": 0xe3a4,       # 😜
    "confused": 0xe36d,    # 😕
}

def parse_arguments():
    parser = argparse.ArgumentParser(description='Font Awesome converter utility')
    parser.add_argument('type', choices=['lvgl', 'dump', 'generate'], help='Output type: lvgl, dump, or generate header files')
    parser.add_argument('--font-size', type=int, default=14, help='Font size (default: 14)')
    parser.add_argument('--bpp', type=int, default=4, help='Bits per pixel (default: 4)')
    return parser.parse_args()

def get_font_file(font_size):
    # 30px 用 Light（细线条），其他字号用 Regular
    if font_size == 30:
        return "./ttf/fa-light-300.otf"
    return "./ttf/fa-regular-400.otf"

def main():
    args = parse_arguments()

    if args.type == "generate":
        return 0 if generate_symbols_header_file() else 1

    symbols = list(icon_mapping.values())
    flags = "--no-compress --no-prefilter --force-fast-kern-format"
    font = get_font_file(args.font_size)

    if not os.path.exists(font):
        print(f"⚠ 字体文件缺失: {font}")
        print(f"  请下载 FA Free Regular / FA Pro Light 到 ./ttf/ 后再跑")
        print(f"  跳过本次生成（保留现有 .c）")
        return 0

    symbols_str = ",".join(map(hex, symbols))

    if args.type == "lvgl":
        output = f"src/font_awesome_{args.font_size}_{args.bpp}.c"
        cmd = f"lv_font_conv {flags} --font {font} --format lvgl --lv-include lvgl.h --bpp {args.bpp} -o {output} --size {args.font_size} -r {symbols_str}"
    else:  # dump
        output = f"./build/font_awesome_dump"
        import shutil
        shutil.rmtree(output, ignore_errors=True)
        os.makedirs(output, exist_ok=True)
        cmd = f"lv_font_conv {flags} --font {font} --format dump --bpp {args.bpp} -o {output} --size {args.font_size} -r {symbols_str}"

    print("Total symbols:", len(symbols))
    print("Generating", output)

    ret = os.system(cmd)
    if ret != 0:
        print(f"命令执行失败，返回码：{ret}")
        return ret
    print("命令执行成功")
    return 0

def generate_symbols_header_file():
    """根据当前的 icon_mapping 重新生成 font_awesome.h 文件
    （UI 图标的 FONT_AWESOME_* 宏由 font_phosphor.h 通过 alias 提供 · 见末尾 #include）
    """

    # 合并所有符号
    symbols = {}
    for k, v in icon_mapping.items():
        symbols[k] = v

    # 生成头文件内容
    header_content = """#ifndef FONT_AWESOME_H
#define FONT_AWESOME_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// 符号结构体
typedef struct {
    const char* name;
    const char* utf8_string;
} font_awesome_symbol_t;

"""

    # 为每个符号生成UTF-8编码的宏定义
    for k, v in symbols.items():
        ch = chr(v)
        utf8 = ''.join(f'\\x{c:02x}' for c in ch.encode("utf-8"))
        header_content += f'#define FONT_AWESOME_{k.upper()} "{utf8}"\n'

    # 添加符号数据表声明
    header_content += """
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
"""

    # 写入头文件
    header_file = "include/font_awesome.h"
    try:
        with open(header_file, 'w', encoding='utf-8') as f:
            f.write(header_content)
        print(f"成功生成 {header_file}")
    except Exception as e:
        print(f"生成头文件失败: {e}")
        return False

    # 生成实现文件
    impl_content = """#include "font_awesome.h"

// 符号数据表实现（21 个 emoji · 用于 oled_display.cc font_awesome_get_utf8）
const font_awesome_symbol_t font_awesome_symbols[] = {
"""

    # 为每个符号生成数据表条目
    for k, v in symbols.items():
        ch = chr(v)
        utf8 = ''.join(f'\\x{c:02x}' for c in ch.encode("utf-8"))
        impl_content += f'    {{"{k}", "{utf8}"}},\n'

    impl_content += """};

// 符号总数
const size_t font_awesome_symbol_count = sizeof(font_awesome_symbols) / sizeof(font_awesome_symbols[0]);
"""

    # 写入实现文件
    impl_file = "src/font_awesome.c"
    try:
        with open(impl_file, 'w', encoding='utf-8') as f:
            f.write(impl_content)
        print(f"成功生成 {impl_file}")
        print(f"总共生成了 {len(symbols)} 个符号")
        return True
    except Exception as e:
        print(f"生成实现文件失败: {e}")
        return False

if __name__ == "__main__":
    sys.exit(main())
