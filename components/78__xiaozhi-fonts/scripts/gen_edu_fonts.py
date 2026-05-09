#!/usr/bin/env python3
"""
教育卡字体生成器（v8 启蒙版）

用 lv_font_conv 从 975MaruSC-Regular.ttf 生成三档字体：
  - 56 px Bold 全集（中 + 英 + 拼音 + 标点）→ 主秀
  - 48 px Bold 仅 ASCII + Phonics 中点 → EN 兜底
  - 30 px SemiBold ASCII + 拼音 + Phonics → 顶部

字符集来源：
  - ASCII 0x20-0x7E
  - 拼音声调字符（ā á ǎ à 等 24 个）
  - Phonics 中点 0x00B7
  - 中文 3000 字（教育部《通用规范汉字表》一级 + LLM tokenizer 高频字 union）
  - 中文标点 14 个

用法：
    cd components/78__xiaozhi-fonts
    python3 scripts/gen_edu_fonts.py
    python3 scripts/gen_edu_fonts.py --bpp1   # 主秀用 1bpp 压缩（节省 4× Flash）
    python3 scripts/gen_edu_fonts.py --skip-cn # 仅生成英文 / 拼音字体（开发期跳过中文）

依赖：
    npm install -g lv_font_conv@1.5.2
    pip install transformers tqdm fonttools
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

# ═══════════════════════════════════════════════════════════════════════
# 字符集定义（v8 spec）
# ═══════════════════════════════════════════════════════════════════════

# ASCII 基础（包含 Phonics 中点 ·）
ASCII_RANGE = "0x20-0x7E"
PHONICS_MID = "0x00B7"  # ·

# 拼音声调字符（24 个）
PINYIN_TONE_CHARS = [
    "ā", "á", "ǎ", "à",
    "ē", "é", "ě", "è",
    "ī", "í", "ǐ", "ì",
    "ō", "ó", "ǒ", "ò",
    "ū", "ú", "ǔ", "ù",
    "ǖ", "ǘ", "ǚ", "ǜ",
    "ü",  # 25 个含 ü
]

# 中文标点（14 个）
CN_PUNCTUATION = list("。，、；：！？""''（）《》")

# 启蒙词高频字（确保覆盖，约 200 字）
EDU_HIGH_FREQ_CHARS = list(
    "猫狗鸟鱼虎象兔鸡鸭鹅马牛羊猴熊鹿狼蛇龙龟蚊蜂蝶蜗虫蛙鼠鱼"
    "花草树叶果苗芽根茎枝干竹梅菊兰荷桃李梨杏桂松柏"
    "头脸眼耳鼻口齿舌唇手足指肩腰背胸腹腿膝肘臂腕"
    "爸妈爷奶兄姐弟妹叔伯姨舅爹娘姑家"
    "米饭面包菜汤水奶肉蛋果茶糖盐酱醋粥虾蟹"
    "红黄蓝绿黑白紫粉橙灰金银"
    "吃喝睡跑跳走站坐玩看听说读写画唱笑哭"
    "山水日月星云风雨雪雷电火土石木金"
    "大小多少高矮胖瘦冷热新旧远近快慢"
    "天地人手口耳目心子女我你他她它"
    "今明昨年月日时分秒早晚午"
    "上下左右前后里外东南西北中"
    "一二三四五六七八九十百千万"
    "好坏对错真假美丑是否"
)

# ═══════════════════════════════════════════════════════════════════════
# 路径
# ═══════════════════════════════════════════════════════════════════════

SCRIPT_DIR = Path(__file__).parent.resolve()
COMPONENT_DIR = SCRIPT_DIR.parent  # components/78__xiaozhi-fonts/
TTF_DIR = COMPONENT_DIR / "ttf"
OUTPUT_DIR = COMPONENT_DIR / "cbin"
BUILD_DIR = COMPONENT_DIR / "build"

DEFAULT_TTF = TTF_DIR / "975MaruSC-Regular.ttf"


# ═══════════════════════════════════════════════════════════════════════
# 字符集生成
# ═══════════════════════════════════════════════════════════════════════

def get_pinyin_unicode_ranges():
    """拼音声调字符的 Unicode 码点（用于 lv_font_conv --range）"""
    points = sorted({ord(c) for c in PINYIN_TONE_CHARS})
    # 合并连续段：[ā-à] [ē-è] etc.
    ranges = []
    start = points[0]
    prev = start
    for p in points[1:]:
        if p == prev + 1:
            prev = p
        else:
            ranges.append(f"0x{start:04X}-0x{prev:04X}" if start != prev else f"0x{start:04X}")
            start = prev = p
    ranges.append(f"0x{start:04X}-0x{prev:04X}" if start != prev else f"0x{start:04X}")
    return ranges


def load_chinese_charset(extra_file: Path = None):
    """从 LLM tokenizer 抓取（复用 gen_ttf.py 输出的 build/chars.txt）+ 启蒙补充字"""
    chars = set(EDU_HIGH_FREQ_CHARS) | set(CN_PUNCTUATION)

    chars_txt = BUILD_DIR / "chars.txt"
    if chars_txt.exists():
        with open(chars_txt, encoding="utf-8") as f:
            for line in f:
                ch = line.strip()
                if len(ch) == 1 and "一" <= ch <= "鿿":
                    chars.add(ch)
        print(f"[+] 从 {chars_txt} 加载 LLM 高频字 → 累计 {len(chars)} 字")
    else:
        print(f"[!] {chars_txt} 不存在，仅使用启蒙补充字（{len(EDU_HIGH_FREQ_CHARS)} 字）")
        print(f"    建议先跑 scripts/gen_ttf.py 提取 LLM 词表")

    # 用户可补充自定义字符
    if extra_file and extra_file.exists():
        with open(extra_file, encoding="utf-8") as f:
            for ch in f.read():
                if len(ch) == 1 and "一" <= ch <= "鿿":
                    chars.add(ch)
        print(f"[+] 从 {extra_file} 加载补充字 → 累计 {len(chars)} 字")

    return chars


def write_unicode_list(chars: set, path: Path):
    """写出 unicode 列表给 lv_font_conv --symbols 用"""
    sorted_chars = sorted(chars, key=lambda c: ord(c))
    with open(path, "w", encoding="utf-8") as f:
        f.write("".join(sorted_chars))
    print(f"[+] 字符表写入 {path}（{len(sorted_chars)} 字）")
    return path


# ═══════════════════════════════════════════════════════════════════════
# 字体生成（调用 lv_font_conv）
# ═══════════════════════════════════════════════════════════════════════

def run_lv_font_conv(ttf_path: Path, size: int, bpp: int,
                     output_bin: Path, ranges: list = None,
                     symbols_file: Path = None, font_format: str = "bin"):
    """调用 lv_font_conv 生成 LVGL 字体"""
    cmd = [
        "lv_font_conv",
        "--font", str(ttf_path),
        "--size", str(size),
        "--bpp", str(bpp),
        "--format", font_format,
        "--no-compress" if bpp == 4 else "--lv-include", "lvgl/lvgl.h",
        "-o", str(output_bin),
    ]

    if ranges:
        for r in ranges:
            cmd.extend(["--range", r])
    if symbols_file:
        cmd.extend(["--symbols-file", str(symbols_file)])

    print(f"[>] 运行: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"[!] lv_font_conv 失败:\n{result.stderr}")
        return False

    size_kb = output_bin.stat().st_size / 1024
    print(f"[✓] {output_bin.name} 生成 ({size_kb:.1f} KB)")
    return True


def gen_56_main(ttf_path: Path, all_chars: set, bpp: int):
    """主秀字体：中 + 英 + 拼音 + Phonics + 标点"""
    print("\n═══ 生成 56 px Bold（主秀）═══")
    symbols = BUILD_DIR / "symbols_56.txt"
    write_unicode_list(all_chars, symbols)

    output = OUTPUT_DIR / f"font_maru_56_{bpp}.bin"
    return run_lv_font_conv(
        ttf_path, size=56, bpp=bpp,
        output_bin=output,
        ranges=[ASCII_RANGE, PHONICS_MID] + get_pinyin_unicode_ranges(),
        symbols_file=symbols,
    )


def gen_48_eng(ttf_path: Path):
    """EN 兜底字体：仅 ASCII + Phonics 中点"""
    print("\n═══ 生成 48 px Bold（EN 兜底）═══")
    output = OUTPUT_DIR / "font_maru_eng_48_4.bin"
    return run_lv_font_conv(
        ttf_path, size=48, bpp=4,
        output_bin=output,
        ranges=[ASCII_RANGE, PHONICS_MID],
    )


def gen_30_top(ttf_path: Path):
    """顶部字体：ASCII + 拼音 + Phonics"""
    print("\n═══ 生成 30 px SemiBold（顶部）═══")
    output = OUTPUT_DIR / "font_maru_30_4.bin"
    return run_lv_font_conv(
        ttf_path, size=30, bpp=4,
        output_bin=output,
        ranges=[ASCII_RANGE, PHONICS_MID] + get_pinyin_unicode_ranges(),
    )


# ═══════════════════════════════════════════════════════════════════════
# 主流程
# ═══════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(description="教育卡字体生成器（v8）")
    parser.add_argument("--ttf", type=Path, default=DEFAULT_TTF,
                        help="源字体文件（默认 975MaruSC-Regular.ttf）")
    parser.add_argument("--output", type=Path, default=OUTPUT_DIR,
                        help="输出目录（默认 cbin/）")
    parser.add_argument("--bpp1", action="store_true",
                        help="主秀用 1bpp 压缩（节省 4× Flash，启蒙产品视觉影响小）")
    parser.add_argument("--skip-cn", action="store_true",
                        help="仅生成英文/拼音（开发期跳过中文，加速调试）")
    parser.add_argument("--extra-chars", type=Path, default=None,
                        help="额外中文字符文件（每行 1 字或连续字符串）")
    args = parser.parse_args()

    if not args.ttf.exists():
        print(f"[!] 字体文件不存在: {args.ttf}")
        print(f"    请确认 components/78__xiaozhi-fonts/ttf/975MaruSC-Regular.ttf 已就位")
        sys.exit(1)

    args.output.mkdir(parents=True, exist_ok=True)
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    print(f"[i] 源字体: {args.ttf}")
    print(f"[i] 输出: {args.output}")
    print(f"[i] 主秀 bpp: {1 if args.bpp1 else 4}")
    print(f"[i] 跳过中文: {args.skip_cn}")

    # 1. 准备字符集
    if args.skip_cn:
        all_chars = set()  # 仅 ASCII + 拼音由 --range 提供
    else:
        all_chars = load_chinese_charset(args.extra_chars)

    # 2. 生成三档字体
    main_bpp = 1 if args.bpp1 else 4

    success = True
    if not args.skip_cn:
        success &= gen_56_main(args.ttf, all_chars, bpp=main_bpp)
    success &= gen_48_eng(args.ttf)
    success &= gen_30_top(args.ttf)

    if success:
        print("\n[✓] 全部字体生成完成！")
        print("\n下一步：")
        print("  1. 把生成的 .bin 文件加入 components/78__xiaozhi-fonts/include/cbin_font.h")
        print("  2. 在 UiDisplay::EnsureDisplayFonts() 加载新字体")
        print("  3. 修改 ShowEduCard() 使用新字号（详见 docs/education-card-lvgl-upgrade-plan.md）")
    else:
        print("\n[!] 部分字体生成失败，检查上方报错")
        sys.exit(1)


if __name__ == "__main__":
    main()
