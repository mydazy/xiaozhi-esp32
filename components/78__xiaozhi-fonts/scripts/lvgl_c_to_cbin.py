#!/usr/bin/env python3
"""
将 lv_font_conv --format lvgl 生成的 C 源码转换为 cbin 二进制格式。

cbin 格式是 78/lv_font_conv fork 的专有格式，用于 ESP32 上 cbin_font_create() 运行时加载。
当 GitHub 不可达或 78/lv_font_conv 无法安装时，可通过标准 npx lv_font_conv 生成 C 源码，
再用本脚本转换为 cbin 二进制。

二进制布局（LVGL 9，ESP32 32-bit）:
  [0:36]   lv_font_t          3 个函数指针(=0) + line_height + base_line + flags + dsc_offset + fallback(=0) + user_data(=0)
  [36:60]  lv_font_fmt_txt_dsc_t  bitmap/glyph_dsc/cmaps/kern 偏移 + kern_scale + bitfield(cmap_num/bpp/kern_classes/bitmap_format) + stride
  [60:...] 数据段（kern → bitmap → glyph_dsc → cmap）所有偏移从 dsc 起算

用法:
  python3 lvgl_c_to_cbin.py <input.c> <output.bin>

示例:
  npx lv_font_conv --font font.ttf --format lvgl --bpp 4 --size 88 --no-compress --no-prefilter -o /tmp/font.c --symbols '0123456789:-. '
  python3 scripts/lvgl_c_to_cbin.py /tmp/font.c main/assets/fonts/font_number_88_4.bin
"""

import re
import struct
import sys
import os


def parse_hex_array(text):
    """从 C 源码中提取十六进制字节数组"""
    values = re.findall(r'0x([0-9a-fA-F]+)', text)
    return bytes(int(v, 16) for v in values)


def parse_glyph_dsc(c_src):
    """解析 glyph_dsc[] 数组"""
    pattern = (
        r'\{\.bitmap_index\s*=\s*(\d+),\s*'
        r'\.adv_w\s*=\s*(\d+),\s*'
        r'\.box_w\s*=\s*(\d+),\s*'
        r'\.box_h\s*=\s*(\d+),\s*'
        r'\.ofs_x\s*=\s*(-?\d+),\s*'
        r'\.ofs_y\s*=\s*(-?\d+)\}'
    )
    matches = re.findall(pattern, c_src)
    glyphs = []
    for m in matches:
        glyphs.append({
            'bitmap_index': int(m[0]),
            'adv_w': int(m[1]),
            'box_w': int(m[2]),
            'box_h': int(m[3]),
            'ofs_x': int(m[4]),
            'ofs_y': int(m[5]),
        })
    return glyphs


def pack_glyph_dsc(glyph, large=True):
    """打包单个 glyph_dsc (large=True: 16字节, large=False: 8字节)"""
    if large:
        # LV_FONT_FMT_TXT_LARGE == 1: uint32+uint32+uint16*2+int16*2 = 16 bytes
        return struct.pack('<IIHHhh',
                           glyph['bitmap_index'],
                           glyph['adv_w'],
                           glyph['box_w'] & 0xFFFF,
                           glyph['box_h'] & 0xFFFF,
                           glyph['ofs_x'],
                           glyph['ofs_y'])
    else:
        # LV_FONT_FMT_TXT_LARGE == 0: bitmap_index:20 | adv_w:12, box_w/h, ofs_x/y
        dword1 = (glyph['bitmap_index'] & 0xFFFFF) | ((glyph['adv_w'] & 0xFFF) << 20)
        return struct.pack('<IBBBB',
                           dword1,
                           glyph['box_w'] & 0xFF,
                           glyph['box_h'] & 0xFF,
                           glyph['ofs_x'] & 0xFF,
                           glyph['ofs_y'] & 0xFF)


def parse_cmaps(c_src):
    """解析 cmaps[] 数组"""
    pattern = (
        r'\.range_start\s*=\s*(\d+),\s*'
        r'\.range_length\s*=\s*(\d+),\s*'
        r'\.glyph_id_start\s*=\s*(\d+),\s*'
        r'\.unicode_list\s*=\s*(NULL|\w+),\s*'
        r'\.glyph_id_ofs_list\s*=\s*(NULL|\w+),\s*'
        r'\.list_length\s*=\s*(\d+),\s*'
        r'\.type\s*=\s*(\w+)'
    )
    cmap_type_map = {
        'LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL': 0,
        'LV_FONT_FMT_TXT_CMAP_SPARSE_FULL': 1,
        'LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY': 2,
        'LV_FONT_FMT_TXT_CMAP_SPARSE_TINY': 3,
    }
    cmaps = []
    for m in re.findall(pattern, c_src):
        cmaps.append({
            'range_start': int(m[0]),
            'range_length': int(m[1]),
            'glyph_id_start': int(m[2]),
            'unicode_list': None if m[3] == 'NULL' else m[3],
            'glyph_id_ofs_list': None if m[4] == 'NULL' else m[4],
            'list_length': int(m[5]),
            'type': cmap_type_map.get(m[6], 2),
        })
    return cmaps


def parse_uint16_array(c_src, name):
    """从 C 源码中解析 uint16_t 数组（用于 unicode_list）"""
    pattern = rf'(?:static\s+)?(?:const\s+)?uint16_t\s+{re.escape(name)}\[\]\s*=\s*\{{([^}}]+)\}}'
    m = re.search(pattern, c_src)
    if not m:
        return None
    values = [int(x.strip(), 0) for x in m.group(1).split(',') if x.strip()]
    return b''.join(struct.pack('<H', v & 0xFFFF) for v in values)


def parse_uint8_array(c_src, name):
    """从 C 源码中解析 uint8_t 数组（用于 glyph_id_ofs_list FORMAT0_FULL）"""
    pattern = rf'(?:static\s+)?(?:const\s+)?uint8_t\s+{re.escape(name)}\[\]\s*=\s*\{{([^}}]+)\}}'
    m = re.search(pattern, c_src)
    if not m:
        return None
    values = [int(x.strip(), 0) for x in m.group(1).split(',') if x.strip()]
    return bytes(v & 0xFF for v in values)


def parse_cmap_aux_array(c_src, name, cmap_type):
    """根据 cmap 类型解析辅助数组。
    glyph_id_ofs_list: FORMAT0_FULL → uint8_t[], SPARSE_FULL → uint16_t[]
    unicode_list: 始终 uint16_t[]
    """
    # 先尝试 uint16，再尝试 uint8
    data = parse_uint16_array(c_src, name)
    if data is None:
        data = parse_uint8_array(c_src, name)
    return data


def parse_kern_pairs(c_src):
    """解析 kern pair 数据（kern_classes == 0 时使用）"""
    gid_match = re.search(r'kern_pair_glyph_ids\[\]\s*=\s*\{([^}]+)\}', c_src)
    val_match = re.search(r'kern_pair_values\[\]\s*=\s*\{([^}]+)\}', c_src)
    pair_match = re.search(r'\.pair_cnt\s*=\s*(\d+)', c_src)
    gid_size_match = re.search(r'\.glyph_ids_size\s*=\s*(\d+)', c_src)

    if not all([gid_match, val_match, pair_match]):
        return None

    glyph_ids_raw = [int(x.strip()) for x in gid_match.group(1).split(',') if x.strip()]
    values_raw = [int(x.strip()) for x in val_match.group(1).split(',') if x.strip()]
    pair_cnt = int(pair_match.group(1))
    gid_size = int(gid_size_match.group(1)) if gid_size_match else 0

    if gid_size == 0:
        glyph_ids_bytes = bytes(v & 0xFF for v in glyph_ids_raw)
    else:
        glyph_ids_bytes = b''.join(struct.pack('<H', v & 0xFFFF) for v in glyph_ids_raw)

    values_bytes = bytes(v & 0xFF for v in values_raw)

    return {
        'glyph_ids': glyph_ids_bytes,
        'values': values_bytes,
        'pair_cnt': pair_cnt,
        'glyph_ids_size': gid_size,
    }


def parse_font_info(c_src):
    """解析 font_dsc 和 font 顶级结构体中的元数据"""
    fields = {
        'kern_scale': (r'\.kern_scale\s*=\s*(\d+)', 0),
        'cmap_num': (r'\.cmap_num\s*=\s*(\d+)', 0),
        'bpp': (r'\.bpp\s*=\s*(\d+)', 4),
        'kern_classes': (r'\.kern_classes\s*=\s*(\d+)', 0),
        'bitmap_format': (r'\.bitmap_format\s*=\s*(\d+)', 0),
        'line_height': (r'\.line_height\s*=\s*(\d+)', 0),
        'base_line': (r'\.base_line\s*=\s*(\d+)', 0),
        'underline_position': (r'\.underline_position\s*=\s*(-?\d+)', 0),
        'underline_thickness': (r'\.underline_thickness\s*=\s*(\d+)', 0),
    }
    info = {}
    for key, (pattern, default) in fields.items():
        m = re.search(pattern, c_src)
        info[key] = int(m.group(1)) if m else default
    return info


def align4(offset):
    """将偏移量向上对齐到 4 字节边界"""
    return offset + (4 - offset % 4) % 4


def build_cbin(c_src):
    """将 lv_font_conv 生成的 C 源码构建为 cbin 二进制"""
    # 解析所有数据段
    bitmap_match = re.search(r'glyph_bitmap\[\]\s*=\s*\{(.*?)\};', c_src, re.DOTALL)
    if not bitmap_match:
        raise ValueError("未找到 glyph_bitmap[] 数组")
    bitmap_data = parse_hex_array(bitmap_match.group(1))

    glyphs = parse_glyph_dsc(c_src)
    cmaps = parse_cmaps(c_src)
    kern = parse_kern_pairs(c_src)
    info = parse_font_info(c_src)

    has_kern = kern is not None and info['kern_classes'] == 0

    print(f"  bitmap: {len(bitmap_data)} bytes, glyphs: {len(glyphs)}, cmaps: {len(cmaps)}")
    print(f"  kern: {'pairs=' + str(kern['pair_cnt']) if has_kern else 'none'}")
    print(f"  line_height={info['line_height']}, bpp={info['bpp']}")

    # === 计算数据段布局（所有偏移从 dsc 起算）===
    SIZEOF_FONT = 36   # lv_font_t on ESP32 (32-bit)
    SIZEOF_DSC = 24    # lv_font_fmt_txt_dsc_t
    GLYPH_DSC_SIZE = 16  # LV_FONT_FMT_TXT_LARGE=1: uint32+uint32+uint16*4 = 16 bytes
    cursor = SIZEOF_DSC  # 数据段从 dsc 之后开始

    print(f"  glyph_dsc_size={GLYPH_DSC_SIZE} bytes/glyph (LV_FONT_FMT_TXT_LARGE=1: u32+u32+u16*4)")

    # 1. kern pair 结构 + 数据
    if has_kern:
        kern_struct_off = cursor
        cursor += 12  # sizeof(lv_font_fmt_txt_kern_pair_t)
        cursor += len(kern['glyph_ids'])
        cursor += len(kern['values'])
        cursor = align4(cursor)
    else:
        kern_struct_off = 0

    # 2. glyph bitmap
    bitmap_off = cursor
    cursor += len(bitmap_data)
    cursor = align4(cursor)

    # 3. glyph_dsc
    glyph_dsc_off = cursor
    cursor += len(glyphs) * GLYPH_DSC_SIZE

    # 4. cmap
    cmap_off = cursor

    # === 构建 lv_font_t (36 bytes) ===
    font = struct.pack('<III', 0, 0, 0)                          # 3 function pointers
    font += struct.pack('<ii', info['line_height'], info['base_line'])
    font += struct.pack('<bbb', 0, info['underline_position'], info['underline_thickness'])
    font += struct.pack('<B', 0)                                  # padding
    font += struct.pack('<III', SIZEOF_FONT, 0, 0)               # dsc_offset, fallback, user_data
    assert len(font) == SIZEOF_FONT

    # === 构建 lv_font_fmt_txt_dsc_t (24 bytes) ===
    bitfield = ((info['cmap_num'] & 0x1FF) |
                ((info['bpp'] & 0xF) << 9) |
                ((info['kern_classes'] & 0x1) << 13) |
                ((info['bitmap_format'] & 0x3) << 14))

    dsc = struct.pack('<IIII', bitmap_off, glyph_dsc_off, cmap_off,
                      kern_struct_off if has_kern else 0)
    dsc += struct.pack('<HH', info['kern_scale'], bitfield)
    dsc += struct.pack('<BBBB', 0, 0, 0, 0)                      # stride + padding
    assert len(dsc) == SIZEOF_DSC

    # === 构建数据段 ===
    data = bytearray()

    # 1. kern
    if has_kern:
        gid_off_from_kern = 12
        val_off_from_kern = gid_off_from_kern + len(kern['glyph_ids'])
        pair_bits = (kern['pair_cnt'] & 0x3FFFFFFF) | ((kern['glyph_ids_size'] & 0x3) << 30)
        data.extend(struct.pack('<III', gid_off_from_kern, val_off_from_kern, pair_bits))
        data.extend(kern['glyph_ids'])
        data.extend(kern['values'])
        while len(data) % 4 != 0:
            data.append(0)

    # 2. bitmap
    assert len(data) + SIZEOF_DSC == bitmap_off
    data.extend(bitmap_data)
    while len(data) + SIZEOF_DSC != glyph_dsc_off:
        data.append(0)

    # 3. glyph_dsc (LV_FONT_FMT_TXT_LARGE=1, 16 bytes each)
    for g in glyphs:
        data.extend(pack_glyph_dsc(g, large=True))

    # 4. cmap (每个 20 字节) + 辅助数据（unicode_list / glyph_id_ofs_list）
    #
    # cbin_font_create 解析逻辑:
    #   cmaps_addr = bin_addr + dsc->cmaps
    #   cmap[i].unicode_list 存储相对于 cmaps_addr 的偏移
    #   addr_add 将偏移 + cmaps_addr 转为绝对地址
    #
    # 布局: [cmap_entry_0][cmap_entry_1]...[辅助数组数据...]
    #        ^cmaps_addr                    ^偏移指向这里
    assert len(data) + SIZEOF_DSC == cmap_off

    CMAP_ENTRY_SIZE = 20
    cmap_header_size = len(cmaps) * CMAP_ENTRY_SIZE

    # 第一遍：收集辅助数组数据，计算偏移
    aux_data = bytearray()
    cmap_aux_info = []  # [(ul_offset, ofs_offset), ...]

    for cm in cmaps:
        ul_offset = 0
        ofs_offset = 0

        if cm['unicode_list'] is not None:
            arr = parse_uint16_array(c_src, cm['unicode_list'])
            if arr is None:
                raise ValueError(f"未找到 unicode_list 数组: {cm['unicode_list']}")
            ul_offset = cmap_header_size + len(aux_data)
            aux_data.extend(arr)

        if cm['glyph_id_ofs_list'] is not None:
            arr = parse_cmap_aux_array(c_src, cm['glyph_id_ofs_list'], cm['type'])
            if arr is None:
                raise ValueError(f"未找到 glyph_id_ofs_list 数组: {cm['glyph_id_ofs_list']}")
            ofs_offset = cmap_header_size + len(aux_data)
            aux_data.extend(arr)

        cmap_aux_info.append((ul_offset, ofs_offset))

    # 第二遍：写入 cmap 条目
    for i, cm in enumerate(cmaps):
        ul_off, ofs_off = cmap_aux_info[i]
        data.extend(struct.pack('<I', cm['range_start']))
        data.extend(struct.pack('<H', cm['range_length']))
        data.extend(struct.pack('<H', cm['glyph_id_start']))
        data.extend(struct.pack('<I', ul_off))
        data.extend(struct.pack('<I', ofs_off))
        data.extend(struct.pack('<H', cm['list_length']))
        data.extend(struct.pack('<B', cm['type']))
        data.extend(struct.pack('<B', 0))  # padding

    # 写入辅助数组数据
    data.extend(aux_data)

    return font + dsc + bytes(data)


def verify_cbin(cbin_data):
    """验证生成的 cbin 二进制结构正确性"""
    ptrs = struct.unpack_from('<III', cbin_data, 0)
    assert ptrs == (0, 0, 0), f"前 3 个指针应为 0，实际: {ptrs}"

    line_h = struct.unpack_from('<i', cbin_data, 12)[0]
    base_l = struct.unpack_from('<i', cbin_data, 16)[0]
    dsc_off = struct.unpack_from('<I', cbin_data, 24)[0]
    assert dsc_off == 36, f"dsc_offset 应为 36，实际: {dsc_off}"

    bitfield = struct.unpack_from('<H', cbin_data, dsc_off + 18)[0]
    cmap_num = bitfield & 0x1FF
    bpp = (bitfield >> 9) & 0xF
    assert bpp > 0, f"bpp 应 > 0，实际: {bpp}"
    assert cmap_num > 0, f"cmap_num 应 > 0，实际: {cmap_num}"

    bm_off = struct.unpack_from('<I', cbin_data, dsc_off)[0]
    gd_off = struct.unpack_from('<I', cbin_data, dsc_off + 4)[0]
    cm_off = struct.unpack_from('<I', cbin_data, dsc_off + 8)[0]
    assert bm_off < len(cbin_data), f"bitmap offset 越界: {bm_off}"
    assert gd_off < len(cbin_data), f"glyph_dsc offset 越界: {gd_off}"
    assert cm_off < len(cbin_data), f"cmap offset 越界: {cm_off}"

    print(f"  验证通过: line_height={line_h}, base_line={base_l}, bpp={bpp}, cmaps={cmap_num}")
    print(f"  bitmap=+{bm_off}, glyph_dsc=+{gd_off}, cmap=+{cm_off}")


def main():
    if len(sys.argv) < 3:
        print(f"用法: {sys.argv[0]} <input.c> <output.bin>")
        print(f"示例: {sys.argv[0]} /tmp/font_88.c main/assets/fonts/font_number_88_4.bin")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2]

    print(f"[1/3] 读取 C 源码: {input_file}")
    with open(input_file, 'r') as f:
        c_src = f.read()

    print("[2/3] 转换为 cbin 格式...")
    cbin_data = build_cbin(c_src)

    print(f"[3/3] 写入: {output_file} ({len(cbin_data)} bytes)")
    os.makedirs(os.path.dirname(output_file) or '.', exist_ok=True)
    with open(output_file, 'wb') as f:
        f.write(cbin_data)

    verify_cbin(cbin_data)
    print("完成!")


if __name__ == '__main__':
    main()
