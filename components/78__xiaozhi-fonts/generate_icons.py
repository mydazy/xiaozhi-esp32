"""
重生成所有图标字体的入口（2026-04-30 重构为双字体源）

调用顺序：
  1) FA 字体 5 个字号（emoji 21 个 · 仅作 LVGL fallback 用）
     - 14_1 / 16_4 / 20_4 用 fa-regular-400.otf
     - 30_1 / 30_4 用 fa-light-300.otf
     - OTF 缺失时跳过（保留现有 .c · 字形冗余但无视觉影响）
  2) Phosphor 字体 4 个字号（114 UI 图标 · 主字体）
     - 14_1 / 16_4 用 Phosphor-Regular.ttf
     - 20_4 / 30_4 用 Phosphor-Fill.ttf
     - 生成后自动 patch fallback 链 → font_awesome_X_Y
  3) 双 header 重生成
     - font_awesome.h（21 emoji 宏 + #include "font_phosphor.h"）
     - font_phosphor.h（114 FONT_PHOSPHOR_* + 114 FONT_AWESOME_* alias）
"""
import os
import sys

# (size, bpp) 配置
FA_CONFIGS = [
    (14, 1),  # OLED 状态栏（备用）
    (16, 4),  # 14px 基线
    (20, 4),  # BUILTIN_ICON_FONT 主字号
    (30, 1),  # OLED 大图标 emoji 用
    (30, 4),  # LCD 大图标 emoji + UI 用
]

PHOSPHOR_CONFIGS = [
    (14, 1),
    (16, 4),
    (20, 4),
    (30, 4),  # OLED 30_1 不切 phosphor，OLED 大图标只渲染 emoji
]


def run(cmd):
    print(f"\n>> {cmd}")
    return os.system(cmd)


def main():
    # === 1) FA 字体（emoji 子集）===
    print("=" * 60)
    print("Step 1: 重生成 FA 字体（emoji 子集）")
    print("=" * 60)
    for size, bpp in FA_CONFIGS:
        ret = run(f"python3 font_awesome.py lvgl --font-size {size} --bpp {bpp}")
        if ret != 0:
            print(f"⚠ FA {size}_{bpp} 生成失败（OTF 字体缺失？继续）")

    # === 2) Phosphor 字体 + fallback patch ===
    print("\n" + "=" * 60)
    print("Step 2: 生成 Phosphor 字体 + fallback patch")
    print("=" * 60)
    for size, bpp in PHOSPHOR_CONFIGS:
        ret = run(f"python3 font_phosphor.py lvgl --font-size {size} --bpp {bpp}")
        if ret != 0:
            print(f"❌ Phosphor {size}_{bpp} 生成失败，终止")
            return ret

    # === 3) Header 重生成 ===
    print("\n" + "=" * 60)
    print("Step 3: 生成 header 文件")
    print("=" * 60)
    ret = run("python3 font_awesome.py generate")
    if ret != 0:
        print("❌ font_awesome.h 生成失败")
        return ret
    ret = run("python3 font_phosphor.py generate")
    if ret != 0:
        print("❌ font_phosphor.h 生成失败")
        return ret

    print("\n" + "=" * 60)
    print("✅ 全部生成完成")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
