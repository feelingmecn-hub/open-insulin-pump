#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 LVGL 格式的中文字体 C 文件 (bpp=4, SPARSE_TINY cmap)。
- 从 ui_screen.cpp / ui_screen.h (真机固件目录与 PC 模拟器目录, 二者共用同一份 UI 文本)
  提取所有非 ASCII 显示字符 (quote-agnostic, 不依赖脆弱的正则)
- 额外补全: 全部可打印 ASCII (0x20-0x7E) + 关键符号 + 常用泵 UI 汉字安全网
- 主渲染字体: 系统黑体 Heiti SC (STHeiti Light.ttc, face 1)
- 缺字回退: Apple Symbols (覆盖 ⚠/✔ 等主字体缺失的符号)
- 用 Pillow 渲染灰度位图。
- 输出 lv_font_cn_16.c / lv_font_cn_12.c (bpp=4, 16级灰阶)

LVGL 9.5.0 字形放置公式 (lv_draw_label.c:624):
    x1 = pos.x + ofs_x
    y1 = pos.y + (line_height - base_line) - box_h - ofs_y
约定: line_height = ascent + descent, base_line = ascent

bpp=4 打包规则:
    每字节存 2 像素, 高 nibble = 左/上像素, 低 nibble = 右/下像素。
    若总像素数为奇数, 末字节低 nibble 补零。
    LVGL draw 时将 0~15 线性映射到 0~255 alpha。

重要: 单次 cmap 用 SPARSE_TINY 且 glyph_id_ofs_list = NULL, 此时 glyph id = glyph_id_start + 索引。
       数组索引 0 处固定插入占位符 U+0000, 让所有真实字符从索引 1 开始, 以避开
       LVGL `get_glyph_dsc_fmt_txt` 的 `if(!gid) return false;` 守卫 (gid==0 会被当作字形未找到)。
"""
import os, glob
from fontTools.ttLib import TTFont
from PIL import Image, ImageFont, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_TTC = "/System/Library/Fonts/STHeiti Light.ttc"
FACE = 1  # Heiti SC (简体)
APPLE_SYM = "/System/Library/Fonts/Apple Symbols.ttf"
ARIAL_UNI = "/Library/Fonts/Arial Unicode.ttf"
SIZES = [(16, "lv_font_cn_16"), (12, "lv_font_cn_12")]

# ---- 1. 从 UI 显示源码提取字符集 (真机与模拟器共用 ui_screen.cpp) ----
chars = set()
SCAN_DIRS = [
    HERE,                                                          # 模拟器目录 (simulator/lvgl_sdl/src)
    os.path.abspath(os.path.join(HERE, "..", "..", "..", "code", "esp32_firmware", "src")),
]
SCAN_FILES = ["ui_screen.cpp", "ui_screen.h"]
for d in SCAN_DIRS:
    for fn in SCAN_FILES:
        p = os.path.join(d, fn)
        if not os.path.exists(p):
            continue
        txt = open(p, encoding="utf-8", errors="ignore").read()
        for ch in txt:
            if ord(ch) >= 0x20:
                chars.add(ch)

# ---- 2. 补全全部可打印 ASCII (0x20 ~ 0x7E) ----
# snprintf 输出的数字/符号可能不出现在源码字面量中
for cp in range(0x20, 0x7F):
    chars.add(chr(cp))

# ---- 3. 关键符号 (即使源码未出现也保证存在) ----
for sym in "\u2014\u2026\u2192\u2191\u2193\u2196\u2197\u2198\u2199\u25B2\u25BC\u25B6\u25C0\u26A0\u2713\u2714\u2717":
    chars.add(sym)

# ---- 4. 补全常用泵 UI 汉字 (防止运行时拼接遗漏) ----
extra_cjk = (
    "总 量 值 设 单 位 毫 升 国际 单位 毫摩尔 每 升 小 时"
    "分 秒 周 月 年 版 本 号 硬 件 型 固 件 信 息"
    "正 在 等 待 试 配 对 成 功 失 败 错 误 警 告 提 示"
    "确 认 取 消 删 除 改 保 存 读 写 发 送 接 收"
    "加 减 乘 除 大 于 小 于 等 于 高 低 多 少"
    "长 宽 高 深 轻 重 快 慢 新 旧 好 坏"
    "开 关 停 启 暂 恢 继 续 返 回 退 出 进 入"
    "选 择 浏 览 搜 索 排 序 过 滤 显 隐藏"
    "日 一 二 三 四 五 六 天 星 期"
    "东 西 南 北 上 下 左 右 前 后 内 外"
    "自动 手动 远程 近程 本地 远程 无线 有线"
    "安 全 密 码 锁 解 注 册 登 录 用 户 名"
    "数 据 日 志 记 录 历 史 统 计 图 表 曲 线"
    "参 数 默认 工厂 恢复出厂设置 校准 标定"
    "针头 管 路 导管 敷贴 胶布 酒精 棉片"
    "脂肪 蛋白 碳水化合物 克 国际单位"
    "百分比 百分 号 摄氏度 度"
)
for ch in extra_cjk:
    if ch != ' ':
        chars.add(ch)

print("字符集大小:", len(chars))

# ---- 5. 准备字体 (主 + 回退) ----
heiti = TTFont(SRC_TTC, fontNumber=FACE)
heiti_cmap = heiti.getBestCmap()
apple = TTFont(APPLE_SYM, fontNumber=0)
apple_cmap = apple.getBestCmap()
arial = TTFont(ARIAL_UNI, fontNumber=0)
arial_cmap = arial.getBestCmap()


def pick_font(cp):
    """返回该 codepoint 应使用的字体键名; 都不支持则返回 None。
    优先级: 黑体(Heiti SC) -> Apple Symbols(符号) -> Arial Unicode(兜底符号/CJK)。"""
    if cp in heiti_cmap:
        return "heiti"
    if cp in apple_cmap:
        return "apple"
    if cp in arial_cmap:
        return "arial"
    return None


def render_glyph(f, ch, H, adv):
    """渲染单个字形, 返回 (bitmap_bytes, box_w, box_h, ofs_x, ofs_y)。"""
    bb = f.getbbox(ch)
    if bb is None:
        return b"", 0, 0, 0, 0
    x0, y0, x1, y1 = bb
    cw = max(int(x1) + 2, int(f.getlength(ch)) + 2, 8)
    canvas = Image.new("L", (cw, H), 0)
    d = ImageDraw.Draw(canvas)
    d.text((0, 0), ch, font=f, fill=255)
    ib = canvas.getbbox()
    if ib is None:
        return b"", 0, 0, 0, 0
    ix0, iy0, ix1, iy1 = ib
    box_w = ix1 - ix0
    box_h = iy1 - iy0
    ofs_x = ix0
    ascent_px = f.getmetrics()[0]
    ofs_y = (H - ascent_px) - box_h - iy0
    bm = canvas.crop((ix0, iy0, ix1, iy1)).tobytes()
    return bm, box_w, box_h, ofs_x, ofs_y


def quantize_to_4bpp(bitmap_8bpp):
    """将 8bpp 灰度位图 (值 0~255) 量化为 4bpp 打包 (2 像素/字节)。
    LVGL bpp=4 将 0~15 线性映射到 alpha 0~255: px * 255 / 15。
    打包: 高 nibble = 第一个像素, 低 nibble = 第二个像素。
    奇数像素时末字节低 nibble 补零。"""
    n = len(bitmap_8bpp)
    out = bytearray()
    for i in range(0, n, 2):
        hi = round(bitmap_8bpp[i] / 17) & 0xF
        lo = round(bitmap_8bpp[i + 1] / 17) & 0xF if (i + 1 < n) else 0
        out.append((hi << 4) | lo)
    return bytes(out)


def write_c(name, bitmaps, glyph_dsc, unicode_list, H, base_line):
    N = len(unicode_list)
    bm_lines = []
    for i in range(0, len(bitmaps), 16):
        row = bitmaps[i:i + 16]
        bm_lines.append("    " + ", ".join("0x%02x" % b for b in row) + ",")

    gd_lines = []
    for (bidx, adv_w, w, h, ox, oy) in glyph_dsc:
        gd_lines.append(
            "    { .bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d },"
            % (bidx, adv_w, w, h, ox, oy))

    ul_str = ", ".join("0x%04x" % u for u in unicode_list)
    max_cp = max(unicode_list) if unicode_list else 0

    lines = []
    lines.append("/* Auto-generated Chinese font (%s) - LVGL fmt_txt, bpp=4 */" % name)
    lines.append('#include "lvgl.h"')
    lines.append("")
    lines.append("static const uint8_t %s_glyph_bitmap[] = {" % name)
    lines.extend(bm_lines)
    lines.append("};")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_glyph_dsc_t %s_glyph_dsc[] = {" % name)
    lines.extend(gd_lines)
    lines.append("};")
    lines.append("")
    lines.append("static const uint16_t %s_unicode_list[] = { %s };" % (name, ul_str))
    lines.append("")
    lines.append("static const lv_font_fmt_txt_cmap_t %s_cmap = {" % name)
    lines.append("    .range_start = 0,")
    lines.append("    .range_length = %d," % (max_cp + 1))
    lines.append("    .glyph_id_start = 0,")
    lines.append("    .unicode_list = %s_unicode_list," % name)
    lines.append("    .glyph_id_ofs_list = NULL,")
    lines.append("    .list_length = %d," % N)
    lines.append("    .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,")
    lines.append("};")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_dsc_t %s_dsc = {" % name)
    lines.append("    .glyph_bitmap = %s_glyph_bitmap," % name)
    lines.append("    .glyph_dsc = %s_glyph_dsc," % name)
    lines.append("    .cmaps = &%s_cmap," % name)
    lines.append("    .kern_dsc = NULL,")
    lines.append("    .kern_scale = 0,")
    lines.append("    .cmap_num = 1,")
    lines.append("    .bpp = 4,")
    lines.append("    .kern_classes = 0,")
    lines.append("    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,")
    lines.append("    .stride = 0,")
    lines.append("};")
    lines.append("")
    lines.append("lv_font_t %s = {" % name)
    lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    lines.append("    .line_height = %d," % H)
    lines.append("    .base_line = %d," % base_line)
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append("    .underline_position = -1,")
    lines.append("    .underline_thickness = 1,")
    lines.append("    .dsc = &%s_dsc," % name)
    lines.append("};")
    lines.append("")
    out = os.path.join(HERE, name + ".c")
    with open(out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    print("wrote", out, "glyphs=%d bitmap_bytes=%d" % (N, len(bitmaps)))


for size, name in SIZES:
    fonts = {
        "heiti": ImageFont.truetype(SRC_TTC, size, index=FACE),
        "apple": ImageFont.truetype(APPLE_SYM, size, index=0),
        "arial": ImageFont.truetype(ARIAL_UNI, size, index=0),
    }
    ascent, descent = fonts["heiti"].getmetrics()
    H = ascent + descent
    bitmaps = bytearray()
    glyph_dsc = []
    unicode_list = []
    chars_sorted = sorted(chars, key=lambda c: ord(c))
    PLACEHOLDER = '\x00'  # NULL 字符, 不会被正常 UI 文本用到
    chars_sorted_with_placeholder = [PLACEHOLDER] + list(chars_sorted)

    n_apple = 0
    n_arial = 0
    for ch in chars_sorted_with_placeholder:  # 索引0=占位符, 真实字符从1开始
        cp = ord(ch)
        which = pick_font(cp)
        if which is None:
            # 主字体与回退字体都不支持: 占位空字形 (gid==0 守卫安全)
            glyph_dsc.append((0, int(round(fonts["heiti"].getlength(ch) * 16)) if cp != 0 else 0, 0, 0, 0, 0))
            unicode_list.append(cp)
            continue
        if which == "apple":
            n_apple += 1
        if which == "arial":
            n_arial += 1
        f = fonts[which]
        bm, w, h, ox, oy = render_glyph(f, ch, H, int(round(f.getlength(ch) * 16)))
        if w == 0 or h == 0:
            # 空字形(空格等): 占位但不存 bitmap
            glyph_dsc.append((0, int(round(f.getlength(ch) * 16)), 0, 0, 0, 0))
            unicode_list.append(cp)
            continue
        bidx = len(bitmaps)
        bitmaps += quantize_to_4bpp(bm)
        glyph_dsc.append((bidx, int(round(f.getlength(ch) * 16)), w, h, ox, oy))
        unicode_list.append(cp)
    if n_apple:
        print("  (%s) 回退到 Apple Symbols 的字形数: %d" % (name, n_apple))
    if n_arial:
        print("  (%s) 回退到 Arial Unicode 的字形数: %d" % (name, n_arial))
    write_c(name, bitmaps, glyph_dsc, unicode_list, H, ascent)

print("DONE")
