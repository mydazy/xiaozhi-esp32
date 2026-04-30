"""
重生成所有图标字体（v3 · 2026-04-30 单字体合并）

字体合并：每个 .c 同时包含
  - 21 个 emoji（FA Free Regular · 11 个重映射）
  - 124 个 UI 图标（Phosphor Bold · 含 GPS/NFC/iBeacon/蓝牙变体）

产出 5 个 font_awesome_*.c：
  14_1   OLED 状态栏备用 · 1bpp
  16_4   小字号 4bpp
  20_4   BUILTIN_ICON_FONT 默认主字号 · 4bpp
  30_1   OLED 大图标 · 1bpp
  30_4   LCD 状态栏（用户选定 · CMakeLists 三 SKU 设此值）+ 表情大图标 · 4bpp

不再有 font_phosphor.* 双源结构（v2 已废弃）。
"""
import os
import sys

CONFIGS = [
    (14, 1),  # OLED 状态栏备用
    (16, 4),
    (20, 4),  # 默认 BUILTIN_ICON_FONT
    (30, 1),  # OLED 大图标
    (30, 4),  # LCD 状态栏（P30 三 SKU 实际用）
]


def run(cmd):
    print(f"\n>> {cmd}")
    return os.system(cmd)


def main():
    print("=" * 60)
    print("生成合并字体（emoji 21 + UI 图标 124 = 145 字形）")
    print("=" * 60)

    for size, bpp in CONFIGS:
        ret = run(f"python3 font_awesome.py lvgl --font-size {size} --bpp {bpp}")
        if ret != 0:
            print(f"❌ font_awesome_{size}_{bpp} 生成失败")
            return ret

    print("\n" + "=" * 60)
    print("生成 header 文件")
    print("=" * 60)
    ret = run("python3 font_awesome.py generate")
    if ret != 0:
        return ret

    print("\n" + "=" * 60)
    print("✅ 全部生成完成")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
