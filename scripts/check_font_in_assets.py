#!/usr/bin/env python3
"""检查 assets.bin（或设备 dump 出的分区）是否包含指定字体文件

用法：
    # 检查 build 产物
    python3 scripts/check_font_in_assets.py build/generated_assets.bin

    # 检查设备 dump（先 esptool.py read_flash 0x800000 0x800000 device_assets.bin）
    python3 scripts/check_font_in_assets.py device_assets.bin

    # 只检查特定文件
    python3 scripts/check_font_in_assets.py build/generated_assets.bin font_maru_56_4.bin
"""
import struct
import sys
import os

TABLE_ENTRY_SIZE = 44  # name[32] + size(4) + offset(4) + width(2) + height(2)
HEADER_SIZE = 12       # total_files(4) + checksum(4) + length(4)


def inspect(bin_path, filter_name=None):
    if not os.path.exists(bin_path):
        print(f"❌ 文件不存在: {bin_path}")
        return 2
    with open(bin_path, 'rb') as f:
        data = f.read()

    total = struct.unpack('<I', data[0:4])[0]
    chksum = struct.unpack('<I', data[4:8])[0]
    length = struct.unpack('<I', data[8:12])[0]

    print(f"📦 {bin_path}")
    print(f"   总文件数 : {total}")
    print(f"   校验和   : 0x{chksum:08X}")
    print(f"   数据长度 : {length:,} bytes")
    print(f"   文件大小 : {len(data):,} bytes")
    print()

    found_target = False
    fonts = []
    for i in range(total):
        off = HEADER_SIZE + i * TABLE_ENTRY_SIZE
        name = data[off:off+32].rstrip(b'\x00').decode('utf-8', errors='replace')
        size = struct.unpack('<I', data[off+32:off+36])[0]
        asset_off = struct.unpack('<I', data[off+36:off+40])[0]

        if filter_name and name != filter_name:
            continue
        # 验证 magic 'Z' 'Z'
        data_pos = HEADER_SIZE + total * TABLE_ENTRY_SIZE + asset_off
        magic_ok = data[data_pos:data_pos+2] == b'\x5A\x5A'
        marker = '✅' if magic_ok else '❌'

        if name.startswith('font_'):
            fonts.append((name, size, magic_ok))
        if filter_name and name == filter_name:
            found_target = True
            print(f"{marker} 找到 {name}")
            print(f"   size      = {size:,} bytes ({size/1024:.1f} KB)")
            print(f"   table off = 0x{asset_off:08X}")
            print(f"   magic ZZ  = {'OK' if magic_ok else 'CORRUPT'}")
            # 解析 cbin header 验证 lv_font_t
            font_start = data_pos + 2
            line_h = struct.unpack('<i', data[font_start+12:font_start+16])[0]
            base_l = struct.unpack('<i', data[font_start+16:font_start+20])[0]
            dsc_off = struct.unpack('<I', data[font_start+24:font_start+28])[0]
            print(f"   line_h    = {line_h} (期待 ~ pt 数)")
            print(f"   base_line = {base_l}")
            print(f"   dsc_off   = {dsc_off} (期待 36 = sizeof lv_font_t)")

    if filter_name and not found_target:
        print(f"❌ 未找到 {filter_name}")
        print(f"   现有字体类资源:")
        for name, size, _ in fonts:
            print(f"     - {name:35} ({size:>10,} bytes)")
        return 1

    if not filter_name:
        print(f"📋 全部字体类资源 ({len(fonts)} 个):")
        for name, size, magic_ok in fonts:
            marker = '✅' if magic_ok else '❌'
            print(f"   {marker} {name:35} {size:>10,} bytes ({size/1024:.1f} KB)")
    return 0


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    bin_path = sys.argv[1]
    filter_name = sys.argv[2] if len(sys.argv) > 2 else None
    sys.exit(inspect(bin_path, filter_name))
