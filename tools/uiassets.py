#!/usr/bin/env python3
"""
uiassets.py - convert the designer's exported UI assets into Main/ui_assets.h.

    python tools/uiassets.py            # writes Main/ui_assets.h
    python tools/uiassets.py --check    # also compares the vector icon renderer
                                        # against the exported 16/20 px masks

Inputs (exported by design/mockups.html -> "Export PNGs + spec"):
  design/png/icons/icon_<name>_<16|20>.png   white + alpha icon masks
  design/png/thumbs/skin_<skin>_<theme>.png  144x96 skin picker thumbnails

Outputs (gfx engine formats, see Main/gfx.h):
  * icons  -> 4-bit alpha masks for gfx::mask4() (row-major, (w+1)/2 bytes per
              row, high nibble = left pixel).
  * thumbs -> RGB565 in the framebuffer's native column order for
              gfx::imageNative() (27 648 B each).

The mockups only export icons at 16 and 20 px. A few are drawn at other sizes
(18 px WiFi in the top bar / check on the skin card, 22 px sun on the
brightness slider); those are rasterized here from the same vector recipes as
design/mockups.html (ICONS), 8x8 supersampled.

Requires Pillow.
"""

import argparse
import math
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ICON_DIR = os.path.join(ROOT, "design", "png", "icons")
THUMB_DIR = os.path.join(ROOT, "design", "png", "thumbs")
OUT = os.path.join(ROOT, "Main", "ui_assets.h")

# (name, size) pairs the firmware draws.
ICONS = [
    ("wifi", 18), ("wifi", 20),
    ("system", 20), ("skin", 20), ("speed", 20), ("odo", 20), ("lang", 20),
    ("reset", 20), ("warn", 20),
    ("check", 18), ("check", 20),
    ("sun", 16), ("sun", 22),
]
SKINS = ["halo", "pure", "chrono", "apex"]
THEMES = ["dark", "light"]

# ---------------------------------------------------------------- vector icons
# Same recipes as ICONS{} in design/mockups.html, in a 20x20 design box,
# stroke width 1.9 with round caps/joins. Compass angles (0 = 12 o'clock, cw).
LW = 1.9


def _seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / l2))
    qx, qy = ax + dx * t, ay + dy * t
    return math.hypot(px - qx, py - qy)


def _polar(cx, cy, r, d):
    a = math.radians(d - 90)
    return cx + r * math.cos(a), cy + r * math.sin(a)


def _arc_dist(px, py, cx, cy, r, d0, d1):
    ang = math.degrees(math.atan2(px - cx, -(py - cy)))  # compass
    while ang < d0:
        ang += 360
    while ang > d0 + 360:
        ang -= 360
    if ang <= d1:
        return abs(math.hypot(px - cx, py - cy) - r)
    e0 = _polar(cx, cy, r, d0)
    e1 = _polar(cx, cy, r, d1)
    return min(math.hypot(px - e0[0], py - e0[1]), math.hypot(px - e1[0], py - e1[1]))


def _inside(name, x, y):
    """True if design-space point (x, y) is covered by icon `name`."""
    h = LW / 2
    if name == "wifi":
        return (math.hypot(x - 10, y - 15.2) <= 1.9 or _arc_dist(x, y, 10, 15.2, 5.4, -46, 46) <= h
                or _arc_dist(x, y, 10, 15.2, 10, -46, 46) <= h)
    if name == "check":
        pts = [(4, 10.5), (8.2, 14.6), (16, 6.4)]
        return any(_seg_dist(x, y, *pts[i], *pts[i + 1]) <= 1.2 for i in range(2))
    if name == "sun":
        if math.hypot(x - 10, y - 10) <= 3.6:
            return True
        for i in range(8):
            a = i * 45
            x0, y0 = _polar(10, 10, 6.3, a)
            x1, y1 = _polar(10, 10, 8.3, a)
            if _seg_dist(x, y, x0, y0, x1, y1) <= h:
                return True
        return False
    raise ValueError("no vector recipe for icon " + name)


def render_icon(name, size, ss=8):
    """Alpha 0..255 row-major list, size x size."""
    k = size / 20.0
    out = []
    for py in range(size):
        for px in range(size):
            hit = 0
            for sy in range(ss):
                for sx in range(ss):
                    x = (px + (sx + 0.5) / ss) / k
                    y = (py + (sy + 0.5) / ss) / k
                    hit += _inside(name, x, y)
            out.append(round(255 * hit / (ss * ss)))
    return out


def load_icon(name, size):
    path = os.path.join(ICON_DIR, f"icon_{name}_{size}.png")
    if os.path.exists(path):
        im = Image.open(path).convert("RGBA")
        assert im.size == (size, size), path
        return list(im.tobytes()[3::4]), "export"
    return render_icon(name, size), "vector"


def pack4(alpha, w, h):
    out = bytearray()
    for y in range(h):
        row = [min(15, (alpha[y * w + x] * 15 + 127) // 255) for x in range(w)]
        if w & 1:
            row.append(0)
        for i in range(0, len(row), 2):
            out.append((row[i] << 4) | row[i + 1])
    return bytes(out)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def fmt_bytes(data, per=24, indent="  "):
    return "\n".join(indent + ",".join(f"0x{b:02x}" for b in data[i:i + per]) + ","
                     for i in range(0, len(data), per))


def fmt_words(data, per=16, indent="  "):
    return "\n".join(indent + ",".join(f"0x{v:04x}" for v in data[i:i + per]) + ","
                     for i in range(0, len(data), per))


def check_vectors():
    for name in ("wifi", "check", "sun"):
        for size in (16, 20):
            ref = list(Image.open(os.path.join(ICON_DIR, f"icon_{name}_{size}.png")).convert("RGBA").tobytes()[3::4])
            got = render_icon(name, size)
            err = sum(abs(a - b) for a, b in zip(ref, got)) / len(ref)
            print(f"  vector {name:6} {size}px: mean |alpha diff| = {err:.1f} / 255")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.check:
        check_vectors()

    L = [
        "// ui_assets.h - GENERATED by tools/uiassets.py. DO NOT EDIT.",
        "// Icon masks (4-bit alpha for gfx::mask4) and skin thumbnails (RGB565 for",
        "// gfx::imageNative, native column order) converted from design/png/icons",
        "// and design/png/thumbs.",
        "// Include from ONE translation unit only (the arrays are defined here).",
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "namespace ui_assets {",
        "",
        "struct IconMask {",
        "  uint8_t w, h;",
        "  const uint8_t *a4;",
        "};",
        "",
    ]
    total = 0
    for name, size in ICONS:
        alpha, src = load_icon(name, size)
        data = pack4(alpha, size, size)
        total += len(data)
        L.append(f"// {name} {size}px ({src})")
        L.append(f"static const uint8_t ICON_{name.upper()}_{size}_A4[] PROGMEM = {{")
        L.append(fmt_bytes(data))
        L.append("};")
        L.append(f"static const IconMask ICON_{name.upper()}_{size} = {{{size}, {size}, ICON_{name.upper()}_{size}_A4}};")
        L.append("")

    L.append("constexpr int THUMB_W = 144, THUMB_H = 96;")
    L.append("")
    for skin in SKINS:
        for th in THEMES:
            im = Image.open(os.path.join(THUMB_DIR, f"skin_{skin}_{th}.png")).convert("RGB")
            assert im.size == (144, 96)
            raw = im.tobytes()
            # native framebuffer order for gfx::imageNative(): column by
            # column, each column from the bottom row up
            px = []
            for x in range(144):
                for y in range(95, -1, -1):
                    i = (y * 144 + x) * 3
                    px.append(rgb565(raw[i], raw[i + 1], raw[i + 2]))
            total += len(px) * 2
            L.append(f"static const uint16_t THUMB_{skin.upper()}_{th.upper()}[144 * 96] PROGMEM = {{")
            L.append(fmt_words(px))
            L.append("};")
            L.append("")
    L.append("// [skin][0 = dark, 1 = light], skin order HALO, PURE, CHRONO, APEX")
    L.append("static const uint16_t *const THUMBS[4][2] = {")
    for skin in SKINS:
        L.append(f"  {{THUMB_{skin.upper()}_DARK, THUMB_{skin.upper()}_LIGHT}},")
    L.append("};")
    L.append("")
    L.append("}  // namespace ui_assets")
    L.append("")
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L))
    print(f"wrote {os.path.relpath(OUT, ROOT)}: {total} bytes of asset data")
    return 0


if __name__ == "__main__":
    sys.exit(main())
