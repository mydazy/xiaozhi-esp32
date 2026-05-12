#!/usr/bin/env python3
"""
gen_maru_56_edu.py — 生成 56px 教育卡主秀字库（v8 启蒙定版）

字符集职责（v8 spec · 56px 主秀）：
  - GB 2312 一级 3755 字（教育部启蒙标准字符集）
  - ASCII A-Z a-z 0-9（英文主秀 EN-mode）
  - 拼音四声（hanzi.top 拼音 / Phonics）
  - Phonics 中点 ·（U+00B7，自然拼读音节切分）
  - 中文标点（基础）
  - 主屏时钟字符（年月日 星期一二三四五六七八九十）

设计意图：
  56px 是 v8 启蒙主秀字号（PY-mode 汉字 / EN-mode 英文）。
  Bold 字重让笔画沉甸甸，配 #FFCA28 金色 OLED 黑底视觉冲击力强。

  ⭐ bpp 升级（v8.1）：从 1bpp → 2bpp
  - 1bpp 黑白二值导致 56 px 大字渲染锯齿严重（无抗锯齿）
  - 2bpp 提供 4 灰度抗锯齿，圆角 975MaruSC 字形圆润边缘恢复
  - Flash 体积约 1.5-2x（vs 1bpp），仍在 8M Flash 容量内

体积估算：
  ~3800 字符 · 2bpp + RLE 压缩 → ~2 MB（vs 旧版 1bpp 1.3 MB）

输出：
  cbin/font_maru_56_4.bin
  cbin/mydazy/font_maru_56_4.bin     ← 项目运行时加载路径（v8.1 4bpp 抗锯齿，替代旧 1bpp 56_1）
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
TTF_MARU_BOLD = ROOT / "ttf" / "975MaruSC-Bold.ttf"
# v8.1: bpp=1 → 4 抗锯齿升级，文件名同步从 _1.bin → _4.bin
CBIN_OUT = ROOT / "cbin" / "font_maru_56_4.bin"
CBIN_OUT_MIRROR = ROOT / "cbin" / "mydazy" / "font_maru_56_4.bin"


def gb2312_level1_chars():
    """GB 2312 一级字 3755 字（启蒙最权威字符集）"""
    chars = set()
    for high in range(0xB0, 0xD8):  # 一级 0xB0A1-0xD7FE
        for low in range(0xA1, 0xFF):
            try:
                ch = bytes([high, low]).decode('gb2312')
                if '一' <= ch <= '鿿':
                    chars.add(ord(ch))
            except UnicodeDecodeError:
                pass
    return chars


def main():
    if not TTF_MARU_BOLD.exists():
        sys.exit(f"FATAL: 975MaruSC-Bold 不存在: {TTF_MARU_BOLD}")

    print("[1/4] 加载源字体 cmap...")
    maru_cm = TTFont(str(TTF_MARU_BOLD)).getBestCmap()
    print(f"  975MaruSC-Bold.ttf: {len(maru_cm)} glyphs")

    # ── GB 2312 一级 3755 字（启蒙主秀全集）──
    GB_L1 = gb2312_level1_chars()

    # ── 主屏时钟（与 30px 一致，确保主秀也能渲染）──
    CLOCK_CHARS = set(ord(c) for c in "年月日时分秒星期一二三四五六七八九十")

    # ── 拼音四声（同 30px）──
    PINYIN3 = {0x01CE, 0x01D0, 0x01D2, 0x01D4, 0x01D6, 0x01D8, 0x01DA, 0x01DC,
               0x0101, 0x0113, 0x012B, 0x014D, 0x016B,
               0x011B, 0x011A,
               0x00E1, 0x00E9, 0x00ED, 0x00F3, 0x00FA,
               0x00E0, 0x00E8, 0x00EC, 0x00F2, 0x00F9,
               0x00FC, 0x00DC}

    # ── Phonics 中点（U+00B7，自然拼读分隔）──
    PHONICS = {0x00B7}

    # ── 中文标点（基础）──
    CN_PUNCT = set(ord(c) for c in "。，、；：！？""''（）《》")

    # ── 合并 + 字体覆盖过滤 ──
    chars = (GB_L1 | CLOCK_CHARS | PINYIN3 | PHONICS | CN_PUNCT)
    chars |= set(range(0x0020, 0x007F))  # ASCII
    chars &= set(maru_cm.keys())          # 仅保留字体覆盖的

    print(f"[2/4] 字符集汇总:")
    print(f"  GB 2312 一级       : {len(GB_L1 & set(maru_cm.keys())):>4} 字")
    print(f"  主屏时钟字符        : {len(CLOCK_CHARS & set(maru_cm.keys())):>4} 字")
    print(f"  ASCII              : {len(chars & set(range(0x0020, 0x007F))):>4} 字")
    print(f"  拼音四声            : {len(PINYIN3 & set(maru_cm.keys())):>4} 字")
    print(f"  Phonics + 中文标点  : {len((PHONICS | CN_PUNCT) & set(maru_cm.keys())):>4} 字")
    print(f"  ─────────────────────────────")
    print(f"  合计 (去重)         : {len(chars):>4} 字")

    chars_str = ''.join(chr(cp) for cp in sorted(chars))

    print(f"[3/4] 调用 lv_font_conv 生成 cbin 56px 4bpp（Bold + 抗锯齿 + RLE 压缩）...")
    CBIN_OUT.parent.mkdir(parents=True, exist_ok=True)
    LV_FONT_CONV_JS = ROOT / "tmp" / "lv_font_conv" / "lv_font_conv.js"
    if not LV_FONT_CONV_JS.exists():
        sys.exit(f"FATAL: lv_font_conv 未安装: {LV_FONT_CONV_JS}")
    cmd = [
        "node", str(LV_FONT_CONV_JS),
        "--bpp", "4",                          # v8.1: 1 → 4，16 灰度抗锯齿
        "--size", "56",
        "--format", "cbin",
        "--force-fast-kern-format",
        # RLE 压缩 · 体积 5.0MB → 2.2MB（节省 ~3MB · 防 assets 分区 8MB 溢出）
        # 依赖：sdkconfig 必须 CONFIG_LV_USE_FONT_COMPRESSED=y（否则 bitmap 全空白）
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
