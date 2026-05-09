#!/usr/bin/env python3
"""
gen_maru_48_edu.py — 生成 48px 英文兜底字库（v8 启蒙定版）

【v8 角色变更】48px 不再是教育卡主秀！
  v3 旧角色：教育卡主秀（含 3000 中文，~3.6 MB）→ 已被 56px 替代
  v8 新角色：仅 EN-mode 主秀长词兜底（11-12 字符英文如 "information"）

字符集职责（v8 spec · 48px 仅英文兜底）：
  - ASCII A-Z a-z 0-9 + 基础标点（英文兜底主体）
  - Latin-1 西欧基础变音（兼容外文绘本）
  - 拼音四声（极少触发，但保留以防边界）
  - Phonics 中点 · (U+00B7，自然拼读分隔)
  - ❌ 不含 CJK 中文（中文都走 56 主秀；超 4 字直接跳过激活）
  - ❌ 不含数学/货币/装饰/全角标点（这些是 v3 中文教学场景用的）

设计意图：
  v8 EN-mode：英文 ≤ 10 字符走 56 主秀；11-12 字符走 48 兜底；> 12 跳过。
  48 不需要中文——副位汉字翻译走 20px 兜底字体（font_maru_common_20_4）。

体积估算（4bpp）：
  ASCII + Latin1 + 拼音 + 基础标点 → ~30-50 KB（vs v3 的 3.6 MB）
  节省 ~3.5 MB Flash ✨

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

    print("[1/4] 加载源字体 cmap...")
    maru_cm = TTFont(str(TTF_MARU)).getBestCmap()
    print(f"  975MaruSC.ttf: {len(maru_cm)} glyphs")

    # ── 拼音四声（v8 极少触发但保留以防边界 case）──
    PINYIN3 = {0x01CE, 0x01D0, 0x01D2, 0x01D4, 0x01D6, 0x01D8, 0x01DA, 0x01DC,
               0x0101, 0x0113, 0x012B, 0x014D, 0x016B,           # ā ē ī ō ū
               0x011B, 0x011A,                                    # ě Ě
               0x00E1, 0x00E9, 0x00ED, 0x00F3, 0x00FA,           # á é í ó ú
               0x00E0, 0x00E8, 0x00EC, 0x00F2, 0x00F9,           # à è ì ò ù
               0x00FC, 0x00DC}                                    # ü Ü

    # ── Phonics 中点（U+00B7，自然拼读分隔，"in·for·ma·tion" 兜底渲染必需）──
    PHONICS = {0x00B7}

    # ── 英文标点（基础：— ' ' " " … 已被 ASCII/Latin-1 覆盖）──
    # 不含中文全角标点（这是 v8 EN 兜底场景，不会出现中文）

    # ── 主体集合（仅 ASCII + Latin-1 + 拼音 + Phonics）──
    # ❌ 不含 CJK 中文（v8 中文都走 56 主秀，超 4 字跳过激活）
    # ❌ 不含数学/货币/装饰/序号/全角标点（v3 中文教学场景，v8 不需要）
    maru_chars = set(range(0x0020, 0x007F))     # ASCII
    maru_chars |= set(range(0x00A0, 0x0100))    # Latin-1（西欧变音 ü ñ ç à 等）
    maru_chars |= PINYIN3                        # 拼音四声
    maru_chars |= PHONICS                        # · 自然拼读
    maru_chars &= set(maru_cm.keys())            # 975MaruSC 实际有的

    print(f"[2/4] 字符集汇总（v8 EN 兜底纯英文版）:")
    print(f"  ASCII A-Z a-z 0-9   : {len(maru_chars & set(range(0x0020, 0x007F))):>4} 字")
    print(f"  Latin-1 西欧变音     : {len(maru_chars & set(range(0x00A0, 0x0100))):>4} 字")
    print(f"  拼音四声 (边界用)   : {len(PINYIN3 & set(maru_cm.keys())):>4} 字")
    print(f"  Phonics 中点 ·      : {len(PHONICS & set(maru_cm.keys())):>4} 字")
    print(f"  ──────────────────────────────")
    print(f"  合计                : {len(maru_chars):>4} 字 (v3 旧版 ~4000 字 → v8 仅保留英文兜底)")

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
