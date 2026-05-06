#!/usr/bin/env python3
"""
gen_maru_edu.py — 生成 975MaruSC 圆体大字符集 cbin（含 LLM vocab + 拼音 + IPA）

字符集策略：
  - 主体 (975MaruSC-Regular.ttf): noto-qwen 已 subset 的 LLM vocab 字符集 (~16000)
    去掉 IPA 区，加上拼音四声 ǎ ǐ ǒ ǔ ǖ ǘ ǚ ǜ ḿ ń ň ǹ
  - IPA  (noto-qwen.ttf): U+0250-U+02AF 全集 + IPA 长音/重音 ː ˈ ˌ ˑ
  - 主字体仅 3 字 IPA，全部由 noto-qwen 接管以保证完整覆盖

输出：
  cbin/font_maru_common_20_4.bin  ← 覆盖当前 1008 字版本
  src/font_maru_common_20_4.c     ← 同时更新链入 Flash 的小集（仅 ASCII+Latin1+核心 600 汉字）
"""

import os
import subprocess
import sys
from pathlib import Path
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent  # components/78__xiaozhi-fonts/
TTF_MARU = ROOT / "ttf" / "975MaruSC-Regular.ttf"
TTF_QWEN = ROOT / "ttf" / "noto-qwen.ttf"
CBIN_OUT = ROOT / "cbin" / "font_maru_common_20_4.bin"
CBIN_OUT_MIRROR = ROOT / "cbin" / "mydazy" / "font_maru_common_20_4.bin"

# ----- 字符集合并 -----

def main():
    if not TTF_MARU.exists():
        sys.exit(f"FATAL: 975MaruSC 不存在: {TTF_MARU}")
    if not TTF_QWEN.exists():
        sys.exit(f"FATAL: noto-qwen 不存在: {TTF_QWEN}")

    print(f"[1/4] 加载源字体 cmap...")
    qwen_cm = TTFont(str(TTF_QWEN)).getBestCmap()
    maru_cm = TTFont(str(TTF_MARU)).getBestCmap()
    print(f"  noto-qwen.ttf: {len(qwen_cm)} glyphs")
    print(f"  975MaruSC.ttf: {len(maru_cm)} glyphs")

    # IPA 区段 (U+0250-U+02AF) + 长音/重音 + 中线 + 部分扩展拉丁 IPA
    IPA_RANGE = set(range(0x0250, 0x02B0))
    IPA_SUPRA = {0x02D0, 0x02C8, 0x02CC, 0x02D1, 0x02BC}  # ː ˈ ˌ ˑ ʼ
    PINYIN3 = {0x01CE, 0x01D0, 0x01D2, 0x01D4, 0x01D6, 0x01D8, 0x01DA, 0x01DC,
               0x1E3F, 0x0144, 0x0148, 0x01F9, 0x01F5, 0x01F8, 0x0143, 0x0147}

    # 主体字符集（975MaruSC 提供）：noto-qwen 已有字符集 - IPA + 拼音补充
    maru_chars = set(qwen_cm.keys()) - IPA_RANGE - IPA_SUPRA
    maru_chars |= PINYIN3
    # 强制保证基础 ASCII + Latin-1 全集
    maru_chars |= set(range(0x0020, 0x007F))
    maru_chars |= set(range(0x00A0, 0x0100))
    # 保证全角符号、标点
    maru_chars |= set(range(0x2000, 0x206F))   # 通用标点
    maru_chars |= set(range(0x3000, 0x303F))   # CJK 标点
    maru_chars |= set(range(0xFF00, 0xFFF0))   # 全角符号

    # 过滤掉 975MaruSC 缺失的（避免 lv_font_conv 报错）
    maru_chars &= set(maru_cm.keys())

    # IPA 字符集（noto-qwen 提供）
    ipa_chars = (IPA_RANGE | IPA_SUPRA) & set(qwen_cm.keys())

    print(f"[2/4] 字符集划分:")
    print(f"  975MaruSC 提供: {len(maru_chars)} 字")
    print(f"  noto-qwen IPA: {len(ipa_chars)} 字")
    print(f"  合计 (去重): {len(maru_chars | ipa_chars)} 字")

    # 验证关键字符
    pinyin_check = sorted(PINYIN3 & set(maru_cm.keys()))
    ipa_keys = [0x0259, 0x0283, 0x028C, 0x0254, 0x026A, 0x028A, 0x014B, 0x03B8,
                0x00F0, 0x00E6, 0x025B, 0x0292, 0x02A4, 0x02A7, 0x02D0]
    ipa_avail = sum(1 for cp in ipa_keys if cp in (maru_chars | ipa_chars))
    print(f"  拼音三声补充: {len(pinyin_check)}/16")
    print(f"  IPA 关键字符: {ipa_avail}/{len(ipa_keys)}")

    # ----- 写字符到临时文件（lv_font_conv --symbols 通过 Python list 传递） -----
    maru_str = ''.join(chr(cp) for cp in sorted(maru_chars))
    ipa_str = ''.join(chr(cp) for cp in sorted(ipa_chars))

    print(f"[3/4] 调用 lv_font_conv 生成 cbin 20px 4bpp...")
    print(f"      975MaruSC symbols 长度: {len(maru_str)} 字符 ({len(maru_str.encode('utf-8'))} 字节)")
    print(f"      noto-qwen IPA symbols 长度: {len(ipa_str)} 字符")

    CBIN_OUT.parent.mkdir(parents=True, exist_ok=True)
    LV_FONT_CONV_JS = ROOT / "tmp" / "lv_font_conv" / "lv_font_conv.js"
    if not LV_FONT_CONV_JS.exists():
        sys.exit(f"FATAL: 78/lv_font_conv 未安装: {LV_FONT_CONV_JS}\n"
                 f"先跑: cd {ROOT}/tmp && git clone https://github.com/78/lv_font_conv.git && cd lv_font_conv && npm install")
    cmd = [
        "node", str(LV_FONT_CONV_JS),
        "--bpp", "4",
        "--size", "20",
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
    print(f"      命令长度: {sum(len(a) for a in cmd)} 字节")

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("STDOUT:", result.stdout[-2000:])
        print("STDERR:", result.stderr[-2000:])
        sys.exit(f"lv_font_conv 失败 (exit {result.returncode})")
    print(f"      stdout: {result.stdout[-300:].strip()}")

    out_size = CBIN_OUT.stat().st_size
    print(f"[4/4] 生成成功: {CBIN_OUT}")
    print(f"      大小: {out_size:,} bytes ({out_size / 1024 / 1024:.2f} MB)")

    # 同步到 mydazy 子目录（项目实际加载路径）
    CBIN_OUT_MIRROR.parent.mkdir(parents=True, exist_ok=True)
    import shutil
    shutil.copy(CBIN_OUT, CBIN_OUT_MIRROR)
    print(f"      同步: {CBIN_OUT_MIRROR}")


if __name__ == "__main__":
    main()
