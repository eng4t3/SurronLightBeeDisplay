// gfx_test.cpp - TEMPORARY test cards / benchmarks for the gfx engine.
// Page 0: dashboard-style primitives (gradient arc + glow, dial, needles,
//         cards, icons). Page 1: every text font incl. Hungarian accents,
//         tracking, alignment, tabular digits. Page 2: hero digits,
//         gradients, alpha, clipping, images, line fan, strokes.
#include "gfx_test.h"

#include <esp_timer.h>

#include "fonts.h"
#include "gfx.h"

using namespace gfx;

namespace gfxtest {
namespace {

const uint16_t BG0 = hex(0x0b0f16);
const uint16_t CARD_T = hex(0x1c2636);
const uint16_t CARD_B = hex(0x121925);
const uint16_t CARD_LINE = hex(0x2b394d);
const uint16_t TXT = hex(0xf1f5f9);
const uint16_t TXT_DIM = hex(0x94a3b8);
const uint16_t TXT_FAINT = hex(0x5b6b82);
const uint16_t TRACK = hex(0x1f2937);
const uint16_t ACCENT = hex(0x22d3ee);
const uint16_t GREEN = hex(0x22c55e);
const uint16_t AMBER = hex(0xf59e0b);
const uint16_t RED = hex(0xef4444);

const Stop SPEED_STOPS[] = {{0.0f, hex(0x10b981)}, {0.5f, hex(0xfacc15)}, {0.8f, hex(0xf97316)}, {1.0f, hex(0xef4444)}};

void card(int x, int y, int w, int h, const char *label) {
  fillRoundRectGradient(x, y, w, h, 14, CARD_T, CARD_B);
  strokeRoundRect(x, y, w, h, 14, 1, CARD_LINE);
  if (label) text(x + 14, y + 12, label, F_LABEL, TXT_DIM, TOP, 255, 2);
}

void gauge(float cx, float cy, float r, float t, float pct) {
  const float a0 = 135.0f, a1 = 135.0f + 270.0f * pct;
  // ticks
  for (int i = 0; i <= 20; i++) {
    const float a = 135.0f + 270.0f * i / 20.0f;
    const bool major = (i % 2) == 0;
    float x0, y0, x1, y1;
    polar(cx, cy, r - t - (major ? 14.0f : 10.0f), a, x0, y0);
    polar(cx, cy, r - t - 5.0f, a, x1, y1);
    line(x0, y0, x1, y1, major ? 2.5f : 1.5f, a <= a1 ? TXT_DIM : TXT_FAINT);
  }
  arc(cx, cy, r, t, a0, 405.0f, TRACK, true);
  const uint16_t tip = gradientAt(SPEED_STOPS, 4, pct);
  arcGlow(cx, cy, r, t, a0, a1, tip, 12.0f, 70);
  arcGradient(cx, cy, r, t, a0, a1, SPEED_STOPS, 4, 135.0f, 405.0f, true);
  float tx, ty;
  polar(cx, cy, r - t * 0.5f, a1, tx, ty);
  glow(tx, ty, t * 1.6f, tip, 170);
  fillCircle(tx, ty, t * 0.22f, WHITE);
}

void dial(float cx, float cy, float r, float pct) {
  arc(cx, cy, r, 5, 135, 405, TRACK, true);
  arc(cx, cy, r, 5, 135, 135 + 270 * pct, ACCENT, true);
  for (int i = 0; i <= 10; i++) {
    float x0, y0, x1, y1;
    const float a = 135.0f + 27.0f * i;
    polar(cx, cy, r - 9, a, x0, y0);
    polar(cx, cy, r - 14, a, x1, y1);
    line(x0, y0, x1, y1, 1.5f, TXT_DIM);
  }
  needle(cx, cy, 135 + 270 * pct, -8, r - 10, 5, 1.5f, RED);
  fillCircle(cx, cy, 6, hex(0x334155));
  strokeCircle(cx, cy, 6, 1.5f, TXT_DIM);
}

void page0() {
  radialGradient(0, 0, W, H, 170, 165, 360, hex(0x1a2332), hex(0x05070b));
  // hero gauge
  const float cx = 165, cy = 172, pct = 0.66f;
  gauge(cx, cy, 148, 20, pct);
  text((int)cx, 178, "57", F_NUM_XL, TXT, CENTER | MIDDLE | TABULAR);
  text((int)cx, 250, "KM/H", F_LABEL, TXT_DIM, CENTER | BASELINE, 255, 3);
  // orientation / color markers in the corners
  fillRect(0, 0, 10, 10, rgb(255, 0, 0));
  fillRect(W - 10, 0, 10, 10, rgb(0, 255, 0));
  fillRect(0, H - 10, 10, 10, rgb(0, 0, 255));
  fillRect(W - 10, H - 10, 10, 10, WHITE);
  text(14, 1, "R", F_LABEL, rgb(255, 80, 80), TOP);
  text(W - 14, 1, "G", F_LABEL, rgb(80, 255, 80), TOP | RIGHT);
  text(14, H - 2, "B", F_LABEL, rgb(120, 120, 255), BASELINE);

  // right column cards
  card(328, 14, 140, 74, "TRIP");
  int w = text(342, 76, "123.4", F_NUM_M, TXT, BASELINE | TABULAR);
  text(342 + w + 4, 76, "km", F_SMALL, TXT_DIM);

  card(328, 96, 140, 112, "G-FORCE");
  dial(398, 162, 42, 0.42f);
  text(398, 200, "0.42 G", F_SMALL, TXT, CENTER | BASELINE);

  card(328, 216, 140, 90, nullptr);
  const float iy0 = 240, iy1 = 282;
  icon::wifi(352, iy0, 26, ACCENT, 2, TXT_FAINT, true);
  icon::check(398, iy0, 26, GREEN);
  icon::gear(444, iy0, 26, TXT_DIM, CARD_B);
  icon::bolt(352, iy1, 26, AMBER);
  icon::flag(398, iy1, 26, TXT);
  icon::stopwatch(444, iy1, 26, TXT, CARD_B);

  // pills along the bottom-left
  fillRoundRect(14, 282, 70, 26, 13, ACCENT);
  text(49, 295, "ON", F_BODYB, BG0, CENTER | MIDDLE);
  fillRoundRect(92, 282, 90, 26, 13, TRACK, 200);
  strokeRoundRect(92, 282, 90, 26, 13, 1.5f, CARD_LINE);
  text(137, 295, "Beállítás", F_SMALL, TXT, CENTER | MIDDLE);
  icon::chevron(196, 295, 16, TXT_DIM, true);
}

void guide(int y) { hLine(0, y, W, hex(0xff00ff), 90); }

void page1() {
  clear(BG0);
  struct Row {
    const Font *f;
    const char *s;
    int tracking;
  };
  const Row rows[] = {
      {&F_LABEL, "F_LABEL 13 · SEBESSÉG · ÁRVÍZTŰRŐ TÜKÖRFÚRÓGÉP · 0-50", 2},
      {&F_SMALL, "F_SMALL 15 – Árvíztűrő tükörfúrógép … 23 °C · 12,5 km", 0},
      {&F_BODY, "F_BODY 18 – Árvíztűrő tükörfúrógép, gyorsulás 0.42 G", 0},
      {&F_BODYB, "F_BODYB 18 – ŐŰÖÜÓÁÉÍ őűöüóáéí Settings", 0},
      {&F_TITLE, "F_TITLE 24 Beállítások · Kilométer", 0},
      {&F_NUM_M, "F_NUM_M 1234.5 km/h Ő", 0},
  };
  int y = 4;
  for (const Row &r : rows) {
    const int base = y + r.f->ascent;
    guide(base);
    text(8, base, r.s, *r.f, TXT, BASELINE, 255, r.tracking);
    y += r.f->lineHeight + 2;
  }
  // F_NUM_L sample and tabular demo
  const int base = y + 42;
  guide(base);
  text(8, base, "0123456789.:-%+", F_NUM_L, ACCENT);
  // right-aligned numbers: proportional (top) vs TABULAR (bottom)
  const int bx = 470;
  text(bx, 250, "PROPORTIONAL", F_LABEL, TXT_DIM, RIGHT, 255, 1);
  text(bx, 290, "111", F_NUM_M, AMBER, RIGHT);
  text(bx - 70, 290, "888", F_NUM_M, AMBER, RIGHT);
  text(bx - 140, 250, "TABULAR", F_LABEL, TXT_DIM, RIGHT, 255, 1);
  text(bx - 140, 290, "111", F_NUM_M, GREEN, RIGHT | TABULAR);
  text(bx - 210, 290, "888", F_NUM_M, GREEN, RIGHT | TABULAR);
  // alignment guides: LEFT / CENTER / RIGHT around x = 120, TOP/MIDDLE
  vLine(120, 236, 80, hex(0xff00ff), 160);
  text(120, 250, "left", F_SMALL, TXT, LEFT);
  text(120, 270, "center", F_SMALL, TXT, CENTER);
  text(120, 290, "right", F_SMALL, TXT, RIGHT);
  hLine(10, 305, 60, hex(0xff00ff), 160);
  text(12, 305, "TOP", F_LABEL, TXT, TOP, 255, 1);
  text(50, 305, "MID", F_LABEL, TXT_DIM, MIDDLE, 255, 1);
  // alpha text
  text(236, 312, "alpha 50%", F_SMALL, TXT, CENTER | BOTTOM, 128);
}

// small generated assets for image565 / mask4
uint16_t s_img[32 * 32];
uint8_t s_mask[24 * 12];
bool s_assets = false;

void makeAssets() {
  if (s_assets) return;
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) s_img[y * 32 + x] = ((x / 8 + y / 8) & 1) ? hsv(x * 11.0f, 220, 255) : 0xF81F;
  for (int y = 0; y < 24; y++)
    for (int x = 0; x < 24; x++) {
      const float dx = x + 0.5f - 12, dy = y + 0.5f - 12;
      const float d = sqrtf(dx * dx + dy * dy);
      float a = 1.0f - fabsf(d - 8.0f) / 3.0f;
      a = a < 0 ? 0 : a;
      const uint8_t n = (uint8_t)(a * 15.0f + 0.5f);
      uint8_t &b = s_mask[y * 12 + x / 2];
      b = (x & 1) ? (uint8_t)((b & 0xF0) | n) : (uint8_t)((b & 0x0F) | (n << 4));
    }
  s_assets = true;
}

void page2() {
  makeAssets();
  clear(BG0);
  // hero digits on a gradient panel
  fillRoundRectGradient(6, 6, 222, 196, 18, hex(0x243044), hex(0x0d131c));
  text(117, 104, "88", F_NUM_XXL, TXT, CENTER | MIDDLE | TABULAR);
  text(117, 280, "57", F_NUM_XL, ACCENT, CENTER | BASELINE | TABULAR);
  // gradients
  hGradient(236, 8, 236, 22, hex(0x000000), hex(0x2563eb));
  vGradient(236, 36, 40, 110, hex(0x0f172a), hex(0x64748b));
  hGradient(284, 36, 188, 22, hex(0x10b981), hex(0xef4444));
  // alpha: three overlapping circles
  fillCircle(310, 100, 26, RED, 170);
  fillCircle(338, 100, 26, GREEN, 170);
  fillCircle(324, 124, 26, hex(0x3b82f6), 170);
  // clipping: a circle clipped to a rect
  strokeRoundRect(372, 66, 100, 80, 0, 1, TXT_FAINT);
  clip(372, 66, 100, 80);
  fillCircle(422, 106, 50, AMBER);
  fillCircle(422, 106, 30, BG0);
  clipReset();
  // images
  image565(236, 156, 32, 32, s_img, 0xF81F);
  mask4(276, 160, 24, 24, s_mask, ACCENT);
  fillRectAlpha(236, 172, 70, 10, WHITE, 100);
  // line fan (AA check) with widths 1, 2, 4
  const float fx = 400, fy = 240;
  for (int i = 0; i < 12; i++) {
    const float a = -90.0f + i * 15.0f;
    float x1, y1;
    polar(fx, fy, 66, a - 90.0f, x1, y1);
    const float wdt = i < 4 ? 1.0f : (i < 8 ? 2.0f : 4.0f);
    line(fx, fy, x1, y1, wdt, TXT);
  }
  // strokes of increasing thickness
  strokeCircle(260, 240, 20, 1, TXT);
  strokeCircle(260, 240, 14, 2, ACCENT);
  strokeCircle(260, 240, 8, 4, AMBER);
  strokeRoundRect(290, 216, 52, 48, 10, 1, TXT);
  strokeRoundRect(296, 222, 40, 36, 8, 3, GREEN);
  // triangle + polygon
  fillTriangle(236, 310, 262, 272, 288, 310, RED);
  const float hexagon[12] = {320, 290, 332, 270, 356, 270, 368, 290, 356, 310, 332, 310};
  fillPolygon(hexagon, 6, hex(0x8b5cf6));
  // needles at several angles
  for (int i = 0; i < 4; i++) needle(440, 300, 200.0f + i * 30.0f, -6, 40, 6, 1.5f, i == 3 ? RED : TXT_DIM);
}

// Arc torture test: spans / caps / thickness / seams.
void page3() {
  clear(BG0);
  const float spans[6] = {30, 90, 179, 181, 270, 359};
  const float starts[6] = {-20, 300, 45, 100, 135, 10};
  for (int i = 0; i < 6; i++) {
    const float cx = 42 + i * 79;
    arc(cx, 44, 34, 9, starts[i], starts[i] + spans[i], ACCENT, false);             // flat caps
    arc(cx, 124, 34, 9, starts[i], starts[i] + spans[i], AMBER, true);              // round caps
    arcGradient(cx, 204, 34, 12, starts[i], starts[i] + spans[i], SPEED_STOPS, 4, starts[i], starts[i] + spans[i], true);
  }
  // thickness sweep and tiny arcs
  const float th[5] = {4, 8, 14, 20, 28};
  for (int i = 0; i < 5; i++) arc(30 + i * 62, 282, 30, th[i], 180, 360, TXT, true);
  arc(350, 282, 8, 3, 0, 300, GREEN, true);
  arc(380, 282, 12, 4, 200, 520, RED, false);
  // full-ring gradient (seam at 3 o'clock) and two flat arcs sharing an edge
  const Stop rainbow[3] = {{0.0f, hex(0x3b82f6)}, {0.5f, hex(0xf43f5e)}, {1.0f, hex(0x3b82f6)}};
  arcGradient(438, 282, 30, 10, 0, 360, rainbow, 3, 0, 360);
  arc(438, 282, 16, 8, 0, 120, GREEN);
  arc(438, 282, 16, 8, 120, 240, AMBER);
  arc(438, 282, 16, 8, 240, 360, RED);
  text(4, 316, "flat / round / gradient caps, spans 30 90 179 181 270 359", F_LABEL, TXT_FAINT, BOTTOM);
}

}  // namespace

void draw(int page) {
  if (!framebuffer()) return;
  clipReset();
  setAlpha(255);
  switch (page) {
    case 1: page1(); break;
    case 2: page2(); break;
    case 3: page3(); break;
    default: page0(); break;
  }
}

namespace {
template <class F>
void timeIt(Print &out, const char *name, int reps, F fn) {
  const int64_t t0 = esp_timer_get_time();
  for (int i = 0; i < reps; i++) fn();
  const int64_t us = (esp_timer_get_time() - t0) / reps;
  out.printf("bench %-28s %7lld us\n", name, (long long)us);
}
}  // namespace

void bench(Print &out) {
  if (!framebuffer()) return;
  clipReset();
  setAlpha(255);
  timeIt(out, "clear", 10, [] { clear(BG0); });
  timeIt(out, "fillRect 480x320 (full)", 10, [] { fillRect(0, 0, W, H, CARD_B); });
  timeIt(out, "vGradient 480x320", 5, [] { vGradient(0, 0, W, H, CARD_T, BG0); });
  timeIt(out, "radialGradient 480x320", 5, [] { radialGradient(0, 0, W, H, 170, 165, 360, hex(0x1a2332), hex(0x05070b)); });
  bgCapture();
  timeIt(out, "bgRestore (memcpy PSRAM)", 10, [] { bgRestore(); });
  timeIt(out, "fillRoundRect 140x70 r14", 20, [] { fillRoundRect(300, 100, 140, 70, 14, CARD_T); });
  timeIt(out, "fillRoundRectGradient 140x70", 20, [] { fillRoundRectGradient(300, 100, 140, 70, 14, CARD_T, CARD_B); });
  timeIt(out, "strokeRoundRect 140x70 t1", 20, [] { strokeRoundRect(300, 100, 140, 70, 14, 1, CARD_LINE); });
  timeIt(out, "fillCircle r60", 20, [] { fillCircle(240, 160, 60, ACCENT); });
  timeIt(out, "strokeCircle r100 t2", 20, [] { strokeCircle(240, 160, 100, 2, ACCENT); });
  timeIt(out, "arc r150 t28 270deg", 10, [] { arc(240, 165, 150, 28, 135, 405, TRACK, true); });
  timeIt(out, "arc r150 t28 360deg", 10, [] { arc(240, 165, 150, 28, 0, 360, TRACK); });
  timeIt(out, "arcGradient r150 t28 270deg", 10, [] { arcGradient(240, 165, 150, 28, 135, 405, SPEED_STOPS, 4, 135, 405, true); });
  timeIt(out, "arcGradient r150 t28 180deg", 10, [] { arcGradient(240, 165, 150, 28, 135, 315, SPEED_STOPS, 4, 135, 405, true); });
  timeIt(out, "arcGlow r150 t28 180deg s12", 5, [] { arcGlow(240, 165, 150, 28, 135, 315, RED, 12, 80); });
  timeIt(out, "glow r40", 20, [] { glow(240, 160, 40, RED, 160); });
  {
    uint16_t *L = layerCreate();
    if (L) {
      drawTo(L);
      clear(BG0);
      arcGlow(240, 165, 150, 28, 135, 405, RED, 12, 80);
      arcGradient(240, 165, 150, 28, 135, 405, SPEED_STOPS, 4, 135, 405, true);
      drawTo(nullptr);
      timeIt(out, "arcCopy (layer) r162 t52 270deg", 10, [L] { arcCopy(L, 240, 165, 162, 52, 123, 417, true); });
      timeIt(out, "arcCopy (layer) r162 t52 180deg", 10, [L] { arcCopy(L, 240, 165, 162, 52, 123, 315, true); });
      timeIt(out, "copyRect 140x70 (layer)", 20, [L] { copyRect(L, 300, 100, 140, 70); });
      layerFree(L);
    }
  }
  timeIt(out, "line 120px w3", 50, [] { line(180, 100, 290, 150, 3, TXT); });
  timeIt(out, "needle 120px", 50, [] { needle(240, 165, 300, -14, 118, 7, 2, RED); });
  timeIt(out, "fillTriangle ~40px", 50, [] { fillTriangle(100, 100, 140, 110, 110, 140, RED); });
  timeIt(out, "text F_NUM_XL \"188\"", 20, [] { text(240, 160, "188", F_NUM_XL, TXT, CENTER | MIDDLE | TABULAR); });
  timeIt(out, "text F_NUM_XXL \"88\"", 20, [] { text(240, 160, "88", F_NUM_XXL, TXT, CENTER | MIDDLE); });
  timeIt(out, "text F_NUM_M \"1234.5\"", 20, [] { text(20, 100, "1234.5", F_NUM_M, TXT); });
  timeIt(out, "text F_BODY 30 chars", 20, [] { text(20, 200, "Árvíztűrő tükörfúrógép 12.5 km", F_BODY, TXT); });
  timeIt(out, "text F_LABEL 8 chars trk2", 50, [] { text(20, 250, "SEBESSÉG", F_LABEL, TXT, LEFT, 255, 2); });
  timeIt(out, "icon::wifi 26", 20, [] { icon::wifi(50, 50, 26, ACCENT); });
  timeIt(out, "test card page 0 (full frame)", 5, [] { draw(0); });
  timeIt(out, "test card page 1 (full frame)", 5, [] { draw(1); });
  timeIt(out, "test card page 2 (full frame)", 5, [] { draw(2); });
}

}  // namespace gfxtest
