#!/usr/bin/env python3
"""
fontgen.py - rasterize the Barlow Semi Condensed TTFs into anti-aliased 4-bit
alpha bitmap fonts for the gfx engine (Main/gfx.h).

    python tools/fontgen.py            # regenerate every font in FONTS below
    python tools/fontgen.py --preview  # also write PNG previews to tools/fonts/preview/

Requirements: Python 3 + Pillow (no freetype-py needed).

Rasterization: each glyph is rendered by FreeType (through Pillow) at 4x the
target size with the pen origin on a whole target pixel, then box-filtered
4x4 -> 1. That gives geometrically exact, hinting-independent coverage with
16+ levels, which is exactly what the 4-bit output can hold.

Size convention: `px` is the EM size in pixels (same as CSS/Figma
`font-size`). Cap height of Barlow is ~0.70 em, so F_NUM_XL (128 px) has
~90 px tall digits. Every generated header lists the real metrics.

Output format (see gfx::Font / gfx::Glyph in Main/gfx.h):
  * glyph bitmaps are 4 bpp alpha, COLUMN-MAJOR (each column top->bottom,
    columns left->right), packed as one continuous nibble stream, high nibble
    first. Column-major matches the panel's native framebuffer orientation,
    so blits walk memory linearly.
  * glyph table sorted by codepoint (binary search at runtime).
  * xoff/yoff position the bitmap's top-left relative to the pen position on
    the baseline (yoff is negative = above the baseline).
"""

import argparse
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FONT_DIR = os.path.join(HERE, "fonts")
OUT_DIR = os.path.join(ROOT, "Main")

TTF = {
    "Medium": "BarlowSemiCondensed-Medium.ttf",
    "SemiBold": "BarlowSemiCondensed-SemiBold.ttf",
    "Bold": "BarlowSemiCondensed-Bold.ttf",
}

HUNGARIAN = "áéíóöőúüűÁÉÍÓÖŐÚÜŰ"
FULL = "".join(chr(c) for c in range(32, 127)) + "°" + HUNGARIAN + "·–—…−"
NUMERIC = " 0123456789.:-%+"
DIGITS = "0123456789"

# (C name, file stem, weight, em px, charset, description)
FONTS = [
    ("F_LABEL", "label", "SemiBold", 13, FULL, "small-caps labels (use with tracking)"),
    ("F_SMALL", "small", "Medium", 15, FULL, "secondary text"),
    ("F_BODY", "body", "Medium", 18, FULL, "body text"),
    ("F_BODYB", "bodyb", "SemiBold", 18, FULL, "emphasised body text / buttons"),
    ("F_TITLE", "title", "SemiBold", 24, FULL, "titles"),
    ("F_NUM_M", "num_m", "Bold", 32, FULL, "medium numbers (full charset)"),
    ("F_NUM_L", "num_l", "Bold", 56, NUMERIC, "large numbers (digits + . : - % + space)"),
    ("F_NUM_XL", "num_xl", "Bold", 128, NUMERIC, "hero speed number (digits + . : - % + space)"),
    ("F_NUM_XXL", "num_xxl", "Bold", 168, DIGITS, "minimalist skin speed (digits only)"),
]

SS = 4  # supersampling factor


def ofl_notice():
    return (
        "Glyphs rasterized from Barlow Semi Condensed, Copyright 2017 The Barlow\n"
        "Project Authors (https://github.com/jpt/barlow), licensed under the SIL\n"
        "Open Font License, Version 1.1 (see tools/fonts/OFL.txt,\n"
        "https://openfontlicense.org). This generated bitmap font is a derivative\n"
        "work distributed under the same license."
    )


def render_glyph(font_ss, ch):
    """Return (w, h, xoff, yoff, pixels[0..15] row-major list) at 1x."""
    l, t, r, b = font_ss.getbbox(ch, anchor="ls")
    if r <= l or b <= t:
        return 0, 0, 0, 0, []
    # target-pixel box that contains the supersampled ink box
    L, T = math.floor(l / SS), math.floor(t / SS)
    R, B = math.ceil(r / SS), math.ceil(b / SS)
    w, h = R - L, B - T
    im = Image.new("L", (w * SS, h * SS), 0)
    ImageDraw.Draw(im).text((-L * SS, -T * SS), ch, font=font_ss, fill=255, anchor="ls")
    im = im.reduce(SS)  # box filter
    px = list(im.tobytes())
    q = [min(15, (v * 15 + 127) // 255) for v in px]
    # trim empty borders after quantization
    rows = [y for y in range(h) if any(q[y * w + x] for x in range(w))]
    cols = [x for x in range(w) if any(q[y * w + x] for y in range(h))]
    if not rows:
        return 0, 0, 0, 0, []
    y0, y1, x0, x1 = rows[0], rows[-1] + 1, cols[0], cols[-1] + 1
    out = [q[y * w + x] for y in range(y0, y1) for x in range(x0, x1)]
    return x1 - x0, y1 - y0, L + x0, T + y0, out


def pack_column_major(w, h, pix):
    nib = [pix[y * w + x] for x in range(w) for y in range(h)]
    if len(nib) & 1:
        nib.append(0)
    return bytes((nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2))


def build_font(cname, stem, weight, px, charset, desc, preview=False):
    path = os.path.join(FONT_DIR, TTF[weight])
    f1 = ImageFont.truetype(path, px)
    fss = ImageFont.truetype(path, px * SS)
    ascent, descent = f1.getmetrics()
    cap = -fss.getbbox("H", anchor="ls")[1] / SS
    xh = -fss.getbbox("x", anchor="ls")[1] / SS
    dig_h = -fss.getbbox("0", anchor="ls")[1] / SS

    glyphs = []
    blob = bytearray()
    for ch in sorted(set(charset), key=ord):
        w, h, xo, yo, pix = render_glyph(fss, ch)
        adv = int(round(f1.getlength(ch)))
        off = len(blob)
        if w and h:
            blob += pack_column_major(w, h, pix)
        glyphs.append((ord(ch), w, h, xo, yo, adv, off, ch, pix))

    digit_adv = max((g[5] for g in glyphs if "0" <= g[7] <= "9"), default=0)
    space_adv = int(round(f1.getlength(" ")))
    line_h = ascent + descent

    lines = []
    lines.append(f"// {os.path.basename(stem_path(stem))} - GENERATED by tools/fontgen.py. DO NOT EDIT.")
    lines.append(f"// {cname}: Barlow Semi Condensed {weight}, {px} px em - {desc}")
    lines.append(f"// ascent {ascent}  descent {descent}  lineHeight {line_h}  capHeight {round(cap)}"
                 f"  xHeight {round(xh)}  digitHeight {round(dig_h)}  digitAdvance {digit_adv}")
    lines.append(f"// {len(glyphs)} glyphs, bitmap {len(blob)} bytes + table {len(glyphs) * 16} bytes")
    lines.append("//")
    for ln in ofl_notice().splitlines():
        lines.append("// " + ln)
    lines.append("#pragma once")
    lines.append('#include "gfx.h"')
    lines.append("")
    lines.append(f"inline constexpr uint8_t {cname}_bits[] PROGMEM = {{")
    for i in range(0, len(blob), 24):
        lines.append("  " + ",".join(f"0x{b:02x}" for b in blob[i:i + 24]) + ",")
    if not blob:
        lines.append("  0")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr gfx::Glyph {cname}_glyphs[] PROGMEM = {{")
    lines.append("  // off, cp, w, h, xoff, yoff, adv")
    for cp, w, h, xo, yo, adv, off, ch, _ in glyphs:
        label = ch if ch not in "\\" else "backslash"
        lines.append(f"  {{{off}, 0x{cp:04x}, {w}, {h}, {xo}, {yo}, {adv}}},  // {label}")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr gfx::Font {cname} = {{")
    lines.append(f"  {cname}_bits, {cname}_glyphs, {len(glyphs)},")
    lines.append(f"  /*size*/ {px}, /*ascent*/ {ascent}, /*descent*/ {descent}, /*lineHeight*/ {line_h},")
    lines.append(f"  /*capHeight*/ {round(cap)}, /*xHeight*/ {round(xh)}, /*digitHeight*/ {round(dig_h)},")
    lines.append(f"  /*digitAdvance*/ {digit_adv}, /*spaceAdvance*/ {space_adv},")
    lines.append(f'  "{cname}"')
    lines.append("};")
    lines.append("")

    with open(stem_path(stem), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))

    if preview:
        write_preview(cname, glyphs, ascent, descent)

    return {
        "name": cname, "weight": weight, "px": px, "glyphs": len(glyphs),
        "bitmap": len(blob), "table": len(glyphs) * 16, "ascent": ascent,
        "descent": descent, "line": line_h, "cap": round(cap), "digit_h": round(dig_h),
        "digit_adv": digit_adv, "space": space_adv,
    }


def stem_path(stem):
    return os.path.join(OUT_DIR, f"font_{stem}.h")


def write_preview(cname, glyphs, ascent, descent):
    d = os.path.join(FONT_DIR, "preview")
    os.makedirs(d, exist_ok=True)
    total = sum(g[5] for g in glyphs) + 8
    line = ascent + descent
    im = Image.new("L", (min(total, 2400), line + 8), 0)
    x = 4
    for cp, w, h, xo, yo, adv, off, ch, pix in glyphs:
        for yy in range(h):
            for xx in range(w):
                v = pix[yy * w + xx] * 17
                X, Y = x + xo + xx, 4 + ascent + yo + yy
                if 0 <= X < im.width and 0 <= Y < im.height:
                    im.putpixel((X, Y), v)
        x += adv
    im.save(os.path.join(d, f"{cname}.png"))


def write_index(stats):
    lines = [
        "// fonts.h - GENERATED by tools/fontgen.py. DO NOT EDIT.",
        "// Includes every anti-aliased gfx font. Regenerate: python tools/fontgen.py",
        "//",
    ]
    for ln in ofl_notice().splitlines():
        lines.append("// " + ln)
    lines.append("//")
    lines.append("// name        weight    em  asc desc line  cap digitH digitAdv  flash")
    for s in stats:
        lines.append(
            f"// {s['name']:<11} {s['weight']:<8} {s['px']:>4} {s['ascent']:>4} {s['descent']:>4}"
            f" {s['line']:>4} {s['cap']:>4} {s['digit_h']:>6} {s['digit_adv']:>8} {s['bitmap'] + s['table']:>6}"
        )
    total = sum(s["bitmap"] + s["table"] for s in stats)
    lines.append(f"// total font flash: {total} bytes")
    lines.append("#pragma once")
    for s in stats:
        stem = [f for f in FONTS if f[0] == s["name"]][0][1]
        lines.append(f'#include "font_{stem}.h"')
    lines.append("")
    with open(os.path.join(OUT_DIR, "fonts.h"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    return total


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preview", action="store_true", help="write PNG previews to tools/fonts/preview/")
    args = ap.parse_args()
    stats = []
    for spec in FONTS:
        s = build_font(*spec, preview=args.preview)
        stats.append(s)
        print(f"{s['name']:<10} {s['weight']:<8} {s['px']:>3}px  glyphs {s['glyphs']:>3}  "
              f"asc {s['ascent']:>3} line {s['line']:>3} cap {s['cap']:>3} digitAdv {s['digit_adv']:>3}  "
              f"{s['bitmap'] + s['table']:>6} B")
    total = write_index(stats)
    print(f"total font flash: {total} bytes ({total / 1024:.1f} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
