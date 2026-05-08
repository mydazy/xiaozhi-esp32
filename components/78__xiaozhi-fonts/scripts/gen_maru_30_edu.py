#!/usr/bin/env python3
"""
gen_maru_30_edu.py — 生成 30px 教育卡辅助字库（极简，必含 IPA 音标）

字符集职责（v3 规划 · 30px 仅辅助）：
  - ASCII A-Z a-z 0-9（word.main 长词 >9 字符兜底降级）
  - Latin-1 基础变音（西欧字母）
  - 拼音四声（hanzi.top 拼音注释 / pinyin.main 兜底）
  - 🆕 IPA 音标完整（U+0250-U+02AF 全集，word.top 音标）
  - 启蒙 UI 提示字（精选 ~30 个 CJK，pinyin.top 类别等）
  - 数学/货币（与 48px 一致，避免 fallback 到 20px 突变）
  - 基础标点

设计意图：
  30px 不再承载海量 CJK（hanzi.main 已升级到 48px 真显示）。
  只保留启蒙必需的辅助字符，体积极致压缩。

体积估算（4bpp）：
  ~280 字符总量 → ~100 KB

输出：
  cbin/font_maru_30_4.bin
  cbin/mydazy/font_maru_30_4.bin     ← 项目运行时加载路径
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
CBIN_OUT = ROOT / "cbin" / "font_maru_30_4.bin"
CBIN_OUT_MIRROR = ROOT / "cbin" / "mydazy" / "font_maru_30_4.bin"


def main():
    if not TTF_MARU.exists():
        sys.exit(f"FATAL: 975MaruSC 不存在: {TTF_MARU}")
    if not TTF_QWEN.exists():
        sys.exit(f"FATAL: noto-qwen 不存在: {TTF_QWEN}")

    print("[1/4] 加载源字体 cmap...")
    qwen_cm = TTFont(str(TTF_QWEN)).getBestCmap()
    maru_cm = TTFont(str(TTF_MARU)).getBestCmap()
    print(f"  noto-qwen.ttf: {len(qwen_cm)} glyphs")
    print(f"  975MaruSC.ttf: {len(maru_cm)} glyphs")

    # ── IPA 音标（noto-qwen 提供）──
    IPA_RANGE = set(range(0x0250, 0x02B0))                # IPA 主集
    IPA_SUPRA = {0x02D0, 0x02C8, 0x02CC, 0x02D1, 0x02BC}  # 长音/重音/省略

    # ── 拼音四声 ──
    PINYIN3 = {0x01CE, 0x01D0, 0x01D2, 0x01D4, 0x01D6, 0x01D8, 0x01DA, 0x01DC,
               0x1E3F, 0x0144, 0x0148, 0x01F9, 0x01F5, 0x01F8, 0x0143, 0x0147,
               0x0101, 0x0113, 0x012B, 0x014D, 0x016B,
               0x011B, 0x011A,                                    # ě Ě（修复：e 三声漏字）
               0x00E1, 0x00E9, 0x00ED, 0x00F3, 0x00FA,
               0x00E0, 0x00E8, 0x00EC, 0x00F2, 0x00F9,
               0x00FC, 0x00DC}

    # ── 启蒙 UI 提示字（精选 CJK，pinyin.top 类别 + 通用启蒙词汇）──
    UI_HINT_CHARS = set(ord(c) for c in (
        "声母韵整体认读写拼字词句例释义类别部首笔画顺共计学教练习"
        "题答对错正反真假大小长短高低多少新旧前后左右上下里外"
    ))

    # ── 数学/货币/装饰（与 48px 一致，避免突变到 20px fallback）──
    MATH = {0x00D7, 0x00F7, 0x00B1, 0x2248, 0x2260,
            0x2264, 0x2265, 0x221A, 0x03C0, 0x221E,
            0x00B2, 0x00B3, 0x00BD, 0x00BC, 0x00BE}
    CURRENCY = {0x00A5, 0x0024, 0x20AC, 0x00A3, 0x00B0, 0x0025}
    DECORATION = {0x2022, 0x2605, 0x25C6, 0x2713, 0x2717, 0x2192}

    # ── 标点（中英）──
    PUNCT = {0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026,
             0x3001, 0x3002,
             0xFF01, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F,
             0xFF08, 0xFF09}

    # ── 主体集合（975MaruSC 提供拼音 + UI 提示字 + ASCII + 数学等）──
    maru_chars = UI_HINT_CHARS                           # 启蒙提示 CJK
    maru_chars |= set(range(0x0020, 0x007F))             # ASCII（word 兜底）
    maru_chars |= set(range(0x00A0, 0x0100))             # Latin-1
    maru_chars |= PINYIN3                                 # 拼音四声
    maru_chars |= MATH | CURRENCY | DECORATION | PUNCT
    maru_chars &= set(maru_cm.keys())

    # ── IPA 音标（noto-qwen 提供）──
    ipa_chars = (IPA_RANGE | IPA_SUPRA) & set(qwen_cm.keys())

    print(f"[2/4] 字符集汇总:")
    print(f"  ASCII + Latin1     : {len(maru_chars & set(range(0x0020, 0x0100))):>4} 字")
    print(f"  拼音四声            : {len(PINYIN3 & set(maru_cm.keys())):>4} 字")
    print(f"  IPA 音标            : {len(ipa_chars):>4} 字")
    print(f"  启蒙 UI 提示字 (CJK): {len(UI_HINT_CHARS & set(maru_cm.keys())):>4} 字")
    print(f"  数学+货币+装饰      : {len((MATH | CURRENCY | DECORATION) & set(maru_cm.keys())):>4} 字")
    print(f"  标点                : {len(PUNCT & set(maru_cm.keys())):>4} 字")
    print(f"  ─────────────────────────────")
    print(f"  合计 (去重)         : {len(maru_chars | ipa_chars):>4} 字")

    maru_str = ''.join(chr(cp) for cp in sorted(maru_chars))
    ipa_str = ''.join(chr(cp) for cp in sorted(ipa_chars))

    print(f"[3/4] 调用 lv_font_conv 生成 cbin 30px 4bpp...")
    CBIN_OUT.parent.mkdir(parents=True, exist_ok=True)
    LV_FONT_CONV_JS = ROOT / "tmp" / "lv_font_conv" / "lv_font_conv.js"
    if not LV_FONT_CONV_JS.exists():
        sys.exit(f"FATAL: lv_font_conv 未安装: {LV_FONT_CONV_JS}")
    cmd = [
        "node", str(LV_FONT_CONV_JS),
        "--bpp", "4",
        "--size", "30",
        "--format", "cbin",
        "--no-compress",
        "--no-prefilter",
        "--force-fast-kern-format",
        "-o", str(CBIN_OUT),
        "--font", str(TTF_MARU),
        "--symbols", maru_str,
        "--font", str(TTF_QWEN),
        "--symbols", ipa_str,
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
