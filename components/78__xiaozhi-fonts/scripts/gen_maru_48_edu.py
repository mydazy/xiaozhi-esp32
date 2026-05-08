#!/usr/bin/env python3
"""
gen_maru_48_edu.py — 生成 48px 教育卡主体字库（含小学常用字 2500）

字符集职责（v3 规划 · 48px 主视觉真大字）：
  - ASCII A-Z a-z 0-9 + Latin-1（西欧基础变音）+ 拼音四声
  - 中英标点（含全角）
  - 🆕 数学符号（× ÷ ± ≈ ≠ ≤ ≥ √ π ∞ ² ³ ½ ¼ ¾）
  - 🆕 货币单位（¥ $ € £ ° %）
  - 🆕 装饰符号（• ★ ◆ ✓ ✗ →）
  - 🆕 小学常用字（默认 GB2312 一级 3755；可通过 scripts/charset_primary.txt
                  覆盖为《现代汉语常用字表》2500 字以剔除生僻字）

设计意图：
  P30 屏小，48px 是孩子的视觉焦点。3-10 岁启蒙阶段必须看清大字。
  hanzi.main 真正以 48px 渲染，不再 fallback 到 30px。
  word.main 短词 48px / pinyin.main 始终 48px。

体积估算（4bpp）：
  GB2312 一级全集 3755  → ~4.2 MB
  小学常用 2500（推荐） → ~2.8 MB
  ASCII + 拼音 + 标点 + 数学/货币/装饰 → ~80 KB

输出：
  cbin/font_maru_48_4.bin
  cbin/mydazy/font_maru_48_4.bin     ← 项目运行时加载路径
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
TTF_MARU = ROOT / "ttf" / "975MaruSC-Regular.ttf"
TTF_QWEN = ROOT / "ttf" / "noto-qwen.ttf"
CBIN_OUT = ROOT / "cbin" / "font_maru_48_4.bin"
CBIN_OUT_MIRROR = ROOT / "cbin" / "mydazy" / "font_maru_48_4.bin"

# 自定义小学常用字字符集（每行可放任意字 · 留空行/注释 # 开头会被忽略）
# 推荐字符集来源：
#   - 国家语委《现代汉语常用字表》2500 字（1988，至今最权威）
#   - 人教版义务教育语文课程标准 1600 识字 + 800 写字 + 100 补充
PRIMARY_CHARSET_FILE = Path(__file__).resolve().parent / "charset_primary.txt"


def load_primary_chars():
    """读取小学常用字字符集，缺失时回退 GB2312 一级全集"""
    if PRIMARY_CHARSET_FILE.exists():
        text = PRIMARY_CHARSET_FILE.read_text(encoding='utf-8')
        chars = set()
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            chars.update(ord(c) for c in line if '一' <= c <= '鿿')
        print(f"  ✅ 自定义字符集 {PRIMARY_CHARSET_FILE.name}: {len(chars)} 字")
        return chars

    # 回退：GB2312 一级全集（3755 字，按 GB2312 区位排序）
    print(f"  ⚠️  {PRIMARY_CHARSET_FILE.name} 不存在，回退 GB2312 一级全集 3755 字")
    print(f"      建议提供该文件以剔除生僻字（推荐 2500 字 → 体积省 ~1.4MB）")
    chars = set()
    for high in range(0xB0, 0xD8):     # 区号 16-55（GB2312 一级）
        for low in range(0xA1, 0xFF):
            try:
                ch = bytes([high, low]).decode('gb2312')
                chars.add(ord(ch))
            except UnicodeDecodeError:
                continue
    return chars


def main():
    if not TTF_MARU.exists():
        sys.exit(f"FATAL: 975MaruSC 不存在: {TTF_MARU}")
    if not TTF_QWEN.exists():
        sys.exit(f"FATAL: noto-qwen 不存在: {TTF_QWEN}")

    print("[1/4] 加载源字体 cmap + 小学常用字...")
    qwen_cm = TTFont(str(TTF_QWEN)).getBestCmap()
    maru_cm = TTFont(str(TTF_MARU)).getBestCmap()
    primary_chars = load_primary_chars()

    # ── 拼音四声（含 ǖǘǚǜ 等）──
    PINYIN3 = {0x01CE, 0x01D0, 0x01D2, 0x01D4, 0x01D6, 0x01D8, 0x01DA, 0x01DC,
               0x1E3F, 0x0144, 0x0148, 0x01F9, 0x01F5, 0x01F8, 0x0143, 0x0147,
               0x0101, 0x0113, 0x012B, 0x014D, 0x016B,           # ā ē ī ō ū
               0x00E1, 0x00E9, 0x00ED, 0x00F3, 0x00FA,           # á é í ó ú
               0x00E0, 0x00E8, 0x00EC, 0x00F2, 0x00F9,           # à è ì ò ù
               0x00FC, 0x00DC}                                    # ü Ü

    # ── 数学/货币/装饰（启蒙小学算术 + 例题标记）──
    MATH = {0x00D7, 0x00F7, 0x00B1, 0x2248, 0x2260,    # × ÷ ± ≈ ≠
            0x2264, 0x2265, 0x221A, 0x03C0, 0x221E,    # ≤ ≥ √ π ∞
            0x00B2, 0x00B3, 0x00BD, 0x00BC, 0x00BE}    # ² ³ ½ ¼ ¾
    CURRENCY = {0x00A5, 0x0024, 0x20AC, 0x00A3, 0x00B0, 0x0025}  # ¥ $ € £ ° %
    DECORATION = {0x2022, 0x2605, 0x25C6, 0x2713, 0x2717, 0x2192}  # • ★ ◆ ✓ ✗ →

    # ── 标点（中英全角）──
    PUNCT = {0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026,    # — ' ' " " …
             0x3001, 0x3002,                                      # 、 。
             0xFF01, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F,     # ！ ， ． ： ； ？
             0xFF08, 0xFF09}                                      # （ ）

    # ── 主体集合（975MaruSC 提供）──
    maru_chars = primary_chars                          # 小学常用字（CJK）
    maru_chars |= set(range(0x0020, 0x007F))            # ASCII
    maru_chars |= set(range(0x00A0, 0x0100))            # Latin-1（含 ü ñ ç à 等）
    maru_chars |= PINYIN3                                # 拼音四声
    maru_chars |= MATH | CURRENCY | DECORATION | PUNCT  # 教学辅助符号
    maru_chars &= set(maru_cm.keys())                    # 975MaruSC 实际有的

    print(f"[2/4] 字符集汇总:")
    print(f"  CJK 小学常用字 : {len(primary_chars & set(maru_cm.keys())):>4} 字")
    print(f"  ASCII + Latin1 : {len(maru_chars & (set(range(0x0020, 0x0100)))):>4} 字")
    print(f"  拼音四声        : {len(PINYIN3 & set(maru_cm.keys())):>4} 字")
    print(f"  数学+货币+装饰  : {len((MATH | CURRENCY | DECORATION) & set(maru_cm.keys())):>4} 字")
    print(f"  标点            : {len(PUNCT & set(maru_cm.keys())):>4} 字")
    print(f"  ─────────────────────────────")
    print(f"  975MaruSC 合计 : {len(maru_chars):>4} 字")

    maru_str = ''.join(chr(cp) for cp in sorted(maru_chars))

    print(f"[3/4] 调用 lv_font_conv 生成 cbin 48px 4bpp...")
    CBIN_OUT.parent.mkdir(parents=True, exist_ok=True)
    LV_FONT_CONV_JS = ROOT / "tmp" / "lv_font_conv" / "lv_font_conv.js"
    if not LV_FONT_CONV_JS.exists():
        sys.exit(f"FATAL: lv_font_conv 未安装: {LV_FONT_CONV_JS}")
    cmd = [
        "node", str(LV_FONT_CONV_JS),
        "--bpp", "4",
        "--size", "48",
        "--format", "cbin",
        "--no-compress",
        "--no-prefilter",
        "--force-fast-kern-format",
        "-o", str(CBIN_OUT),
        "--font", str(TTF_MARU),
        "--symbols", maru_str,
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("STDOUT:", result.stdout[-2000:])
        print("STDERR:", result.stderr[-2000:])
        sys.exit(f"lv_font_conv 失败 (exit {result.returncode})")
    print(f"      stdout: {result.stdout[-300:].strip()}")

    out_size = CBIN_OUT.stat().st_size
    print(f"[4/4] 生成成功: {CBIN_OUT}")
    print(f"      大小: {out_size:,} bytes ({out_size / 1024 / 1024:.2f} MB)")

    CBIN_OUT_MIRROR.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(CBIN_OUT, CBIN_OUT_MIRROR)
    print(f"      同步: {CBIN_OUT_MIRROR}")


if __name__ == "__main__":
    main()
