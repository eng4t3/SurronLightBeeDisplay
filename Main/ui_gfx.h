// ui_gfx.h - Drawing helpers for the 480x320 landscape dashboard.
//
// All coordinates are LANDSCAPE (x: 0..479 left->right, y: 0..319 top->bottom).
// We draw straight into the Arduino_Canvas framebuffer with setRotation(1),
// which Arduino_Canvas maps natively (x,y) -> panel (319-y, x). This is the
// same mapping as the JC3248W535EN library's drawFillRect(), but avoids the
// library's per-call transforms (whose drawLine/drawFillCircle are 1px off and
// whose round-rect clipping shifts shapes instead of clipping them).
//
// Only included from Main.ino (single translation unit), so everything here is
// file-static.
#ifndef WHEELIE_UI_GFX_H
#define WHEELIE_UI_GFX_H

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include "big_font.h"   // BigFont: digits '0'..'9' only

#define UI_W 480
#define UI_H 320

// RGB888 -> RGB565 at compile time.
#define C565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// --------------------------------------------------------------------------
// Palette
// --------------------------------------------------------------------------
struct UiPalette {
  uint16_t bg;          // screen background
  uint16_t surface;     // card fill
  uint16_t surface_hi;  // button / inactive control fill
  uint16_t surface_sel; // selected card fill
  uint16_t pressed;     // pressed control fill
  uint16_t border;      // card outline
  uint16_t sep;         // separator lines
  uint16_t track;       // unlit gauge / slider track
  uint16_t text;        // primary text
  uint16_t text_dim;    // secondary text
  uint16_t text_faint;  // hints
  uint16_t accent;      // cyan brand accent
  uint16_t green;
  uint16_t amber;
  uint16_t red;
  uint16_t purple;
  uint16_t on_accent;   // text drawn on accent / amber fills
  uint16_t danger_bg;   // destructive button fill
};

static const UiPalette PAL_DARK = {
  C565(8, 12, 22),     // bg
  C565(17, 24, 39),    // surface
  C565(28, 38, 58),    // surface_hi
  C565(12, 44, 60),    // surface_sel
  C565(45, 60, 88),    // pressed
  C565(40, 52, 74),    // border
  C565(32, 43, 62),    // sep
  C565(30, 40, 60),    // track
  C565(248, 250, 252), // text
  C565(160, 174, 194), // text_dim
  C565(96, 110, 134),  // text_faint
  C565(6, 182, 212),   // accent
  C565(16, 185, 129),  // green
  C565(245, 158, 11),  // amber
  C565(239, 68, 68),   // red
  C565(168, 85, 247),  // purple
  C565(4, 10, 20),     // on_accent
  C565(70, 18, 24),    // danger_bg
};

static const UiPalette PAL_LIGHT = {
  C565(236, 241, 247), // bg
  C565(255, 255, 255), // surface
  C565(222, 229, 238), // surface_hi
  C565(219, 241, 253), // surface_sel
  C565(196, 207, 222), // pressed
  C565(196, 207, 222), // border
  C565(222, 229, 238), // sep
  C565(210, 218, 230), // track
  C565(12, 18, 34),    // text
  C565(60, 74, 96),    // text_dim
  C565(130, 144, 166), // text_faint
  C565(2, 132, 199),   // accent
  C565(5, 150, 105),   // green
  C565(217, 119, 6),   // amber
  C565(220, 38, 38),   // red
  C565(147, 51, 234),  // purple
  C565(255, 255, 255), // on_accent
  C565(254, 215, 215), // danger_bg
};

static Arduino_GFX *ui_g = nullptr;       // set once the display is up
static const UiPalette *P = &PAL_DARK;    // active palette (per frame)

// --------------------------------------------------------------------------
// Geometry
// --------------------------------------------------------------------------
struct Rect {
  int16_t x, y, w, h;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  int cx() const { return x + w / 2; }
  int cy() const { return y + h / 2; }
  int right() const { return x + w; }
  int bottom() const { return y + h; }
};

enum UiAlign : uint8_t { AL_LEFT = 0, AL_CENTER = 1, AL_RIGHT = 2 };

static const float UI_DEG2RAD = 0.01745329252f;

// --------------------------------------------------------------------------
// Primitives
// --------------------------------------------------------------------------
static inline void uiFill(int x, int y, int w, int h, uint16_t c) {
  if (w > 0 && h > 0) ui_g->fillRect(x, y, w, h, c);
}
static inline void uiRRect(int x, int y, int w, int h, int r, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  int m = (w < h ? w : h) / 2;
  if (r > m) r = m;
  ui_g->fillRoundRect(x, y, w, h, r, c);
}
static inline void uiRRect(const Rect &rc, int r, uint16_t c) { uiRRect(rc.x, rc.y, rc.w, rc.h, r, c); }
static inline void uiRRectLine(int x, int y, int w, int h, int r, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  int m = (w < h ? w : h) / 2;
  if (r > m) r = m;
  ui_g->drawRoundRect(x, y, w, h, r, c);
}
static inline void uiRRectLine(const Rect &rc, int r, uint16_t c) { uiRRectLine(rc.x, rc.y, rc.w, rc.h, r, c); }
static inline void uiCircle(int x, int y, int r, uint16_t c) { if (r > 0) ui_g->fillCircle(x, y, r, c); }
static inline void uiHLine(int x, int y, int w, uint16_t c) { if (w > 0) ui_g->drawFastHLine(x, y, w, c); }
static inline void uiVLine(int x, int y, int h, uint16_t c) { if (h > 0) ui_g->drawFastVLine(x, y, h, c); }

// Thick line with round caps.
static inline void uiThickLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.5f) { uiCircle(lroundf(x0), lroundf(y0), (int)(w / 2), c); return; }
  float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
  int ax = lroundf(x0 + nx), ay = lroundf(y0 + ny);
  int bx = lroundf(x0 - nx), by = lroundf(y0 - ny);
  int cx = lroundf(x1 + nx), cy = lroundf(y1 + ny);
  int dx2 = lroundf(x1 - nx), dy2 = lroundf(y1 - ny);
  ui_g->fillTriangle(ax, ay, bx, by, cx, cy, c);
  ui_g->fillTriangle(bx, by, cx, cy, dx2, dy2, c);
  uiCircle(lroundf(x0), lroundf(y0), (int)(w / 2), c);
  uiCircle(lroundf(x1), lroundf(y1), (int)(w / 2), c);
}

// Filled annular band from angle a0 to a1 (degrees; 0 = +x, 90 = down, i.e.
// clockwise on screen). Built from quads of <= step degrees: at r=120 a 3 deg
// chord deviates from the true arc by 0.04 px, so it is visually identical to
// a real arc and far cheaper than overlapping circles or a bbox arc scan.
static inline void uiArc(int cx, int cy, int r_in, int r_out, float a0, float a1, uint16_t c, float step = 3.0f) {
  if (a1 - a0 < 0.2f) return;
  int n = (int)ceilf((a1 - a0) / step);
  if (n < 1) n = 1;
  float da = (a1 - a0) / n;
  float ca = cosf(a0 * UI_DEG2RAD), sa = sinf(a0 * UI_DEG2RAD);
  int ox0 = lroundf(cx + r_out * ca), oy0 = lroundf(cy + r_out * sa);
  int ix0 = lroundf(cx + r_in * ca), iy0 = lroundf(cy + r_in * sa);
  for (int i = 1; i <= n; i++) {
    float a = (a0 + da * i) * UI_DEG2RAD;
    ca = cosf(a); sa = sinf(a);
    int ox1 = lroundf(cx + r_out * ca), oy1 = lroundf(cy + r_out * sa);
    int ix1 = lroundf(cx + r_in * ca), iy1 = lroundf(cy + r_in * sa);
    ui_g->fillTriangle(ox0, oy0, ox1, oy1, ix0, iy0, c);
    ui_g->fillTriangle(ix0, iy0, ox1, oy1, ix1, iy1, c);
    ox0 = ox1; oy0 = oy1; ix0 = ix1; iy0 = iy1;
  }
}

static inline void uiPolar(int cx, int cy, float r, float deg, int &x, int &y) {
  x = lroundf(cx + r * cosf(deg * UI_DEG2RAD));
  y = lroundf(cy + r * sinf(deg * UI_DEG2RAD));
}

// Arc band with round end caps.
static inline void uiArcRound(int cx, int cy, int r_in, int r_out, float a0, float a1, uint16_t c) {
  if (a1 - a0 < 0.2f) return;
  uiArc(cx, cy, r_in, r_out, a0, a1, c);
  float rm = (r_in + r_out) * 0.5f;
  int cap = (r_out - r_in) / 2;
  int x, y;
  uiPolar(cx, cy, rm, a0, x, y); uiCircle(x, y, cap, c);
  uiPolar(cx, cy, rm, a1, x, y); uiCircle(x, y, cap, c);
}

// --------------------------------------------------------------------------
// Text. `y` is the TOP of capital letters (cap height is added to reach the
// baseline), matching the original printText() convention.
// --------------------------------------------------------------------------
static inline int uiCap(const GFXfont *f) {
  if (f == &FreeSans9pt7b || f == &FreeSansBold9pt7b) return 12;
  if (f == &FreeSans12pt7b || f == &FreeSansBold12pt7b) return 17;
  if (f == &FreeSansBold18pt7b) return 25;
  if (f == &FreeSansBold24pt7b) return 33;
  if (f == &BigFont) return 69;
  return 12;
}

static inline int uiGlyphAdvance(const GFXfont *f, uint8_t ch) {
  if (ch < f->first || ch > f->last) return -1;
  return f->glyph[ch - f->first].xAdvance;
}

// Advance width of a string (what the cursor moves), used for alignment.
static inline int uiTextW(const char *s, const GFXfont *f) {
  int w = 0;
  for (; *s; ++s) {
    int a = uiGlyphAdvance(f, (uint8_t)*s);
    if (a > 0) w += a;
  }
  return w;
}

static inline void uiText(const char *s, int x, int y, const GFXfont *f, uint16_t c, UiAlign al = AL_LEFT) {
  int w = (al == AL_LEFT) ? 0 : uiTextW(s, f);
  int sx = (al == AL_LEFT) ? x : (al == AL_CENTER ? x - w / 2 : x - w);
  ui_g->setFont(f);
  ui_g->setTextColor(c);
  ui_g->setCursor(sx, y + uiCap(f));
  ui_g->print(s);
}

// Same, but `ymid` is the vertical centre of the capitals.
static inline void uiTextMid(const char *s, int x, int ymid, const GFXfont *f, uint16_t c, UiAlign al = AL_LEFT) {
  uiText(s, x, ymid - uiCap(f) / 2, f, c, al);
}

// Copies `s` into `out`, cutting it with ".." so it fits `maxw` pixels.
static inline void uiFit(const char *s, int maxw, const GFXfont *f, char *out, size_t n) {
  strlcpy(out, s, n);
  if (uiTextW(out, f) <= maxw) return;
  size_t len = strlen(out);
  const int dots = uiTextW("..", f);
  while (len > 0) {
    out[--len] = 0;
    if (uiTextW(out, f) + dots <= maxw) break;
  }
  strlcat(out, "..", n);
}

// Big numeric text: digits come from BigFont (which only has '0'..'9'); every
// other character ('.', 'x', '%', ' ', letters) is drawn with `alt` on the same
// baseline, so values like "1.00x", "12345 KM" or "73%" render correctly.
static inline int uiBigW(const char *s, const GFXfont *alt) {
  int w = 0;
  for (; *s; ++s) {
    uint8_t ch = (uint8_t)*s;
    int a = (ch >= '0' && ch <= '9') ? uiGlyphAdvance(&BigFont, ch) : uiGlyphAdvance(alt, ch);
    if (a > 0) w += a;
  }
  return w;
}

static inline void uiBigText(const char *s, int x, int y, uint16_t c, UiAlign al = AL_CENTER,
                      const GFXfont *alt = &FreeSansBold24pt7b) {
  int w = uiBigW(s, alt);
  int cx = (al == AL_LEFT) ? x : (al == AL_CENTER ? x - w / 2 : x - w);
  const int baseline = y + uiCap(&BigFont);
  ui_g->setTextColor(c);
  for (; *s; ++s) {
    uint8_t ch = (uint8_t)*s;
    const GFXfont *f = (ch >= '0' && ch <= '9') ? &BigFont : alt;
    int a = uiGlyphAdvance(f, ch);
    if (a < 0) continue;
    ui_g->setFont(f);
    ui_g->setCursor(cx, baseline);
    ui_g->write(ch);
    cx += a;
  }
}

// --------------------------------------------------------------------------
// Widgets
// --------------------------------------------------------------------------
static const int UI_RADIUS = 12;

// Standard card: rounded surface with a 1px outline. Selected cards get a
// tinted fill and a 2px accent outline; pressed cards darken.
static inline void uiCard(const Rect &r, bool selected = false, bool pressed = false, int radius = UI_RADIUS) {
  uint16_t fill = pressed ? P->pressed : (selected ? P->surface_sel : P->surface);
  uiRRect(r, radius, fill);
  if (selected) {
    uiRRectLine(r, radius, P->accent);
    uiRRectLine(r.x + 1, r.y + 1, r.w - 2, r.h - 2, radius - 1, P->accent);
  } else {
    uiRRectLine(r, radius, P->border);
  }
}

// Push button with centred label.
static inline void uiButton(const Rect &r, const char *label, const GFXfont *f, bool pressed,
                     uint16_t fill, uint16_t text, int radius = 10) {
  uiRRect(r, radius, pressed ? P->pressed : fill);
  uiRRectLine(r, radius, pressed ? P->accent : P->border);
  uiTextMid(label, r.cx(), r.cy(), f, text, AL_CENTER);
}

// iOS-style switch, 60x30.
static inline void uiToggle(int x, int y, bool on, bool pressed) {
  const int w = 60, h = 30;
  uiRRect(x, y, w, h, h / 2, on ? P->accent : P->track);
  if (pressed) uiRRectLine(x - 2, y - 2, w + 4, h + 4, h / 2 + 2, P->accent);
  int kx = on ? x + w - h / 2 : x + h / 2;
  uiCircle(kx, y + h / 2, h / 2 - 4, on ? P->on_accent : P->text_dim);
}

// Radio indicator.
static inline void uiRadio(int cx, int cy, bool on) {
  uiCircle(cx, cy, 10, on ? P->accent : P->border);
  uiCircle(cx, cy, 8, on ? P->surface_sel : P->surface);
  if (on) uiCircle(cx, cy, 5, P->accent);
}

// Two-option segmented pill; `right_active` selects the right half.
static inline void uiSegmented(const Rect &r, const char *left, const char *right, bool right_active) {
  uiRRect(r, r.h / 2, P->surface_hi);
  int half = r.w / 2;
  Rect a = {(int16_t)(right_active ? r.x + half : r.x), r.y, (int16_t)half, r.h};
  uiRRect(a.x + 2, a.y + 2, a.w - 4, a.h - 4, (a.h - 4) / 2, P->accent);
  uiTextMid(left, r.x + half / 2, r.cy(), &FreeSansBold9pt7b, right_active ? P->text_dim : P->on_accent, AL_CENTER);
  uiTextMid(right, r.x + half + half / 2, r.cy(), &FreeSansBold9pt7b, right_active ? P->on_accent : P->text_dim, AL_CENTER);
}

// Page indicator: active page is a pill, others dots.
static inline void uiPageDots(int cx, int cy, int count, int active) {
  const int spacing = 16;
  int x = cx - (count - 1) * spacing / 2;
  for (int i = 0; i < count; i++, x += spacing) {
    if (i == active) uiRRect(x - 7, cy - 3, 14, 6, 3, P->accent);
    else uiCircle(x, cy, 3, P->text_faint);
  }
}

// WiFi glyph; (cx, by) is the centre of the dot at the bottom. scale 1 = 22x16px.
static inline void uiWifiIcon(int cx, int by, uint16_t c, float scale = 1.0f) {
  const float t = 2.6f * scale;
  for (int i = 1; i <= 3; i++) {
    float r = (4.5f * i + 1.0f) * scale;
    uiArc(cx, by, (int)lroundf(r - t / 2), (int)lroundf(r + t / 2), 225.0f, 315.0f, c, 6.0f);
  }
  uiCircle(cx, by, (int)lroundf(2.0f * scale), c);
}

// Horizontal progress bar.
static inline void uiProgress(const Rect &r, float pct, uint16_t fill) {
  if (pct < 0) pct = 0;
  if (pct > 1) pct = 1;
  uiRRect(r, r.h / 2, P->track);
  int w = (int)lroundf(r.w * pct);
  if (w > 0) uiRRect(r.x, r.y, w < r.h ? r.h : w, r.h, r.h / 2, fill);
}

#endif
