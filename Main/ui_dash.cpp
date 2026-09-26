// ui_dash.cpp - dashboard skins HALO / PURE / CHRONO / APEX (DESIGN_SPEC §4).
// Coordinates are the generated spec tables (from design/mockups.html).
#include <math.h>

#include "ui_draw.h"

namespace ui {
namespace {

// ============================================================================
// Shared bits
// ============================================================================
struct Vals {
  char max[8], ride[12], trip[16], odo[20];
  const char *spdUnit;  // KM/H | MPH (under the speed)
  const char *vUnit;    // km/h | mph
  const char *dUnit;    // km | mi
};

void values(const View &v, Vals &o) {
  long mx = lroundf(v.max_speed);
  if (mx < 0) mx = 0;
  if (mx > 99) mx = 99;
  snprintf(o.max, sizeof(o.max), "%ld", mx);
  fmtRide(o.ride, sizeof(o.ride), v.ride_s);
  if (v.trip < 1000.0) fmtGrouped(o.trip, sizeof(o.trip), v.trip, 1);  // "18.4" (truncated)
  else fmtGrouped(o.trip, sizeof(o.trip), v.trip, 0);
  fmtGrouped(o.odo, sizeof(o.odo), v.odo, 0);
  o.spdUnit = v.imperial ? "MPH" : "KM/H";
  o.vUnit = v.imperial ? "mph" : "km/h";
  o.dUnit = v.imperial ? "mi" : "km";
}

// Label of a hold-to-reset stat: accent + ring while held, green RESET for
// 800 ms after a reset, otherwise `c`.
struct Lbl {
  const char *text;
  uint16_t color;
  float hold;  // < 0: no ring
};
Lbl resettable(const View &v, const char *label, float hold, uint32_t resetAt, uint16_t c) {
  if (resetFlash(v, resetAt)) return {TR(v, "RESET", "NULLÁZVA"), T->green, -1.0f};
  return {label, c, hold};
}
inline uint16_t valColor(const Lbl &l) { return l.hold >= 0.0f ? T->text_dim : T->text_hi; }

void spdText(char *b, size_t n, int spd) { snprintf(b, n, "%d", spd < 0 ? 0 : (spd > 99 ? 99 : spd)); }

inline float frac(float v, float fs) { return clampf(v / fs, 0.0f, 1.0f); }

// ============================================================================
// 1. HALO - 270° ring gauge + spec-sheet column
// ============================================================================
constexpr float HX = 150, HY = 176, HR = 118, HTH = 18, HA0 = -135, HSW = 270;
constexpr int COLX = 306, ROWY = 38, ROWH = 70;

void haloStatic(const View &v) {
  gfx::fillRect(0, 0, gfx::W, gfx::H, T->bg);
  const int fs = (int)v.full_scale;  // 85 km/h or 50 mph, ticks every 5
  for (int k = 0; k <= fs; k += 5) {
    const float d = HA0 + HSW * k / v.full_scale;
    const bool major = k % 10 == 0;
    float x0, y0, x1, y1;
    cpolar(HX, HY, major ? 132 : 137, d, x0, y0);
    cpolar(HX, HY, 142, d, x1, y1);
    capsule(x0, y0, x1, y1, major ? 2.0f : 1.5f, major ? T->text_dim : T->text_faint);
  }
  arcC(HX, HY, HR, HTH, HA0, HA0 + HSW, T->track);
  ring(HX, HY, 99, 1, T->border);
  for (int i = 1; i < 4; i++) gfx::hLine(COLX, ROWY + i * ROWH, 464 - COLX, T->border);
}

void drawHalo(const View &v, const Anim &a) {
  if (uint16_t *L = staticLayer(staticKey(1, v), haloStatic, v)) gfx::copyRect(L, 0, 0, gfx::W, gfx::H);
  else haloStatic(v);

  const float pct = frac(a.gauge, v.full_scale);
  const float dv = HA0 + HSW * pct;
  gfx::Stop zs[ZONE_STOPS];
  zoneStops(*T, zs);
  arcGradC(HX, HY, HR, HTH, HA0, dv, zs, ZONE_STOPS, HA0, HA0 + HSW);
  float tx, ty;
  cpolar(HX, HY, HR, dv, tx, ty);
  if (pct > 0.0f) glow(tx, ty, 34, zoneColor(*T, pct), 0.55f);
  gfx::fillCircle(tx, ty, 4.5f, T->text_hi);

  // Session-max tell-tale pointing at the ring
  const float dm = HA0 + HSW * frac(v.max_speed, v.full_scale);
  float m[6];
  cpolar(HX, HY, 106, dm, m[0], m[1]);
  cpolar(HX, HY, 98, dm - 3.6f, m[2], m[3]);
  cpolar(HX, HY, 98, dm + 3.6f, m[4], m[5]);
  poly(m, 3, T->amber);

  Vals s;
  values(v, s);
  char b[8];
  spdText(b, sizeof(b), a.spd);
  text(b, NUM_XL, HX, 211, T->text_hi, AL_CENTER);
  text(s.spdUnit, LABEL, HX, 243, T->text_dim, AL_CENTER);

  // Spec sheet: MAX / RIDE TIME / TRIP / ODO
  const Lbl lmax = resettable(v, "MAX", a.holdMax, v.max_reset_ms, T->text_dim);
  const Lbl ltrip = resettable(v, TR(v, "TRIP", "ÚT"), a.holdTrip, v.trip_reset_ms, T->text_dim);
  struct Row {
    Lbl l;
    const char *val, *unit;
  } rows[4] = {
      {lmax, s.max, s.vUnit},
      {{TR(v, "RIDE TIME", "MENETIDŐ"), T->text_dim, -1.0f}, s.ride, ""},
      {ltrip, s.trip, s.dUnit},
      {{TR(v, "ODO", "ÖSSZES"), T->text_dim, -1.0f}, s.odo, s.dUnit},
  };
  for (int i = 0; i < 4; i++) {
    const int y = ROWY + i * ROWH;
    const Row &r = rows[i];
    const TextBox tb = statLabel(r.l.text, COLX, y + 26, AL_LEFT, r.l.hold, r.l.color);
    if (i == 0 && r.l.hold < 0.0f) {  // amber max marker after the label (hidden while holding)
      const float t = tb.w;
      const float tri[6] = {COLX + t + 8, (float)(y + 17), COLX + t + 16, (float)(y + 17), COLX + t + 12,
                            (float)(y + 24)};
      poly(tri, 3, T->amber);
    }
    valUnit(r.val, NUM_M, r.unit, SMALL, COLX, y + 61, AL_LEFT, valColor(r.l), T->text_dim);
  }
}

// ============================================================================
// 2. PURE - hero number, zone bar, one stats row
// ============================================================================
constexpr float BAR_X = 28, BAR_W = 424, BAR_Y = 248, BAR_TH = 6;

// Straight part of the zone-gradient bar, columns [x0, x1), rows BAR_Y +- 3.
// The gradient is anchored to the full bar; bands of equal colour are flat
// fills, ramps use the dithered hGradient.
void gradientBar(float x0, float x1) {
  gfx::Stop zs[ZONE_STOPS];
  zoneStops(*T, zs);
  const int ya = (int)(BAR_Y - BAR_TH / 2), h = (int)BAR_TH;
  for (int i = 0; i + 1 < ZONE_STOPS; i++) {
    const float s0 = BAR_X + zs[i].pos * BAR_W, s1 = BAR_X + zs[i + 1].pos * BAR_W;
    const int a = jsRound(fmaxf(s0, x0)), b = jsRound(fminf(s1, x1));
    if (b <= a) continue;
    if (zs[i].color == zs[i + 1].color) {
      gfx::fillRect(a, ya, b - a, h, zs[i].color);
    } else {
      const uint16_t ca = gfx::gradientAt(zs, ZONE_STOPS, (a + 0.5f - BAR_X) / BAR_W);
      const uint16_t cb = gfx::gradientAt(zs, ZONE_STOPS, (b - 0.5f - BAR_X) / BAR_W);
      gfx::hGradient(a, ya, b - a, h, ca, cb);
    }
  }
}

void drawPure(const View &v, const Anim &a) {
  gfx::fillRect(0, 0, gfx::W, gfx::H, T->bg);
  Vals s;
  values(v, s);
  char b[8];
  spdText(b, sizeof(b), a.spd);
  text(b, NUM_XXL, 240, 196, T->text_hi, AL_CENTER);
  text(s.spdUnit, LABEL, 240, 226, T->text_dim, AL_CENTER);

  const float pct = frac(a.gauge, v.full_scale);
  hcapsule(BAR_X, BAR_Y, BAR_W, BAR_TH, T->track);
  const float fw = fmaxf(BAR_TH, BAR_W * pct);
  const float tipX = BAR_X + fw - BAR_TH / 2;
  gfx::fillCircle(BAR_X + BAR_TH / 2, BAR_Y, BAR_TH / 2, T->accent);  // left cap
  gradientBar(BAR_X + BAR_TH / 2, tipX);                             // right cap is under the tip dot
  if (pct > 0.0f) glow(tipX, BAR_Y, 22, zoneColor(*T, pct), 0.6f);
  gfx::fillCircle(tipX, BAR_Y, 5, T->text_hi);
  const float mx = BAR_X + BAR_W * frac(v.max_speed, v.full_scale);
  capsule(mx, BAR_Y - 8, mx, BAR_Y + 8, 2, T->amber);

  const Lbl lmax = resettable(v, "MAX", a.holdMax, v.max_reset_ms, T->text_dim);
  const Lbl ltrip = resettable(v, TR(v, "TRIP", "ÚT"), a.holdTrip, v.trip_reset_ms, T->text_dim);
  struct Col {
    Lbl l;
    const char *val, *unit;
  } cols[4] = {
      {lmax, s.max, ""},
      {{TR(v, "RIDE", "IDŐ"), T->text_dim, -1.0f}, s.ride, ""},
      {ltrip, s.trip, s.dUnit},
      {{"ODO", T->text_dim, -1.0f}, s.odo, s.dUnit},
  };
  constexpr float CW = 448.0f / 4;
  for (int i = 0; i < 4; i++) {
    const float x = 16 + CW * i + CW / 2;
    if (i) gfx::vLine((int)(16 + CW * i), 272, 34, T->border);
    statLabel(cols[i].l.text, x, 280, AL_CENTER, cols[i].l.hold, cols[i].l.color);
    valUnit(cols[i].val, NUM_M, cols[i].unit, SMALL, x, 312, AL_CENTER, valColor(cols[i].l), T->text_dim);
  }
}

// ============================================================================
// 3. CHRONO - watch dial (static dial pre-rendered into a layer)
// ============================================================================
constexpr float CX = 240, CY = 175, CA0 = -135, CSW = 270;

void chronoStatic(const View &v) {
  gfx::fillRect(0, 0, gfx::W, gfx::H, T->bg);
  const float fs = v.full_scale;
  auto ang = [&](float k) { return CA0 + CSW * k / fs; };
  // Brushed-steel bezel: angle gradient with highlights at -45° and +135°.
  const gfx::Stop bez[7] = {{0.0f, T->metal_lo},   {0.125f, T->metal_hi}, {0.3f, T->metal_lo},
                            {0.5f, T->metal_lo},   {0.625f, T->metal_hi}, {0.8f, T->metal_lo},
                            {1.0f, T->metal_lo}};
  arcGradC(CX, CY, 139, 8, 0, 359.99f, bez, 7, -90, 270, false);
  gfx::fillCircle(CX, CY, 135, T->face);
  ring(CX, CY, 131, 1, T->border);
  arcC(CX, CY, 129, 3, ang(fs * 0.60f), ang(fs * 0.85f), T->amber, false);
  arcC(CX, CY, 129, 3, ang(fs * 0.85f), ang(fs), T->red, false);
  // Minute track: every unit, longer every 5 (multiples of 10 get an index).
  const int ifs = (int)fs;
  for (int k = 0; k <= ifs; k++) {
    if (k % 10 == 0) continue;
    const bool five = k % 5 == 0;
    float x0, y0, x1, y1;
    cpolar(CX, CY, five ? 116 : 121, ang(k), x0, y0);
    cpolar(CX, CY, 126, ang(k), x1, y1);
    capsule(x0, y0, x1, y1, five ? 2.0f : 1.0f, five ? T->text_dim : T->text_faint);
  }
  // Applied baton indices with lume lines, numerals every 10.
  char b[4];
  for (int k = 0; k <= ifs; k += 10) {
    const float d = ang(k);
    float p[8];
    cpolar(CX, CY, 124, d - 1.7f, p[0], p[1]);
    cpolar(CX, CY, 124, d + 1.7f, p[2], p[3]);
    cpolar(CX, CY, 99, d + 2.0f, p[4], p[5]);
    cpolar(CX, CY, 99, d - 2.0f, p[6], p[7]);
    poly(p, 4, T->text_hi);
    float lx, ly, lx2, ly2;
    cpolar(CX, CY, 120, d, lx, ly);
    cpolar(CX, CY, 104, d, lx2, ly2);
    capsule(lx, ly, lx2, ly2, 1.5f, T->accent, pctA(0.9f));
    float nx, ny;
    cpolar(CX, CY, 83, d, nx, ny);
    snprintf(b, sizeof(b), "%d", k);
    text(b, BODYB, (float)jsRound(nx), jsRound(ny + 6), T->text, AL_CENTER);
  }
  text("SUR-RON", LABEL, CX, 146, T->text_dim, AL_CENTER, 3.0f);
  card(196, 205, 88, 54, 10, T->bg, T->border);
  text(v.imperial ? "MPH" : "KM/H", LABEL, CX, 280, T->text_dim, AL_CENTER);
}

void needlePoly(float *p, float ox, float oy, float dv) {
  cpolar(CX + ox, CY + oy, 127, dv, p[0], p[1]);
  cpolar(CX + ox, CY + oy, 10, dv + 22, p[2], p[3]);
  cpolar(CX + ox, CY + oy, 24, dv + 180 - 8, p[4], p[5]);
  cpolar(CX + ox, CY + oy, 24, dv + 180 + 8, p[6], p[7]);
  cpolar(CX + ox, CY + oy, 10, dv - 22, p[8], p[9]);
}

void drawChrono(const View &v, const Anim &a) {
  if (uint16_t *L = staticLayer(staticKey(3, v), chronoStatic, v)) gfx::copyRect(L, 0, 0, gfx::W, gfx::H);
  else chronoStatic(v);

  char b[8];
  spdText(b, sizeof(b), a.spd);
  text(b, NUM_L, CX, 252, T->text_hi, AL_CENTER);

  const float dm = CA0 + CSW * frac(v.max_speed, v.full_scale);
  float m[6];
  cpolar(CX, CY, 112, dm, m[0], m[1]);
  cpolar(CX, CY, 104, dm - 3, m[2], m[3]);
  cpolar(CX, CY, 104, dm + 3, m[4], m[5]);
  poly(m, 3, T->amber);

  // Needle (spring value may overshoot slightly past the stops)
  const float dv = CA0 + CSW * clampf(a.needle / v.full_scale, -0.02f, 1.02f);
  float p[10];
  needlePoly(p, 2, 4, dv);
  poly(p, 5, T->shadow, T->dark ? pctA(0.55f) : pctA(0.22f));
  needlePoly(p, 0, 0, dv);
  poly(p, 5, T->accent);
  gfx::fillCircle(CX, CY, 10, T->metal_lo);
  ring(CX, CY, 9, 2, T->metal_hi);
  gfx::fillCircle(CX, CY, 3.5f, T->accent);

  // Data in the four corners
  Vals s;
  values(v, s);
  const Lbl ltrip = resettable(v, TR(v, "TRIP", "ÚT"), a.holdTrip, v.trip_reset_ms, T->text_dim);
  const Lbl lmax = resettable(v, "MAX", a.holdMax, v.max_reset_ms, T->text_dim);
  statLabel(TR(v, "RIDE", "IDŐ"), 16, 60, AL_LEFT, -1.0f, T->text_dim);
  valUnit(s.ride, NUM_M, "", SMALL, 16, 94, AL_LEFT, T->text_hi, T->text_dim);
  statLabel(ltrip.text, 464, 60, AL_RIGHT, ltrip.hold, ltrip.color);
  valUnit(s.trip, NUM_M, s.dUnit, SMALL, 464, 94, AL_RIGHT, valColor(ltrip), T->text_dim);
  statLabel(lmax.text, 16, 272, AL_LEFT, lmax.hold, lmax.color);
  valUnit(s.max, NUM_M, s.vUnit, SMALL, 16, 306, AL_LEFT, valColor(lmax), T->text_dim);
  statLabel("ODO", 464, 272, AL_RIGHT, -1.0f, T->text_dim);
  valUnit(s.odo, NUM_M, s.dUnit, SMALL, 464, 306, AL_RIGHT, T->text_hi, T->text_dim);
}

// ============================================================================
// 4. APEX - track telemetry HUD
// ============================================================================
constexpr float GCX = 360, GCY = 176, GR = 56, GTH = 10;
constexpr int SPARK_X = 272, SPARK_W = 176, SPARK_Y = 222, SPARK_H = 30;
constexpr float G_FS = 0.60f;  // G gauge full scale

// Segment i of the 32-segment bar (skewed parallelogram).
void segPoly(float *p, int i) {
  const float x = 16 + i * 14;
  const float v[8] = {x + 3, 38, x + 14, 38, x + 11, 50, x, 50};
  memcpy(p, v, sizeof(v));
}
constexpr int SEG_Y = 36, SEG_H = 16;  // rows covering the bar incl. AA

void apexStatic(const View &v) {
  gfx::fillRect(0, 0, gfx::W, gfx::H, T->bg);
  float p[8];
  for (int i = 0; i < 32; i++) {  // unlit bar
    segPoly(p, i);
    poly(p, 4, T->track);
  }
  // Scale numerals under the segment bar
  char b[6];
  const int step = v.imperial ? 10 : 20;
  for (int k = 0; k <= (int)v.full_scale; k += step) {
    const float x = 16 + (k / v.full_scale) * 448;
    snprintf(b, sizeof(b), "%d", k);
    const Align al = k == 0 ? AL_LEFT : (x + 8 > 464 ? AL_RIGHT : AL_CENTER);
    text(b, LABEL, (float)jsRound(k ? (al == AL_RIGHT ? 464 : x) : x + 1), 70, T->text_faint, al, 0.5f);
  }
  chamferPanel(16, 196, 228, 42, 10, T->surface, T->border);
  chamferPanel(256, 78, 208, 158, 14, T->surface, T->border);
  text(TR(v, "LONG. G", "GYORS. G"), LABEL, 272, 100, T->text_dim);
  for (int k = 0; k <= 6; k++) {
    const float d = -90 + 180.0f * k / 6;
    const bool mj = k % 3 == 0;
    float x0, y0, x1, y1;
    cpolar(GCX, GCY, GR + 9, d, x0, y0);
    cpolar(GCX, GCY, GR + (mj ? 15 : 13), d, x1, y1);
    capsule(x0, y0, x1, y1, mj ? 2.0f : 1.5f, mj ? T->text_dim : T->text_faint);
  }
  arcC(GCX, GCY, GR, GTH, -90, 90, T->track);
  gfx::hLine(SPARK_X, SPARK_Y, SPARK_W, T->border);
  chamferPanel(16, 248, 448, 64, 12, T->surface, T->border);
  gfx::vLine(165, 260, 40, T->border);
  gfx::vLine(314, 260, 40, T->border);
}

// The same bar fully lit in zone colours: lit segments are copied from here.
// Segment i lies in columns [16 + 14i, 16 + 14(i+1)), so a column range copy
// reveals exactly the lit ones.
void apexLitStatic(const View &) {
  gfx::fillRect(0, SEG_Y, gfx::W, SEG_H, T->bg);
  float p[8];
  for (int i = 0; i < 32; i++) {
    segPoly(p, i);
    poly(p, 4, zoneColor(*T, (i + 0.5f) / 32));
  }
}

void sparkline(const View &v) {
  float px[G_HIST], py[G_HIST];
  for (int i = 0; i < G_HIST; i++) {
    px[i] = SPARK_X + (float)SPARK_W * i / (G_HIST - 1);
    py[i] = SPARK_Y - SPARK_H * (clampf(v.g_hist[i], 0.0f, G_FS) / G_FS);
  }
  // One pass per column: area under the trace (accent @16 %) and the 2 px
  // trace itself as a vertical AA span of height 2*sqrt(1 + slope^2) - far
  // cheaper than 39 capsules and seamless at the joints.
  const uint8_t A = pctA(0.16f);
  constexpr float DX = (float)SPARK_W / (G_HIST - 1);
  for (int x = SPARK_X; x < SPARK_X + SPARK_W; x++) {
    const float t = (x + 0.5f - SPARK_X) / DX;
    int i = (int)t;
    if (i > G_HIST - 2) i = G_HIST - 2;
    const float k = (py[i + 1] - py[i]) / DX;  // slope
    const float y = py[i] + (py[i + 1] - py[i]) * (t - i);
    const int yc = (int)ceilf(y);
    if (yc < SPARK_Y) gfx::fillRectAlpha(x, yc, 1, SPARK_Y - yc, T->accent, A);
    const float part = yc - y;
    if (part > 0.0f && yc - 1 < SPARK_Y) gfx::pixel(x, yc - 1, T->accent, (uint8_t)(A * part));
    const float hv = sqrtf(1.0f + k * k);  // half height of a 2 px wide line
    const float top = y - hv, bot = y + hv;
    const int ft = (int)floorf(top), fb = (int)floorf(bot);
    if (ft == fb) {
      gfx::pixel(x, ft, T->accent, (uint8_t)(255 * (bot - top)));
    } else {
      gfx::pixel(x, ft, T->accent, (uint8_t)(255 * (ft + 1 - top)));
      if (fb > ft + 1) gfx::fillRect(x, ft + 1, 1, fb - ft - 1, T->accent);
      gfx::pixel(x, fb, T->accent, (uint8_t)(255 * (bot - fb)));
    }
  }
  gfx::fillCircle(px[G_HIST - 1], py[G_HIST - 1], 3, T->text_hi);
}

void drawApex(const View &v, const Anim &a) {
  if (uint16_t *L = staticLayer(staticKey(4, v), apexStatic, v)) gfx::copyRect(L, 0, 0, gfx::W, gfx::H);
  else apexStatic(v);

  // 32-segment shift-light bar: lit segments are revealed from the lit layer.
  const float pct = frac(a.gauge, v.full_scale);
  const int lit = jsRound(pct * 32);
  if (lit > 0) {
    if (uint16_t *L = staticLayer(staticKey(5, v), apexLitStatic, v)) {
      gfx::copyRect(L, 16, SEG_Y, 14 * lit, SEG_H);
    } else {
      float p[8];
      for (int i = 0; i < lit; i++) {
        segPoly(p, i);
        poly(p, 4, zoneColor(*T, (i + 0.5f) / 32));
      }
    }
  }
  if (pct >= 0.97f) {  // shift light: the last 4 lit segments blink red / white at 6 Hz
    const uint16_t c = ((v.now_ms / 83) & 1) ? T->text_hi : T->red;
    float p[8];
    for (int i = lit - 4; i < lit; i++) {
      segPoly(p, i);
      gfx::fillRect(16 + 14 * i, SEG_Y, 14, SEG_H, T->bg);
      poly(p, 4, c);
    }
  }

  Vals s;
  values(v, s);
  char b[12];
  spdText(b, sizeof(b), a.spd);
  text(b, NUM_XL, 206, 176, T->text_hi, AL_RIGHT);
  text(s.spdUnit, LABEL, 216, 176, T->text_dim);

  // Peak chip
  const Lbl lpk = resettable(v, TR(v, "PEAK", "CSÚCS"), a.holdMax, v.max_reset_ms, T->amber);
  statLabel(lpk.text, 30, 222, AL_LEFT, lpk.hold, lpk.color);
  valUnit(s.max, TITLE, s.vUnit, SMALL, 230, 226, AL_RIGHT, valColor(lpk), T->text_dim);

  // Longitudinal G semicircle
  const float peak = clampf(v.peak_g, 0.0f, G_FS);
  snprintf(b, sizeof(b), "%s %.2f", TR(v, "PEAK", "CSÚCS"), peak);
  text(b, LABEL, 450, 100, T->amber, AL_RIGHT);
  const float g = clampf(a.g, 0.0f, G_FS);
  const float gp = g / G_FS;
  const gfx::Stop gs[4] = {{0.0f, T->accent}, {0.55f, T->accent}, {0.8f, T->amber}, {1.0f, T->amber}};
  arcGradC(GCX, GCY, GR, GTH, -90, -90 + 180 * gp, gs, 4, -90, 90);
  if (gp > 0.0f) {
    float tx, ty;
    cpolar(GCX, GCY, GR, -90 + 180 * gp, tx, ty);
    glow(tx, ty, 24, gp > 0.66f ? T->amber : T->accent, 0.5f);
    gfx::fillCircle(tx, ty, 3.5f, T->text_hi);
  }
  float kx, ky;
  cpolar(GCX, GCY, GR, -90 + 180 * (peak / G_FS), kx, ky);
  gfx::fillCircle(kx, ky, 3, T->amber);
  snprintf(b, sizeof(b), "%.2f", g);
  valUnit(b, NUM_M, "G", LABEL, GCX, GCY - 2, AL_CENTER, T->text_hi, T->text_dim, 4);
  sparkline(v);

  // Bottom strip: RIDE / TRIP / ODO
  const Lbl ltrip = resettable(v, TR(v, "TRIP", "ÚT"), a.holdTrip, v.trip_reset_ms, T->text_dim);
  statLabel(TR(v, "RIDE", "IDŐ"), 34, 270, AL_LEFT, -1.0f, T->text_dim);
  valUnit(s.ride, NUM_M, "", SMALL, 34, 302, AL_LEFT, T->text_hi, T->text_dim);
  statLabel(ltrip.text, 183, 270, AL_LEFT, ltrip.hold, ltrip.color);
  valUnit(s.trip, NUM_M, s.dUnit, SMALL, 183, 302, AL_LEFT, valColor(ltrip), T->text_dim);
  statLabel("ODO", 332, 270, AL_LEFT, -1.0f, T->text_dim);
  valUnit(s.odo, NUM_M, s.dUnit, SMALL, 332, 302, AL_LEFT, T->text_hi, T->text_dim);
}

}  // namespace

void drawDash(const View &v, const Anim &a) {
  switch (v.skin) {
    case SKIN_PURE: drawPure(v, a); break;
    case SKIN_CHRONO: drawChrono(v, a); break;
    case SKIN_APEX: drawApex(v, a); break;
    default: drawHalo(v, a); break;
  }
}

// Dashboard touch targets (DESIGN_SPEC §4, TOUCH rows).
bool dashTarget(const View &v, uint8_t t, Rect &r) {
  if (t == TGT_MAX_RESET) {
    switch (v.skin) {
      case SKIN_PURE: r = {16, 262, 112, 58}; return true;
      case SKIN_CHRONO: r = {0, 248, 100, 72}; return true;
      case SKIN_APEX: r = {16, 190, 228, 54}; return true;
      default: r = {298, 38, 182, 70}; return true;
    }
  }
  if (t == TGT_TRIP_RESET) {
    switch (v.skin) {
      case SKIN_PURE: r = {240, 262, 112, 58}; return true;
      case SKIN_CHRONO: r = {380, 36, 100, 72}; return true;
      case SKIN_APEX: r = {165, 248, 149, 64}; return true;
      default: r = {298, 178, 182, 70}; return true;
    }
  }
  return false;
}

}  // namespace ui
