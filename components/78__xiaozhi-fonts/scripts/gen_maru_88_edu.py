#!/usr/bin/env python3
"""
gen_maru_88_edu.py — 88px 教育卡超大主秀字库（英文 + 数学 + 时钟数字）

字符集职责（v8 启蒙超大档）：
  - ASCII 数字 0-9（时钟时间，原 88 用途，向后兼容）
  - 冒号 :（时钟时间分隔）
  - 大写字母 A-Z（26 字母教学超大版）
  - 小写字母 a-z（字母教学 + 拼音声母卡）
  - 加减乘除等号 + - × ÷ = ± . ()（数学算式超大版）
  - 共 ~70 字符

设计意图（v8 ⭐ 主动学习场景）：
  88 px 是 56 主秀的"超大版"，仅在主动学习场景使用：
    - "我要学字母"序列 → "Aa" 88 px 视觉冲击翻倍
    - "晨读"声母韵母 → "b" 88 px 占屏 37% 启蒙感拉满
    - "教我算 3+5" → "3+5=8" 88 px 计算感强
  被动触发（故事/对话）继续用 56 px，不抢戏。

  ❌ 不含中文 — 中文 3000 字 88 px 1bpp 约 2.3 MB，性价比太低
     单字识字超大 → 用 56 已够（"猫" 56 px 占屏 23% 已大气）

体积估算（4bpp）：
  ~70 字符 × ~3.5 KB/字 ≈ 50-150 KB

字符宽度参考（88 px 4bpp）：
  - 窄字符 i, l, j: ~22 px
  - 中等字符 a-z 大部分: ~40 px
  - 宽字符 m, w: ~60 px
  - 大写比小写宽 10-15%
  - 数学符号: ~35-40 px
  - 平均: ~40 px → 280 安全宽 = 7 字符

输出：
  cbin/font_maru_88_4.bin
  cbin/mydazy/font_maru_88_4.bin     ← 项目运行时加载路径
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
TTF_MARU_BOLD = ROOT / "ttf" / "975MaruSC-Bold.ttf"
CBIN_OUT = ROOT / "cbin" / "font_maru_88_4.bin"
CBIN_OUT_MIRROR = ROOT / "cbin" / "mydazy" / "font_maru_88_4.bin"


def main():
    if not TTF_MARU_BOLD.exists():
        sys.exit(f"FATAL: 975MaruSC-Bold 不存在: {TTF_MARU_BOLD}")

    print("[1/4] 加载源字体 cmap...")
    maru_cm = TTFont(str(TTF_MARU_BOLD)).getBestCmap()
    print(f"  975MaruSC-Bold.ttf: {len(maru_cm)} glyphs")

    # ── 88 px 字符集（v8 超大主秀档）──
    chars = set()
    chars |= set(range(0x0030, 0x003A))     # 0-9
    chars |= set(range(0x0041, 0x005B))     # A-Z
    chars |= set(range(0x0061, 0x007B))     # a-z
    chars |= {0x0020, 0x003A, 0x002E, 0x002B, 0x002D, 0x003D,    # space : . + - =
              0x0028, 0x0029, 0x002F, 0x002A,                      # ( ) / *
              0x00D7, 0x00F7, 0x00B1}                              # × ÷ ±
    chars &= set(maru_cm.keys())

    print(f"[2/4] 字符集汇总（88 px 主动学习超大档）:")
    print(f"  数字 0-9          : {len(chars & set(range(0x0030, 0x003A))):>4} 字符")
    print(f"  大写字母 A-Z       : {len(chars & set(range(0x0041, 0x005B))):>4} 字符")
    print(f"  小写字母 a-z       : {len(chars & set(range(0x0061, 0x007B))):>4} 字符")
    print(f"  数学符号 + - × ÷ = : {len(chars & {0x002B, 0x002D, 0x003D, 0x00D7, 0x00F7, 0x00B1, 0x002F, 0x002A}):>4} 字符")
    print(f"  其他标点 . : ( )   : {len(chars & {0x002E, 0x003A, 0x0028, 0x0029}):>4} 字符")
    print(f"  ─────────────────────────────")
    print(f"  合计                : {len(chars):>4} 字符")

    chars_str = ''.join(chr(cp) for cp in sorted(chars))

    print(f"[3/4] 调用 lv_font_conv 生成 cbin 88px 4bpp（Bold）...")
    CBIN_OUT.parent.mkdir(parents=True, exist_ok=True)
    LV_FONT_CONV_JS = ROOT / "tmp" / "lv_font_conv" / "lv_font_conv.js"
    if not LV_FONT_CONV_JS.exists():
        sys.exit(f"FATAL: lv_font_conv 未安装: {LV_FONT_CONV_JS}")
    cmd = [
        "node", str(LV_FONT_CONV_JS),
        "--bpp", "4",
        "--size", "88",
        "--format", "cbin",
        "--no-compress",
        "--no-prefilter",
        "--force-fast-kern-format",
        "-o", str(CBIN_OUT),
        "--font", str(TTF_MARU_BOLD),
        "--symbols", chars_str,
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("STDOUT:", result.stdout[-2000:])
        print("STDERR:", result.stderr[-2000:])
        sys.exit(f"lv_font_conv 失败 (exit {result.returncode})")
    print(f"      stdout: {result.stdout[-300:].strip()}")

    out_size = CBIN_OUT.stat().st_size
    print(f"[4/4] 生成成功: {CBIN_OUT}")
    print(f"      大小: {out_size:,} bytes ({out_size / 1024:.1f} KB)")

    CBIN_OUT_MIRROR.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(CBIN_OUT, CBIN_OUT_MIRROR)
    print(f"      同步: {CBIN_OUT_MIRROR}")


if __name__ == "__main__":
    main()
