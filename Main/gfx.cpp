// gfx.cpp - anti-aliased 2D engine. See gfx.h for conventions.
//
// Implementation notes
//  * Memory: landscape (x, y) -> native fb[x * H + (H - 1 - y)]. One landscape
//    column is a contiguous run of native memory (y increasing = address
//    decreasing), so every rasterizer loops columns outermost.
//  * Anti-aliasing: each shape has a signed distance function sd (px, > 0
//    outside). Pixel coverage = clamp(0.5 - sd, 0, 1) evaluated at the pixel
//    centre. Per column we compute analytically which rows are certainly
//    fully covered (filled as a span, no per-pixel math) and which rows may
//    be partially covered; only the latter evaluate the SDF.
//  * Blending: RGB565 fields are spread into a 32-bit word (0x07E0F81F mask)
//    so one multiply blends all three channels with 5-bit alpha.
//  * The sketch is built with -Os; the hot loops here want -O2.
#pragma GCC optimize("O2")

#include "gfx.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

namespace gfx {

// ============================================================================
// State & low level helpers
// ============================================================================
namespace {

uint16_t *s_fb = nullptr;      // current draw target (screen or a layer)
uint16_t *s_screen = nullptr;  // current screen back buffer
int s_cx0 = 0, s_cy0 = 0, s_cx1 = W, s_cy1 = H;  // clip, exclusive max
uint8_t s_alpha = 255;
uint16_t *s_bg = nullptr;
bool s_bgValid = false;

// Per-row gradient scratch (4 dither phases x H rows) shared by the gradient
// fills. Indexed by landscape y.
uint16_t s_rowLut[4][H];

constexpr uint32_t MASK = 0x07E0F81Fu;

inline uint32_t expand(uint16_t c) { return (c | ((uint32_t)c << 16)) & MASK; }
inline uint16_t pack(uint32_t x) { return (uint16_t)(x | (x >> 16)); }
// a32: 0..32. The per-channel bias (+16 in each field before >> 5) rounds to
// nearest instead of flooring: without it any non-zero alpha darkens a channel
// by one full RGB565 step when the foreground is darker (visible as a grey
// cast around soft glows on the light theme).
inline uint16_t blendX(uint32_t fx, uint16_t bg, uint32_t a32) {
  const uint32_t bx = expand(bg);
  return pack((((((fx - bx) * a32 + 0x02008010u) >> 5) + bx) & MASK));
}

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline float fminf_(float a, float b) { return a < b ? a : b; }
inline float fmaxf_(float a, float b) { return a > b ? a : b; }
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ---- Fast math. newlib's sqrtf/floorf/ceilf are out-of-line calls (sqrtf
// also does errno handling) and float division is a ROM soft routine on the
// ESP32-S3, so the per-pixel code uses these instead.
// 1/sqrt(x) for x > 0: bit hack + 2 Newton steps, rel. error ~5e-6.
inline float frsqrt(float x) {
  union {
    float f;
    uint32_t i;
  } u{x};
  u.i = 0x5f375a86u - (u.i >> 1);
  float y = u.f;
  const float hx = 0.5f * x;
  y = y * (1.5f - hx * y * y);
  y = y * (1.5f - hx * y * y);
  return y;
}
inline float fsqrt(float x) { return x > 0.0f ? x * frsqrt(x) : 0.0f; }
// 1/x for x > 0 with one Newton step (rel. error ~2e-3), for angles only.
inline float frcpFast(float x) {
  union {
    float f;
    uint32_t i;
  } u{x * x};
  u.i = 0x5f375a86u - (u.i >> 1);
  const float y = u.f;
  return y * (1.5f - 0.5f * x * x * y * y);
}
// Distance to a circle of radius r from the squared distance d2, without a
// sqrt: (d2 - r^2) / 2r differs from (d - r) by (d - r)^2 / 2r, i.e. < 0.07 px
// inside the 1 px AA band for r >= 2. Used for every curved AA edge.
inline int ifloor(float v) {
  const int i = (int)v;
  return i - (v < (float)i);
}
inline int iceil(float v) {
  const int i = (int)v;
  return i + (v > (float)i);
}

// Pointer to landscape pixel (x, y). y+1 is at p-1.
inline uint16_t *pix(int x, int y) { return s_fb + x * H + (H - 1 - y); }

inline uint8_t mul8(uint8_t a, uint8_t b) { return (uint8_t)(((uint32_t)a * b + 255) >> 8); }
inline uint8_t effAlpha(uint8_t a) { return s_alpha == 255 ? a : mul8(a, s_alpha); }

// 4x4 Bayer thresholds 0..15
const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// Fill n pixels starting at p (ascending addresses) with c.
inline void fill16(uint16_t *p, int n, uint16_t c) {
  if (n <= 0) return;
  if ((uintptr_t)p & 2) {
    *p++ = c;
    --n;
  }
  const uint32_t c2 = c | ((uint32_t)c << 16);
  uint32_t *q = (uint32_t *)p;
  int n2 = n >> 1;
  while (n2 >= 8) {
    q[0] = c2; q[1] = c2; q[2] = c2; q[3] = c2;
    q[4] = c2; q[5] = c2; q[6] = c2; q[7] = c2;
    q += 8;
    n2 -= 8;
  }
  while (n2--) *q++ = c2;
  if (n & 1) *(uint16_t *)q = c;
}

// Dithered RGB565 from 8.8 fixed-point channels (0..255.996) and a Bayer
// threshold 0..15.
inline uint16_t dither565(uint32_t r88, uint32_t g88, uint32_t b88, uint32_t th) {
  // Quantization step is 8 (R, B) or 4 (G) in 8-bit units: add th/16 of a step.
  uint32_t r = (r88 + (th << 7)) >> 11;  // (r88/256 + th*8/16) / 8
  uint32_t g = (g88 + (th << 6)) >> 10;  // (g88/256 + th*4/16) / 4
  uint32_t b = (b88 + (th << 7)) >> 11;
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Channel values in 8.8 fixed point WITHOUT bit replication, so an RGB565
// endpoint dithers back to exactly itself (no noise on flat gradients).
inline int32_t r88(uint16_t c) { return (int32_t)(c >> 11) << 11; }
inline int32_t g88(uint16_t c) { return (int32_t)((c >> 5) & 63) << 10; }
inline int32_t b88(uint16_t c) { return (int32_t)(c & 31) << 11; }

// Fill s_rowLut[phase][y0..y0+n) with a dithered ramp a->b (phase = x & 3).
void buildRamp(int y0, int n, uint16_t a, uint16_t b) {
  const int32_t ra = r88(a), ga = g88(a), ba = b88(a);
  const int32_t rb = r88(b), gb = g88(b), bb = b88(b);
  const int32_t den = n > 1 ? n - 1 : 1;
  for (int i = 0; i < n; i++) {
    const int y = y0 + i;
    if (y < 0 || y >= H) continue;
    const uint32_t r = (uint32_t)(ra + (rb - ra) * i / den);
    const uint32_t g = (uint32_t)(ga + (gb - ga) * i / den);
    const uint32_t bl = (uint32_t)(ba + (bb - ba) * i / den);
    for (int ph = 0; ph < 4; ph++) s_rowLut[ph][y] = dither565(r, g, bl, BAYER[y & 3][ph]);
  }
}

// ---------------------------------------------------------------------------
// Painters: how covered pixels get their color.
//   span(x, ya, yb)   rows [ya, yb) fully covered (already clipped)
//   px(x, y, cov)     one pixel with coverage 0..1 (already clipped)
// ---------------------------------------------------------------------------
struct PSolid {
  uint16_t c;
  uint32_t fx;
  float am;  // alpha in 0..32 units
  bool opaque;
  PSolid(uint16_t col, uint8_t alpha) : c(col), fx(expand(col)), am(alpha * (32.0f / 255.0f)), opaque(alpha == 255) {}
  inline void span(int x, int ya, int yb) const {
    uint16_t *p = pix(x, yb - 1);
    const int n = yb - ya;
    if (opaque) {
      fill16(p, n, c);
    } else {
      const uint32_t a = (uint32_t)(am + 0.5f);
      if (!a) return;
      for (int i = 0; i < n; i++) p[i] = blendX(fx, p[i], a);
    }
  }
  inline void px(int x, int y, float cov) const {
    const uint32_t a = (uint32_t)(cov * am + 0.5f);
    if (!a) return;
    uint16_t *p = pix(x, y);
    *p = a >= 32 ? c : blendX(fx, *p, a);
  }
};

// Color from s_rowLut (vertical gradient prepared by buildRamp).
struct PRows {
  float am;
  bool opaque;
  explicit PRows(uint8_t alpha) : am(alpha * (32.0f / 255.0f)), opaque(alpha == 255) {}
  inline void span(int x, int ya, int yb) const {
    const uint16_t *lut = s_rowLut[x & 3];
    uint16_t *p = pix(x, ya);
    if (opaque) {
      for (int y = ya; y < yb; y++) *p-- = lut[y];
    } else {
      const uint32_t a = (uint32_t)(am + 0.5f);
      for (int y = ya; y < yb; y++, p--) *p = blendX(expand(lut[y]), *p, a);
    }
  }
  inline void px(int x, int y, float cov) const {
    const uint32_t a = (uint32_t)(cov * am + 0.5f);
    if (!a) return;
    uint16_t *p = pix(x, y);
    const uint16_t c = s_rowLut[x & 3][y];
    *p = a >= 32 ? c : blendX(expand(c), *p, a);
  }
};

// Pixels copied from a layer with the same layout (arcCopy).
struct PCopy {
  const uint16_t *src;
  float am;
  bool opaque;
  PCopy(const uint16_t *layer, uint8_t alpha) : src(layer), am(alpha * (32.0f / 255.0f)), opaque(alpha == 255) {}
  inline void span(int x, int ya, int yb) const {
    uint16_t *p = pix(x, yb - 1);
    const uint16_t *q = src + (p - s_fb);
    const int n = yb - ya;
    if (opaque) {
      memcpy(p, q, (size_t)n * 2);
    } else {
      const uint32_t a = (uint32_t)(am + 0.5f);
      for (int i = 0; i < n; i++) p[i] = blendX(expand(q[i]), p[i], a);
    }
  }
  inline void px(int x, int y, float cov) const {
    const uint32_t a = (uint32_t)(cov * am + 0.5f);
    if (!a) return;
    uint16_t *p = pix(x, y);
    const uint16_t c = src[p - s_fb];
    *p = a >= 32 ? c : blendX(expand(c), *p, a);
  }
};

// Fast atan2 in degrees, (-180, 180], y down = clockwise. Max error ~0.01 deg.
inline float fastAtan2Deg(float y, float x) {
  const float ax = fabsf(x), ay = fabsf(y);
  const float mx = ax > ay ? ax : ay;
  if (mx == 0.0f) return 0.0f;
  const float mn = ax > ay ? ay : ax;
  const float a = mn * frcpFast(mx);
  const float s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = 3.14159274f - r;
  if (y < 0) r = -r;
  return r * 57.2957795f;
}

// Color from a 256-entry LUT indexed by angle around (cx, cy).
// Per-column cache of exact angle samples every `1 << sh` rows; pixels in
// between interpolate linearly (the angle is smooth along a column away from
// the centre: error < 0.2 deg for the radii that enable it). Shared by all
// gradient arcs (UI task only).
struct AngleCache {
  int32_t v[H / 4 + 3];  // sh >= 2
  uint16_t gen[H / 4 + 3];
  uint16_t curGen = 0;
  int col = -1;
};
AngleCache s_angCache;

struct PAngle {
  const uint16_t *lut;
  float cx, cy, ga0, gspan, gapHalf;
  float k;   // 255 / gspan
  float am;
  int sh;    // log2 of the sample spacing in rows; 0 = exact atan per pixel
  // LUT position (unclamped, 8.8 fixed point) of the pixel centre (x, y).
  inline int32_t pos88(int x, int y) const {
    float rel = fastAtan2Deg(y + 0.5f - cy, x + 0.5f - cx) - ga0;
    if (rel < -gapHalf) rel += 360.0f;
    if (rel >= gspan + gapHalf) rel -= 360.0f;
    return (int32_t)(rel * k * 256.0f);
  }
  inline int32_t sample(int x, int j) const {
    AngleCache &c = s_angCache;
    if (x != c.col) {
      c.col = x;
      if (++c.curGen == 0) {  // wrapped: invalidate everything
        memset(c.gen, 0, sizeof(c.gen));
        c.curGen = 1;
      }
    }
    if (c.gen[j] != c.curGen) {
      c.gen[j] = c.curGen;
      c.v[j] = pos88(x, j << sh);
    }
    return c.v[j];
  }
  inline int32_t posAt(int x, int y) const {
    if (!sh) return pos88(x, y);
    const int j = y >> sh, r = y & ((1 << sh) - 1);
    const int32_t a = sample(x, j);
    if (!r) return a;
    const int32_t d = sample(x, j + 1) - a;
    if (d > (40 << 8) || d < -(40 << 8)) return pos88(x, y);  // crossed the LUT seam
    return a + ((d * r) >> sh);
  }
  static inline int clampIdx(int32_t p88) {
    const int i = (p88 + 128) >> 8;
    return i < 0 ? 0 : (i > 255 ? 255 : i);
  }
  inline void put(uint16_t *p, uint16_t c, uint32_t a) const { *p = a >= 32 ? c : blendX(expand(c), *p, a); }
  inline void span(int x, int ya, int yb) const {
    uint16_t *p = pix(x, ya);
    const uint32_t a = (uint32_t)(am + 0.5f);
    if (!sh) {
      for (int y = ya; y < yb; y++, p--) put(p, lut[clampIdx(pos88(x, y))], a);
      return;
    }
    // Walk the sample grid: per block two cached samples, then an integer
    // ramp (q is scaled by 2^sh to keep the fraction).
    int y = ya;
    while (y < yb) {
      const int j = y >> sh;
      const int yEnd = imin(yb, (j + 1) << sh);
      const int32_t q0 = sample(x, j);
      const int32_t d = sample(x, j + 1) - q0;
      if (d > (40 << 8) || d < -(40 << 8)) {  // crossed the LUT seam: exact
        for (; y < yEnd; y++, p--) put(p, lut[clampIdx(pos88(x, y))], a);
        continue;
      }
      int32_t q = (q0 << sh) + d * (y - (j << sh));
      for (; y < yEnd; y++, p--, q += d) put(p, lut[clampIdx(q >> sh)], a);
    }
  }
  inline void px(int x, int y, float cov) const {
    const uint32_t a = (uint32_t)(cov * am + 0.5f);
    if (!a) return;
    uint16_t *p = pix(x, y);
    const uint16_t c = lut[clampIdx(posAt(x, y))];
    *p = a >= 32 ? c : blendX(expand(c), *p, a);
  }
};

// ---------------------------------------------------------------------------
// Rounded box SDF (covers rects, rounded rects, pills and circles).
// ---------------------------------------------------------------------------
struct RR {
  float cx, cy, hw, hh, r;  // centre, half extents, corner radius (<= min(hw, hh))
  float inv2r;              // 0.5 / r when r >= 2 (sqrt-free corner SDF), else 0
};

inline RR withInv(RR g) {
  g.inv2r = g.r >= 2.0f ? 0.5f / g.r : 0.0f;
  return g;
}

inline float rrSD(const RR &g, float px, float py) {
  const float qx = fabsf(px - g.cx) - (g.hw - g.r);
  const float qy = fabsf(py - g.cy) - (g.hh - g.r);
  if (qx > 0.0f && qy > 0.0f) {
    const float q2 = qx * qx + qy * qy;
    return g.inv2r > 0.0f ? (q2 - g.r * g.r) * g.inv2r : fsqrt(q2) - g.r;
  }
  return (qx > qy ? qx : qy) - g.r;
}

// Rows of column x (centre xc) that are non-zero [ya, yb) and fully covered
// [sa, sb) (sa == sb: none). Returns false if the column is empty.
inline bool rrColumn(const RR &g, float xc, int &ya, int &yb, int &sa, int &sb) {
  const float qx = fabsf(xc - g.cx) - (g.hw - g.r);
  if (qx >= g.r + 0.5f) return false;
  float E, S;
  if (qx <= 0.0f) {
    E = g.hh + 0.5f;
    S = (qx <= g.r - 0.5f) ? g.hh - 0.5f : -1.0f;
  } else {
    const float ro = g.r + 0.5f, ri = g.r - 0.5f;
    E = (g.hh - g.r) + fsqrt(ro * ro - qx * qx);
    S = (qx < ri) ? (g.hh - g.r) + fsqrt(ri * ri - qx * qx) : -1.0f;
  }
  ya = iceil(g.cy - E - 0.5f);
  yb = ifloor(g.cy + E - 0.5f) + 1;
  if (S > 0.0f) {
    sa = iceil(g.cy - S - 0.5f);
    sb = ifloor(g.cy + S - 0.5f) + 1;
    if (sa < ya) sa = ya;
    if (sb > yb) sb = yb;
    if (sb < sa) sb = sa;
  } else {
    sa = sb = yb;
  }
  return true;
}

inline bool colRange(float cx, float hw, int &x0, int &x1) {
  x0 = imax(ifloor(cx - hw - 0.5f), s_cx0);
  x1 = imin(iceil(cx + hw + 0.5f) + 1, s_cx1);
  return x0 < x1;
}

template <class P>
void rrFill(const RR &g, const P &paint) {
  int x0, x1;
  if (!s_fb || !colRange(g.cx, g.hw, x0, x1)) return;
  for (int x = x0; x < x1; x++) {
    const float xc = x + 0.5f;
    int ya, yb, sa, sb;
    if (!rrColumn(g, xc, ya, yb, sa, sb)) continue;
    ya = imax(ya, s_cy0);
    yb = imin(yb, s_cy1);
    if (ya >= yb) continue;
    sa = imin(imax(sa, ya), yb);
    sb = imin(imax(sb, sa), yb);
    for (int y = ya; y < sa; y++) {
      const float cov = 0.5f - rrSD(g, xc, y + 0.5f);
      if (cov > 0.0f) paint.px(x, y, cov > 1.0f ? 1.0f : cov);
    }
    if (sb > sa) paint.span(x, sa, sb);
    for (int y = sb; y < yb; y++) {
      const float cov = 0.5f - rrSD(g, xc, y + 0.5f);
      if (cov > 0.0f) paint.px(x, y, cov > 1.0f ? 1.0f : cov);
    }
  }
}

// Outer box minus inner box (inner must lie inside outer).
template <class P>
void rrStroke(const RR &o, const RR &in, const P &paint) {
  int x0, x1;
  if (!s_fb || !colRange(o.cx, o.hw, x0, x1)) return;
  for (int x = x0; x < x1; x++) {
    const float xc = x + 0.5f;
    int ya, yb, sa, sb;
    if (!rrColumn(o, xc, ya, yb, sa, sb)) continue;
    int ia, ib, isa, isb;
    if (!rrColumn(in, xc, ia, ib, isa, isb)) ia = ib = isa = isb = INT32_MIN / 2;
    const int y0 = imax(ya, s_cy0), y1 = imin(yb, s_cy1);
    int y = y0;
    while (y < y1) {
      if (y >= isa && y < isb) {  // inside the inner solid: hole
        y = isb;
        continue;
      }
      const bool oSolid = (y >= sa && y < sb);
      const bool iZero = (y < ia || y >= ib);
      if (oSolid && iZero) {
        int e = imin(sb, y1);
        if (y < ia) e = imin(e, ia);
        paint.span(x, y, e);
        y = e;
        continue;
      }
      const float yc = y + 0.5f;
      const float co = oSolid ? 1.0f : clamp01(0.5f - rrSD(o, xc, yc));
      const float ci = iZero ? 0.0f : clamp01(0.5f - rrSD(in, xc, yc));
      const float cov = co - ci;
      if (cov > 0.0f) paint.px(x, y, cov);
      ++y;
    }
  }
}

RR makeBox(int x, int y, int w, int h, float r) {
  RR g;
  g.hw = w * 0.5f;
  g.hh = h * 0.5f;
  g.cx = x + g.hw;
  g.cy = y + g.hh;
  const float m = g.hw < g.hh ? g.hw : g.hh;
  g.r = r < 0.0f ? 0.0f : (r > m ? m : r);
  return withInv(g);
}

RR insetBox(const RR &o, float t) {
  RR g = o;
  g.hw -= t;
  g.hh -= t;
  g.r = o.r - t;
  if (g.r < 0.0f) g.r = 0.0f;
  const float m = g.hw < g.hh ? g.hw : g.hh;
  if (g.r > m) g.r = m;
  return withInv(g);
}

// ---------------------------------------------------------------------------
// Arc SDF
// ---------------------------------------------------------------------------
struct ArcG {
  float cx, cy, rO, rI, hw;  // centre, outer/inner radius, half thickness
  float rO2, rI2, hw2;       // squares
  float i2rO, i2rI, i2hw;    // 0.5 / radius, 0 = use an exact sqrt (radius < 2)
  bool full, wide, round;
  float c0, s0, c1, s1;      // unit vectors at a0 / a1
  float ik0, ik1;            // 1 / c0 and -1 / c1 (0 if the slope is ~0)
  float p0x, p0y, p1x, p1y;  // cap centres (relative to centre)
  // bounding box (inclusive-exclusive pixel rows/cols)
  int bx0, by0, bx1, by1;
};

// Wedge distance: > 0 inside the angular range (px), for partial arcs.
inline float arcWedge(const ArcG &g, float dx, float dy) {
  const float e0 = g.c0 * dy - g.s0 * dx;  // > 0: clockwise after a0
  const float e1 = g.s1 * dx - g.c1 * dy;  // > 0: before a1
  return g.wide ? (e0 > e1 ? e0 : e1) : (e0 < e1 ? e0 : e1);
}

// Signed distance to a circle of radius r (r2 = r^2, i2r = 0.5/r or 0).
inline float circSD(float d2, float r, float r2, float i2r) {
  return i2r > 0.0f ? (d2 - r2) * i2r : fsqrt(d2) - r;
}

// Signed distance to the arc band (> 0 outside). d2 = dx^2 + dy^2.
inline float arcSD(const ArcG &g, float dx, float dy, float d2) {
  const float so = circSD(d2, g.rO, g.rO2, g.i2rO);
  const float sdR = g.rI > 0.0f ? fmaxf_(so, -circSD(d2, g.rI, g.rI2, g.i2rI)) : so;
  if (g.full) return sdR;
  const float w = arcWedge(g, dx, dy);
  if (g.round) {
    if (w >= 0.0f) return sdR;
    const float ax = dx - g.p0x, ay = dy - g.p0y, bx = dx - g.p1x, by = dy - g.p1y;
    return circSD(fminf_(ax * ax + ay * ay, bx * bx + by * by), g.hw, g.hw2, g.i2hw);
  }
  return fmaxf_(sdR, -w);
}

bool arcSetup(ArcG &g, float cx, float cy, float rOuter, float thickness, float a0, float a1,
              bool roundCaps, float pad) {
  if (!(rOuter > 0.0f) || !(thickness > 0.0f)) return false;
  if (thickness > rOuter) thickness = rOuter;
  if (a1 < a0) {
    const float t = a0;
    a0 = a1;
    a1 = t;
  }
  float span = a1 - a0;
  g.cx = cx;
  g.cy = cy;
  g.rO = rOuter;
  g.rI = rOuter - thickness;
  g.hw = thickness * 0.5f;
  g.rO2 = g.rO * g.rO;
  g.rI2 = g.rI * g.rI;
  g.hw2 = g.hw * g.hw;
  g.i2rO = g.rO >= 2.0f ? 0.5f / g.rO : 0.0f;
  g.i2rI = g.rI >= 2.0f ? 0.5f / g.rI : 0.0f;
  g.i2hw = g.hw >= 2.0f ? 0.5f / g.hw : 0.0f;
  g.round = roundCaps;
  g.full = span >= 360.0f;
  if (!g.full && span <= 0.0f && !roundCaps) return false;
  a0 = fmodf(a0, 360.0f);
  if (a0 < 0.0f) a0 += 360.0f;
  a1 = a0 + span;
  g.wide = span > 180.0f;
  const float d2r = 0.0174532925f;
  g.c0 = cosf(a0 * d2r);
  g.s0 = sinf(a0 * d2r);
  g.c1 = cosf(a1 * d2r);
  g.s1 = sinf(a1 * d2r);
  g.ik0 = fabsf(g.c0) > 1e-6f ? 1.0f / g.c0 : 0.0f;
  g.ik1 = fabsf(g.c1) > 1e-6f ? -1.0f / g.c1 : 0.0f;
  const float rm = rOuter - g.hw;
  g.p0x = rm * g.c0;
  g.p0y = rm * g.s0;
  g.p1x = rm * g.c1;
  g.p1y = rm * g.s1;

  // Bounding box
  float xmn, xmx, ymn, ymx;
  if (g.full) {
    xmn = -rOuter; xmx = rOuter; ymn = -rOuter; ymx = rOuter;
  } else {
    xmn = ymn = 1e9f;
    xmx = ymx = -1e9f;
    auto add = [&](float x, float y) {
      xmn = fminf_(xmn, x); xmx = fmaxf_(xmx, x);
      ymn = fminf_(ymn, y); ymx = fmaxf_(ymx, y);
    };
    if (roundCaps) {
      add(g.p0x - g.hw, g.p0y - g.hw); add(g.p0x + g.hw, g.p0y + g.hw);
      add(g.p1x - g.hw, g.p1y - g.hw); add(g.p1x + g.hw, g.p1y + g.hw);
    } else {
      add(rOuter * g.c0, rOuter * g.s0); add(g.rI * g.c0, g.rI * g.s0);
      add(rOuter * g.c1, rOuter * g.s1); add(g.rI * g.c1, g.rI * g.s1);
    }
    for (int k = 0; k < 8; k++) {  // cardinal extremes inside the sweep
      const float ang = k * 90.0f;
      if (ang >= a0 && ang <= a1) {
        const int q = k & 3;
        add(q == 0 ? rOuter : (q == 2 ? -rOuter : 0.0f), q == 1 ? rOuter : (q == 3 ? -rOuter : 0.0f));
      }
    }
  }
  g.bx0 = imax(ifloor(cx + xmn - pad - 0.5f), s_cx0);
  g.bx1 = imin(iceil(cx + xmx + pad + 0.5f) + 1, s_cx1);
  g.by0 = imax(ifloor(cy + ymn - pad - 0.5f), s_cy0);
  g.by1 = imin(iceil(cy + ymx + pad + 0.5f) + 1, s_cy1);
  return g.bx0 < g.bx1 && g.by0 < g.by1;
}

// Rows [lo, hi) where the linear function e(y) = ea + k (y - a) is >= t.
struct RowIv {
  int lo, hi;
};
constexpr int ROW_INF = 1 << 20;
inline RowIv rowsAtLeast(float ea, float k, float ik, int a, float t) {
  if (k > 1e-6f) return RowIv{a + iceil((t - ea) * ik), ROW_INF};
  if (k < -1e-6f) return RowIv{-ROW_INF, a + ifloor((t - ea) * ik) + 1};
  return ea >= t ? RowIv{-ROW_INF, ROW_INF} : RowIv{ROW_INF, ROW_INF};
}
// Rows where neither interval holds (both are half-lines, so this is one
// interval).
inline RowIv gapOf(const RowIv &p, const RowIv &q) {
  auto comp = [](const RowIv &r) {
    if (r.lo >= r.hi) return RowIv{-ROW_INF, ROW_INF};      // empty -> everything
    if (r.lo <= -ROW_INF && r.hi >= ROW_INF) return RowIv{ROW_INF, ROW_INF};  // all -> none
    if (r.lo <= -ROW_INF) return RowIv{r.hi, ROW_INF};
    return RowIv{-ROW_INF, r.lo};
  };
  const RowIv a = comp(p), b = comp(q);
  return RowIv{imax(a.lo, b.lo), imin(a.hi, b.hi)};
}

template <class P>
void arcRaster(const ArcG &g, const P &paint) {
  if (!s_fb) return;
  const float ro = g.rO + 0.5f, ros = g.rO - 0.5f;
  const float riz = g.rI - 0.5f, ris = g.rI + 0.5f;
  const float ro2 = ro * ro, ros2 = ros * ros, riz2 = riz * riz, ris2 = ris * ris;
  const bool hasInner = g.rI > 0.0f;

  for (int x = g.bx0; x < g.bx1; x++) {
    const float fx = x + 0.5f - g.cx;
    const float fx2 = fx * fx;
    if (fx2 >= ro2) continue;
    const float hO = fsqrt(ro2 - fx2);
    const float hOs = fx2 < ros2 && ros > 0.0f ? fsqrt(ros2 - fx2) : -1.0f;
    const float hIs = hasInner && fx2 < ris2 ? fsqrt(ris2 - fx2) : 0.0f;
    const float hIz = hasInner && riz > 0.0f && fx2 < riz2 ? fsqrt(riz2 - fx2) : -1.0f;

    // Row boundaries (see header comment): T0..T7
    int seg[8][2];
    uint8_t type[8];  // 0 edge, 1 solid
    int ns = 0;
    const float cy = g.cy;
    auto rowCeil = [&](float v) { return iceil(v - 0.5f); };
    auto rowEnd = [&](float v) { return ifloor(v - 0.5f) + 1; };
    const int T0 = rowCeil(cy - hO), T7 = rowEnd(cy + hO);
    if (hOs < 0.0f) {
      // No fully covered rows in this column
      if (hIz > 0.0f) {
        seg[ns][0] = T0; seg[ns][1] = rowEnd(cy - hIz); type[ns++] = 0;
        seg[ns][0] = rowCeil(cy + hIz); seg[ns][1] = T7; type[ns++] = 0;
      } else {
        seg[ns][0] = T0; seg[ns][1] = T7; type[ns++] = 0;
      }
    } else {
      const int T1 = rowCeil(cy - hOs), T6 = rowEnd(cy + hOs);
      if (hIs <= 0.0f) {
        seg[ns][0] = T0; seg[ns][1] = T1; type[ns++] = 0;
        seg[ns][0] = T1; seg[ns][1] = T6; type[ns++] = 1;
        seg[ns][0] = T6; seg[ns][1] = T7; type[ns++] = 0;
      } else {
        const int T2 = rowEnd(cy - hIs), T5 = rowCeil(cy + hIs);
        seg[ns][0] = T0; seg[ns][1] = T1; type[ns++] = 0;
        seg[ns][0] = T1; seg[ns][1] = T2; type[ns++] = 1;
        if (hIz > 0.0f) {
          seg[ns][0] = T2; seg[ns][1] = rowEnd(cy - hIz); type[ns++] = 0;
          seg[ns][0] = rowCeil(cy + hIz); seg[ns][1] = T5; type[ns++] = 0;
        } else {
          seg[ns][0] = T2; seg[ns][1] = T5; type[ns++] = 0;
        }
        seg[ns][0] = T5; seg[ns][1] = T6; type[ns++] = 1;
        seg[ns][0] = T6; seg[ns][1] = T7; type[ns++] = 0;
      }
    }

    int prevEnd = g.by0;
    for (int s = 0; s < ns; s++) {
      int a = imax(seg[s][0], prevEnd);
      const int b = imin(seg[s][1], g.by1);
      if (a >= b) continue;
      prevEnd = b;
      if (type[s] == 1) {
        if (g.full) {
          paint.span(x, a, b);
          continue;
        }
        // Radially solid: only the angular extent matters. Both wedge
        // distances are linear in y, so the rows that are fully inside
        // (span fill), possibly partial (per-pixel AA / round cap) or
        // certainly outside are found analytically per column.
        const float dy0 = a + 0.5f - cy;
        const float e0a = g.c0 * dy0 - g.s0 * fx, k0 = g.c0;   // e0(y) = e0a + k0 (y - a)
        const float e1a = g.s1 * fx - g.c1 * dy0, k1 = -g.c1;  // e1(y) = e1a + k1 (y - a)
        const float tFull = g.round ? 0.0f : 0.5f;
        const float tMaybe = g.round ? -(g.hw + 0.5f) : -0.5f;
        auto maybePx = [&](int y) {
          const float e0 = e0a + k0 * (y - a), e1 = e1a + k1 * (y - a);
          const float w = g.wide ? (e0 > e1 ? e0 : e1) : (e0 < e1 ? e0 : e1);
          if (w >= tFull) {
            paint.px(x, y, 1.0f);
          } else if (g.round) {
            const float dy = y + 0.5f - cy;
            const float ax = fx - g.p0x, ay = dy - g.p0y, bx = fx - g.p1x, by = dy - g.p1y;
            const float cov = 0.5f - circSD(fminf_(ax * ax + ay * ay, bx * bx + by * by), g.hw, g.hw2, g.i2hw);
            if (cov > 0.0f) paint.px(x, y, cov > 1.0f ? 1.0f : cov);
          } else if (w > -0.5f) {
            paint.px(x, y, w + 0.5f);
          }
        };
        const RowIv f0 = rowsAtLeast(e0a, k0, g.ik0, a, tFull), f1 = rowsAtLeast(e1a, k1, g.ik1, a, tFull);
        const RowIv m0 = rowsAtLeast(e0a, k0, g.ik0, a, tMaybe), m1 = rowsAtLeast(e1a, k1, g.ik1, a, tMaybe);
        if (!g.wide) {
          // inside = e0 >= t AND e1 >= t: one interval each for F and M (F within M)
          const int fl = imax(imax(f0.lo, f1.lo), a), fh = imin(imin(f0.hi, f1.hi), b);
          const int ml = imax(imax(m0.lo, m1.lo), a), mh = imin(imin(m0.hi, m1.hi), b);
          if (fl < fh) {
            for (int y = ml; y < fl; y++) maybePx(y);
            paint.span(x, fl, fh);
            for (int y = fh; y < mh; y++) maybePx(y);
          } else {
            for (int y = ml; y < mh; y++) maybePx(y);
          }
        } else {
          // inside = e0 >= t OR e1 >= t: the complement (gap) is one interval
          const RowIv gf = gapOf(f0, f1), gm = gapOf(m0, m1);
          const int gl = imax(gf.lo, a), gh = imin(gf.hi, b);
          if (gl >= gh) {
            paint.span(x, a, b);
          } else {
            if (a < gl) paint.span(x, a, gl);
            const int hl = imax(gm.lo, gl), hh = imin(gm.hi, gh);
            if (hl < hh) {
              for (int y = gl; y < hl; y++) maybePx(y);
              for (int y = hh; y < gh; y++) maybePx(y);
            } else {
              for (int y = gl; y < gh; y++) maybePx(y);
            }
            if (gh < b) paint.span(x, gh, b);
          }
        }
      } else {
        for (int y = a; y < b; y++) {
          const float dy = y + 0.5f - cy;
          const float cov = 0.5f - arcSD(g, fx, dy, fx2 + dy * dy);
          if (cov > 0.0f) paint.px(x, y, cov > 1.0f ? 1.0f : cov);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Uneven capsule SDF (line, needle). Segment A->B, radius ra at A, rb at B.
// ---------------------------------------------------------------------------
struct Caps {
  float ax, ay;    // start
  float ux, uy;    // unit direction A->B
  float h;         // length
  float ra, rb;
  float a, b;      // iq's uneven capsule terms
};

inline float capsSD(const Caps &c, float px, float py) {
  // local frame: v along the axis, u across (abs)
  const float dx = px - c.ax, dy = py - c.ay;
  const float v = dx * c.ux + dy * c.uy;
  const float u = fabsf(dx * c.uy - dy * c.ux);
  const float k = -c.b * u + c.a * v;
  if (k < 0.0f) return fsqrt(u * u + v * v) - c.ra;
  if (k > c.a * c.h) {
    const float w = v - c.h;
    return fsqrt(u * u + w * w) - c.rb;
  }
  return u * c.a + v * c.b - c.ra;
}

template <class P>
void capsRaster(float x0, float y0, float r0, float x1, float y1, float r1, const P &paint) {
  if (!s_fb) return;
  Caps c;
  const float dx = x1 - x0, dy = y1 - y0;
  const float len = fsqrt(dx * dx + dy * dy);
  const float rmax = r0 > r1 ? r0 : r1;
  if (len < 1e-3f || fabsf(r0 - r1) >= len) {  // degenerate: one disc
    RR g{};
    if (r0 >= r1) { g.cx = x0; g.cy = y0; } else { g.cx = x1; g.cy = y1; }
    g.hw = g.hh = g.r = rmax;
    rrFill(withInv(g), paint);
    return;
  }
  c.ax = x0; c.ay = y0;
  const float il = 1.0f / len;
  c.ux = dx * il; c.uy = dy * il;
  c.h = len; c.ra = r0; c.rb = r1;
  c.b = (r0 - r1) * il;
  c.a = fsqrt(1.0f - c.b * c.b);

  const float pad = rmax + 1.0f;
  int xa = imax(ifloor(fminf_(x0, x1) - pad), s_cx0);
  int xb = imin(iceil(fmaxf_(x0, x1) + pad) + 1, s_cx1);
  const float idx = fabsf(dx) < 1e-4f ? 0.0f : 1.0f / dx;
  for (int x = xa; x < xb; x++) {
    const float xc = x + 0.5f;
    // Rows: the part of the segment within |x - xc| <= pad, widened by pad.
    float t0, t1;
    if (fabsf(dx) < 1e-4f) {
      if (fabsf(x0 - xc) > pad) continue;
      t0 = 0.0f; t1 = 1.0f;
    } else {
      t0 = (xc - pad - x0) * idx;
      t1 = (xc + pad - x0) * idx;
      if (t0 > t1) { const float t = t0; t0 = t1; t1 = t; }
      if (t0 < 0.0f) t0 = 0.0f;
      if (t1 > 1.0f) t1 = 1.0f;
      if (t0 > t1) continue;
    }
    const float ya_f = y0 + dy * t0, yb_f = y0 + dy * t1;
    int ya = imax(ifloor(fminf_(ya_f, yb_f) - pad), s_cy0);
    int yb = imin(iceil(fmaxf_(ya_f, yb_f) + pad) + 1, s_cy1);
    for (int y = ya; y < yb; y++) {
      const float cov = 0.5f - capsSD(c, xc, y + 0.5f);
      if (cov > 0.0f) paint.px(x, y, cov > 1.0f ? 1.0f : cov);
    }
  }
}

}  // namespace

// ============================================================================
// Colors
// ============================================================================
uint16_t mix(uint16_t a, uint16_t b, uint8_t t) {
  const int r = red8(a) + ((red8(b) - red8(a)) * t + 127) / 255;
  const int g = green8(a) + ((green8(b) - green8(a)) * t + 127) / 255;
  const int bl = blue8(a) + ((blue8(b) - blue8(a)) * t + 127) / 255;
  return rgb((uint8_t)r, (uint8_t)g, (uint8_t)bl);
}

uint16_t lerp(uint16_t a, uint16_t b, float t) {
  return mix(a, b, (uint8_t)(clamp01(t) * 255.0f + 0.5f));
}

uint16_t hsv(float h, uint8_t s, uint8_t v) {
  h = fmodf(h, 360.0f);
  if (h < 0.0f) h += 360.0f;
  const float sf = s / 255.0f, vf = v / 255.0f;
  const float c = vf * sf;
  const float hp = h / 60.0f;
  const float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
  float r = 0, g = 0, b = 0;
  switch ((int)hp) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
  }
  const float m = vf - c;
  return rgb((uint8_t)((r + m) * 255.0f + 0.5f), (uint8_t)((g + m) * 255.0f + 0.5f),
             (uint8_t)((b + m) * 255.0f + 0.5f));
}

uint16_t scale(uint16_t c, uint16_t k) {
  if (k > 511) k = 511;
  const uint32_t r = (red8(c) * k) >> 8, g = (green8(c) * k) >> 8, b = (blue8(c) * k) >> 8;
  return rgb((uint8_t)(r > 255 ? 255 : r), (uint8_t)(g > 255 ? 255 : g), (uint8_t)(b > 255 ? 255 : b));
}

uint16_t gradientAt(const Stop *st, int n, float t) {
  if (n <= 0) return 0;
  if (t <= st[0].pos || n == 1) return st[0].color;
  for (int i = 1; i < n; i++) {
    if (t <= st[i].pos) {
      const float span = st[i].pos - st[i - 1].pos;
      const float u = span > 0.0f ? (t - st[i - 1].pos) / span : 1.0f;
      return lerp(st[i - 1].color, st[i].color, u);
    }
  }
  return st[n - 1].color;
}

// ============================================================================
// Setup / clip / alpha
// ============================================================================
void begin(uint16_t *fb) {
  s_fb = s_screen = fb;
  clipReset();
  s_alpha = 255;
}
uint16_t *framebuffer() { return s_screen; }
void setFramebuffer(uint16_t *fb) {
  if (s_fb == s_screen) s_fb = fb;  // keep an active drawTo(layer) redirect
  s_screen = fb;
}

// ============================================================================
// Layers
// ============================================================================
uint16_t *layerCreate() {
  return (uint16_t *)heap_caps_aligned_alloc(16, W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void layerFree(uint16_t *layer) {
  if (layer) heap_caps_free(layer);
}
void drawTo(uint16_t *layer) { s_fb = layer ? layer : s_screen; }
uint16_t *drawTarget() { return s_fb; }

void copyRect(const uint16_t *layer, int x, int y, int w, int h) {
  if (!s_fb || !layer || layer == s_fb) return;
  const int x0 = imax(x, s_cx0), x1 = imin(x + w, s_cx1);
  const int y0 = imax(y, s_cy0), y1 = imin(y + h, s_cy1);
  if (x0 >= x1 || y0 >= y1) return;
  if (y0 == 0 && y1 == H) {  // full-height columns are one contiguous block
    memcpy(pix(x0, H - 1), layer + x0 * H, (size_t)(x1 - x0) * H * 2);
    return;
  }
  for (int xx = x0; xx < x1; xx++) {
    const size_t off = (size_t)(pix(xx, y1 - 1) - s_fb);
    memcpy(s_fb + off, layer + off, (size_t)(y1 - y0) * 2);
  }
}

void clip(int x, int y, int w, int h) {
  s_cx0 = imax(x, 0);
  s_cy0 = imax(y, 0);
  s_cx1 = imin(x + w, W);
  s_cy1 = imin(y + h, H);
  if (s_cx1 < s_cx0) s_cx1 = s_cx0;
  if (s_cy1 < s_cy0) s_cy1 = s_cy0;
}
void clipReset() {
  s_cx0 = 0; s_cy0 = 0; s_cx1 = W; s_cy1 = H;
}
ClipRect getClip() {
  return ClipRect{(int16_t)s_cx0, (int16_t)s_cy0, (int16_t)s_cx1, (int16_t)s_cy1};
}
void setClip(const ClipRect &c) { clip(c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0); }
void setAlpha(uint8_t a) { s_alpha = a; }
uint8_t getAlpha() { return s_alpha; }

// ============================================================================
// Background
// ============================================================================
void clear(uint16_t c) {
  if (s_fb) fill16(s_fb, W * H, c);
}

bool bgCapture() {
  if (!s_fb) return false;
  if (!s_bg) s_bg = (uint16_t *)heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_bg) return false;
  memcpy(s_bg, s_fb, W * H * 2);
  s_bgValid = true;
  return true;
}

bool bgRestore() {
  if (!s_fb || !s_bg || !s_bgValid) return false;
  memcpy(s_fb, s_bg, W * H * 2);
  return true;
}

void bgFree() {
  if (s_bg) heap_caps_free(s_bg);
  s_bg = nullptr;
  s_bgValid = false;
}

// ============================================================================
// Rectangles & gradients
// ============================================================================
void pixel(int x, int y, uint16_t c, uint8_t alpha) {
  if (!s_fb || x < s_cx0 || x >= s_cx1 || y < s_cy0 || y >= s_cy1) return;
  PSolid(c, effAlpha(alpha)).px(x, y, 1.0f);
}

void fillRectAlpha(int x, int y, int w, int h, uint16_t c, uint8_t alpha) {
  if (!s_fb) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  const int x0 = imax(x, s_cx0), x1 = imin(x + w, s_cx1);
  const int y0 = imax(y, s_cy0), y1 = imin(y + h, s_cy1);
  if (x0 >= x1 || y0 >= y1) return;
  if (alpha == 255 && y0 == 0 && y1 == H) {  // full-height: one contiguous run
    fill16(pix(x0, H - 1), (x1 - x0) * H, c);
    return;
  }
  const PSolid p(c, alpha);
  for (int xx = x0; xx < x1; xx++) p.span(xx, y0, y1);
}

void fillRect(int x, int y, int w, int h, uint16_t c) { fillRectAlpha(x, y, w, h, c, 255); }
void hLine(int x, int y, int w, uint16_t c, uint8_t alpha) { fillRectAlpha(x, y, w, 1, c, alpha); }
void vLine(int x, int y, int h, uint16_t c, uint8_t alpha) { fillRectAlpha(x, y, 1, h, c, alpha); }

void vGradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom, uint8_t alpha) {
  if (!s_fb || h <= 0) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  const int x0 = imax(x, s_cx0), x1 = imin(x + w, s_cx1);
  const int y0 = imax(y, s_cy0), y1 = imin(y + h, s_cy1);
  if (x0 >= x1 || y0 >= y1) return;
  buildRamp(y, h, top, bottom);
  const PRows p(alpha);
  for (int xx = x0; xx < x1; xx++) p.span(xx, y0, y1);
}

void hGradient(int x, int y, int w, int h, uint16_t left, uint16_t right, uint8_t alpha) {
  if (!s_fb || w <= 0) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  const int x0 = imax(x, s_cx0), x1 = imin(x + w, s_cx1);
  const int y0 = imax(y, s_cy0), y1 = imin(y + h, s_cy1);
  if (x0 >= x1 || y0 >= y1) return;
  const int32_t ra = r88(left), ga = g88(left), ba = b88(left);
  const int32_t rb = r88(right), gb = g88(right), bb = b88(right);
  const int32_t den = w > 1 ? w - 1 : 1;
  const uint32_t a32 = (alpha * 32 + 127) / 255;
  for (int xx = x0; xx < x1; xx++) {
    const int i = xx - x;
    const uint32_t r = (uint32_t)(ra + (rb - ra) * i / den);
    const uint32_t g = (uint32_t)(ga + (gb - ga) * i / den);
    const uint32_t b = (uint32_t)(ba + (bb - ba) * i / den);
    uint16_t pat[4];
    for (int k = 0; k < 4; k++) pat[k] = dither565(r, g, b, BAYER[k][xx & 3]);
    uint16_t *p = pix(xx, y0);
    for (int yy = y0; yy < y1; yy++, p--) {
      const uint16_t c = pat[yy & 3];
      *p = alpha == 255 ? c : blendX(expand(c), *p, a32);
    }
  }
}

void radialGradient(int x, int y, int w, int h, float cx, float cy, float r, uint16_t inner,
                    uint16_t outer) {
  if (!s_fb || !(r > 0.0f)) return;
  const int x0 = imax(x, s_cx0), x1 = imin(x + w, s_cx1);
  const int y0 = imax(y, s_cy0), y1 = imin(y + h, s_cy1);
  if (x0 >= x1 || y0 >= y1) return;
  // Integer distances in 1/16 px. LUT indexed by d^2 >> sh (<= 1024
  // entries, t = sqrt of it baked in), 4 dither phases (2x2 Bayer).
  constexpr int N = 1024;
  static uint16_t lut[4][N];
  static uint16_t kIn = 0, kOut = 0;
  static int32_t kR2 = -1;
  const int32_t r16 = (int32_t)(r * 16.0f + 0.5f);
  const int32_t r2 = r16 * r16;
  int sh = 0;
  while ((r2 >> sh) >= N) sh++;
  const int nmax = r2 >> sh;
  if (kIn != inner || kOut != outer || kR2 != r2) {
    const int32_t ra = r88(inner), ga = g88(inner), ba = b88(inner);
    const int32_t rb = r88(outer), gb = g88(outer), bb = b88(outer);
    static const uint8_t th[4] = {0, 8, 12, 4};  // (x&1) | (y&1)<<1 -> 2x2 Bayer
    for (int i = 0; i <= nmax; i++) {
      const float t = sqrtf((float)((int64_t)i << sh) / (float)r2);
      const int32_t k = (int32_t)((t > 1.0f ? 1.0f : t) * 4096.0f);
      const uint32_t rr = (uint32_t)(ra + (((rb - ra) * k) >> 12));
      const uint32_t gg = (uint32_t)(ga + (((gb - ga) * k) >> 12));
      const uint32_t bb2 = (uint32_t)(ba + (((bb - ba) * k) >> 12));
      for (int ph = 0; ph < 4; ph++) lut[ph][i] = dither565(rr, gg, bb2, th[ph]);
    }
    kIn = inner;
    kOut = outer;
    kR2 = r2;
  }
  const uint8_t alpha = effAlpha(255);
  const uint32_t a32 = (alpha * 32 + 127) / 255;
  const int32_t cx16 = (int32_t)(cx * 16.0f), cy16 = (int32_t)(cy * 16.0f);
  for (int xx = x0; xx < x1; xx++) {
    const int32_t fx = xx * 16 + 8 - cx16;
    const int32_t dy = y0 * 16 + 8 - cy16;
    int32_t d2 = fx * fx + dy * dy;
    int32_t inc = 32 * dy + 256;  // (dy + 16)^2 - dy^2
    const uint16_t *la = lut[(xx & 1) | ((y0 & 1) << 1)];
    const uint16_t *lb = lut[(xx & 1) | (((y0 + 1) & 1) << 1)];
    uint16_t *p = pix(xx, y0);
    int n = y1 - y0;
    if (alpha == 255) {
      for (; n >= 2; n -= 2, p -= 2) {
        int i = d2 >> sh;
        p[0] = la[i > nmax ? nmax : i];
        d2 += inc;
        inc += 512;
        i = d2 >> sh;
        p[-1] = lb[i > nmax ? nmax : i];
        d2 += inc;
        inc += 512;
      }
      if (n) {
        const int i = d2 >> sh;
        p[0] = la[i > nmax ? nmax : i];
      }
    } else {
      for (int k = 0; k < n; k++, p--) {
        const int i = d2 >> sh;
        const uint16_t c = (k & 1 ? lb : la)[i > nmax ? nmax : i];
        *p = blendX(expand(c), *p, a32);
        d2 += inc;
        inc += 512;
      }
    }
  }
}

// ============================================================================
// Rounded rects & circles
// ============================================================================
void fillRoundRect(int x, int y, int w, int h, float r, uint16_t c, uint8_t alpha) {
  if (w <= 0 || h <= 0) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  rrFill(makeBox(x, y, w, h, r), PSolid(c, alpha));
}

void fillRoundRectGradient(int x, int y, int w, int h, float r, uint16_t top, uint16_t bottom,
                           uint8_t alpha) {
  if (w <= 0 || h <= 0) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  buildRamp(y, h, top, bottom);
  rrFill(makeBox(x, y, w, h, r), PRows(alpha));
}

void strokeRoundRect(int x, int y, int w, int h, float r, float t, uint16_t c, uint8_t alpha) {
  if (w <= 0 || h <= 0 || !(t > 0.0f)) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  const RR o = makeBox(x, y, w, h, r);
  const RR in = insetBox(o, t);
  if (in.hw <= 0.0f || in.hh <= 0.0f) {
    rrFill(o, PSolid(c, alpha));
    return;
  }
  rrStroke(o, in, PSolid(c, alpha));
}

void fillCircle(float cx, float cy, float r, uint16_t c, uint8_t alpha) {
  if (!(r > 0.0f)) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  const RR g = withInv(RR{cx, cy, r, r, r, 0.0f});
  rrFill(g, PSolid(c, alpha));
}

void strokeCircle(float cx, float cy, float r, float t, uint16_t c, uint8_t alpha) {
  if (!(r > 0.0f) || !(t > 0.0f)) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  const RR o = withInv(RR{cx, cy, r, r, r, 0.0f});
  if (t >= r) {
    rrFill(o, PSolid(c, alpha));
    return;
  }
  const float ri = r - t;
  const RR in = withInv(RR{cx, cy, ri, ri, ri, 0.0f});
  rrStroke(o, in, PSolid(c, alpha));
}

// ============================================================================
// Arcs
// ============================================================================
void arc(float cx, float cy, float rOuter, float thickness, float a0, float a1, uint16_t c,
         bool roundCaps, uint8_t alpha) {
  alpha = effAlpha(alpha);
  if (!alpha) return;
  ArcG g;
  if (!arcSetup(g, cx, cy, rOuter, thickness, a0, a1, roundCaps, 1.0f)) return;
  arcRaster(g, PSolid(c, alpha));
}

void arcCopy(const uint16_t *layer, float cx, float cy, float rOuter, float thickness, float a0,
             float a1, bool roundCaps, uint8_t alpha) {
  if (!layer || layer == s_fb) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  ArcG g;
  if (!arcSetup(g, cx, cy, rOuter, thickness, a0, a1, roundCaps, 1.0f)) return;
  arcRaster(g, PCopy(layer, alpha));
}

namespace {
uint16_t s_angleLut[256];

void arcGradientLut(float cx, float cy, float rOuter, float thickness, float a0, float a1,
                    float ga0, float ga1, bool roundCaps, uint8_t alpha) {
  alpha = effAlpha(alpha);
  if (!alpha) return;
  ArcG g;
  if (!arcSetup(g, cx, cy, rOuter, thickness, a0, a1, roundCaps, 1.0f)) return;
  if (ga1 < ga0) {
    const float t = ga0;
    ga0 = ga1;
    ga1 = t;
  }
  float gspan = ga1 - ga0;
  if (gspan < 0.01f) gspan = 0.01f;
  if (gspan > 360.0f) gspan = 360.0f;
  PAngle p;
  p.lut = s_angleLut;
  p.cx = cx;
  p.cy = cy;
  p.ga0 = ga0;
  p.gspan = gspan;
  p.gapHalf = (360.0f - gspan) * 0.5f;
  p.k = 255.0f / gspan;
  // Interpolate angles over 8 rows at large radii, 4 at medium, exact below.
  const float rIn = rOuter - thickness;
  p.sh = rIn >= 96.0f ? 4 : (rIn >= 48.0f ? 3 : (rIn >= 20.0f ? 2 : 0));
  s_angCache.col = -1;  // new centre: drop cached samples
  p.am = alpha * (32.0f / 255.0f);
  arcRaster(g, p);
}
}  // namespace

void arcGradient(float cx, float cy, float rOuter, float thickness, float a0, float a1,
                 const Stop *stops, int nStops, float ga0, float ga1, bool roundCaps,
                 uint8_t alpha) {
  if (!stops || nStops <= 0) return;
  for (int i = 0; i < 256; i++) s_angleLut[i] = gradientAt(stops, nStops, i / 255.0f);
  arcGradientLut(cx, cy, rOuter, thickness, a0, a1, ga0, ga1, roundCaps, alpha);
}

void arcGradientFn(float cx, float cy, float rOuter, float thickness, float a0, float a1,
                   ColorFn fn, void *user, float ga0, float ga1, bool roundCaps, uint8_t alpha) {
  if (!fn) return;
  for (int i = 0; i < 256; i++) s_angleLut[i] = fn(i / 255.0f, user);
  arcGradientLut(cx, cy, rOuter, thickness, a0, a1, ga0, ga1, roundCaps, alpha);
}

void arcGlow(float cx, float cy, float rOuter, float thickness, float a0, float a1, uint16_t c,
             float spread, uint8_t intensity, bool roundCaps) {
  if (!s_fb || !(spread > 0.0f)) return;
  intensity = effAlpha(intensity);
  if (!intensity) return;
  ArcG g;
  if (!arcSetup(g, cx, cy, rOuter, thickness, a0, a1, roundCaps, spread + 1.0f)) return;
  const uint32_t fx32 = expand(c);
  const float I = intensity * (32.0f / 255.0f);
  const float inv = 1.0f / spread;
  const float ro = g.rO + spread + 0.5f, ro2 = ro * ro;
  const float ri = g.rI - spread - 0.5f, ri2 = ri > 0.0f ? ri * ri : -1.0f;
  const float rOut2 = g.rO * g.rO, rIn2 = g.rI * g.rI;
  const float inv2rO = 0.5f / g.rO, inv2rI = g.rI > 0.5f ? 0.5f / g.rI : 1.0f;
  const float capLim = g.hw + spread, capLim2 = capLim * capLim;
  const float tReach = -(spread + (g.round ? g.hw : 0.0f) + 1.0f);
  for (int x = g.bx0; x < g.bx1; x++) {
    const float fx = x + 0.5f - cx;
    const float fx2 = fx * fx;
    if (fx2 >= ro2) continue;
    const float hO = fsqrt(ro2 - fx2);
    const float hI = (ri2 > 0.0f && fx2 < ri2) ? fsqrt(ri2 - fx2) : -1.0f;
    int ya = imax(iceil(cy - hO - 0.5f), g.by0);
    int yb = imin(ifloor(cy + hO - 0.5f) + 1, g.by1);
    int ha = yb, hb = yb;  // hole rows
    if (hI > 0.0f) {
      ha = ifloor(cy - hI - 0.5f) + 1;
      hb = iceil(cy + hI - 0.5f);
    }
    const float dya = ya + 0.5f - cy;
    const float e0a = g.c0 * dya - g.s0 * fx, e1a = g.s1 * fx - g.c1 * dya;  // wedge distances at ya
    auto shade = [&](int y) {
      const float dy = y + 0.5f - cy;
      const float d2 = fx2 + dy * dy;
      float w = 1.0f;
      if (!g.full) {
        const float e0 = e0a + g.c0 * (y - ya), e1 = e1a - g.c1 * (y - ya);
        w = g.wide ? (e0 > e1 ? e0 : e1) : (e0 < e1 ? e0 : e1);
      }
      float sd;
      if (w >= 0.0f || !g.round) {
        // Radial distance from d^2 without a sqrt: (d^2 - r^2) / 2r is within
        // a few % of (d - r) for |d - r| << r, plenty for a soft falloff.
        if (d2 > rOut2) sd = (d2 - rOut2) * inv2rO;
        else if (d2 < rIn2) sd = (rIn2 - d2) * inv2rI;
        else sd = 0.0f;
        if (w < 0.0f && -w > sd) sd = -w;  // flat caps: distance past the end line
      } else {
        const float ax = fx - g.p0x, ay = dy - g.p0y, bx = fx - g.p1x, by = dy - g.p1y;
        const float dc2 = fminf_(ax * ax + ay * ay, bx * bx + by * by);
        if (dc2 >= capLim2) return;
        sd = fsqrt(dc2) - g.hw;
      }
      if (sd >= spread) return;
      float al;
      if (sd <= 0.0f) {
        al = I;
      } else {
        const float u = 1.0f - sd * inv;
        al = u * u * I;
      }
      const uint32_t ai = (uint32_t)(al + BAYER[y & 3][x & 3] * (1.0f / 16.0f));
      if (ai) {
        uint16_t *p = pix(x, y);
        *p = blendX(fx32, *p, ai > 32 ? 32 : ai);
      }
    };
    // Rows to visit: [ya, yb) minus the hole, and for partial arcs only rows
    // within reach of the wedge (analytic, see arcRaster).
    RowIv iv[2] = {{ya, yb}, {0, 0}};
    if (!g.full) {
      const RowIv m0 = rowsAtLeast(e0a, g.c0, g.ik0, ya, tReach);
      const RowIv m1 = rowsAtLeast(e1a, -g.c1, g.ik1, ya, tReach);
      if (!g.wide) {
        iv[0] = RowIv{imax(imax(m0.lo, m1.lo), ya), imin(imin(m0.hi, m1.hi), yb)};
      } else {
        const RowIv gp = gapOf(m0, m1);
        if (gp.lo < gp.hi) {
          iv[0] = RowIv{ya, imin(gp.lo, yb)};
          iv[1] = RowIv{imax(gp.hi, ya), yb};
        }
      }
    }
    for (const RowIv &r : iv) {
      for (int y = r.lo; y < r.hi; y++) {
        if (y >= ha && y < hb) {
          y = hb - 1;
          continue;
        }
        shade(y);
      }
    }
  }
}

void glow(float cx, float cy, float r, uint16_t c, uint8_t intensity) {
  if (!s_fb || !(r > 0.0f)) return;
  intensity = effAlpha(intensity);
  if (!intensity) return;
  int x0, x1;
  if (!colRange(cx, r, x0, x1)) return;
  const uint32_t fx32 = expand(c);
  const float I = intensity * (32.0f / 255.0f);
  const float r2 = r * r, inv = 1.0f / r2;
  for (int x = x0; x < x1; x++) {
    const float fx = x + 0.5f - cx;
    const float fx2 = fx * fx;
    if (fx2 >= r2) continue;
    const float hh = fsqrt(r2 - fx2);
    const int ya = imax(iceil(cy - hh - 0.5f), s_cy0);
    const int yb = imin(ifloor(cy + hh - 0.5f) + 1, s_cy1);
    uint16_t *p = pix(x, ya);
    float dy = ya + 0.5f - cy;
    for (int y = ya; y < yb; y++, p--, dy += 1.0f) {
      const float u = 1.0f - (fx2 + dy * dy) * inv;
      if (u <= 0.0f) continue;
      const uint32_t ai = (uint32_t)(u * u * I + BAYER[y & 3][x & 3] * (1.0f / 16.0f));
      if (ai) *p = blendX(fx32, *p, ai > 32 ? 32 : ai);
    }
  }
}

void glowSoft(float cx, float cy, float r, uint16_t c, uint8_t intensity) {
  if (!s_fb || !(r > 0.0f)) return;
  intensity = effAlpha(intensity);
  if (!intensity) return;
  int x0, x1;
  if (!colRange(cx, r, x0, x1)) return;
  // alpha(d) = I * (1 - d/r)^2, tabulated over q = d^2/r^2 (no per-pixel sqrt).
  // 256 entries in 1/256 alpha steps of the 5-bit blend.
  constexpr int N = 256;
  uint16_t lut[N];
  const float I = intensity * (32.0f / 255.0f) * 256.0f;
  for (int i = 0; i < N; i++) {
    const float u = 1.0f - fsqrt((i + 0.5f) * (1.0f / N));
    lut[i] = (uint16_t)(u * u * I);
  }
  const uint32_t fx32 = expand(c);
  const float r2 = r * r, k = N / r2;
  for (int x = x0; x < x1; x++) {
    const float fx = x + 0.5f - cx;
    const float fx2 = fx * fx;
    if (fx2 >= r2) continue;
    const float hh = fsqrt(r2 - fx2);
    const int ya = imax(iceil(cy - hh - 0.5f), s_cy0);
    const int yb = imin(ifloor(cy + hh - 0.5f) + 1, s_cy1);
    uint16_t *p = pix(x, ya);
    float dy = ya + 0.5f - cy;
    for (int y = ya; y < yb; y++, p--, dy += 1.0f) {
      const int i = (int)((fx2 + dy * dy) * k);
      if (i >= N) continue;
      const uint32_t ai = (lut[i] + BAYER[y & 3][x & 3] * 16u) >> 8;
      if (ai) *p = blendX(fx32, *p, ai > 32 ? 32 : ai);
    }
  }
}

// ============================================================================
// Lines & polygons
// ============================================================================
void line(float x0, float y0, float x1, float y1, float width, uint16_t c, uint8_t alpha) {
  if (!(width > 0.0f)) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  const float r = width * 0.5f;
  capsRaster(x0, y0, r, x1, y1, r, PSolid(c, alpha));
}

void polar(float cx, float cy, float r, float deg, float &x, float &y) {
  const float a = deg * 0.0174532925f;
  x = cx + r * cosf(a);
  y = cy + r * sinf(a);
}

void needle(float cx, float cy, float deg, float rBack, float rTip, float wBack, float wTip,
            uint16_t c, uint8_t alpha) {
  alpha = effAlpha(alpha);
  if (!alpha) return;
  float xb, yb, xt, yt;
  polar(cx, cy, rBack, deg, xb, yb);
  polar(cx, cy, rTip, deg, xt, yt);
  const float rt = wTip > 0.0f ? wTip * 0.5f : 0.01f;
  capsRaster(xb, yb, wBack * 0.5f, xt, yt, rt, PSolid(c, alpha));
}

void fillPolygon(const float *xy, int n, uint16_t c, uint8_t alpha) {
  if (!s_fb || !xy || n < 3 || n > 16) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  float nx[16], ny[16], nc[16];
  float mx = 0, my = 0, xmn = 1e9f, xmx = -1e9f, ymn = 1e9f, ymx = -1e9f;
  for (int i = 0; i < n; i++) {
    mx += xy[2 * i];
    my += xy[2 * i + 1];
    xmn = fminf_(xmn, xy[2 * i]); xmx = fmaxf_(xmx, xy[2 * i]);
    ymn = fminf_(ymn, xy[2 * i + 1]); ymx = fmaxf_(ymx, xy[2 * i + 1]);
  }
  mx /= n;
  my /= n;
  int ne = 0;
  for (int i = 0; i < n; i++) {
    const float ax = xy[2 * i], ay = xy[2 * i + 1];
    const float bx = xy[2 * ((i + 1) % n)], by = xy[2 * ((i + 1) % n) + 1];
    const float ex = bx - ax, ey = by - ay;
    const float len = fsqrt(ex * ex + ey * ey);
    if (len < 1e-5f) continue;
    float px = ey / len, py = -ex / len;
    if (px * (mx - ax) + py * (my - ay) > 0.0f) { px = -px; py = -py; }  // outward
    nx[ne] = px;
    ny[ne] = py;
    nc[ne] = px * ax + py * ay;
    ne++;
  }
  if (ne < 3) return;
  const PSolid paint(c, alpha);
  // Edges sorted left->right for the per-column extent computation.
  float ex0[16], ey0[16], ex1[16], ey1[16], einv[16];
  for (int i = 0; i < n; i++) {
    float ax = xy[2 * i], ay = xy[2 * i + 1];
    float bx = xy[2 * ((i + 1) % n)], by = xy[2 * ((i + 1) % n) + 1];
    if (ax > bx) { float t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
    ex0[i] = ax; ey0[i] = ay; ex1[i] = bx; ey1[i] = by;
    einv[i] = bx - ax > 1e-6f ? 1.0f / (bx - ax) : 0.0f;
  }
  const int xa = imax(ifloor(xmn - 1.0f), s_cx0), xb = imin(iceil(xmx + 1.0f) + 1, s_cx1);
  for (int x = xa; x < xb; x++) {
    const float xc = x + 0.5f;
    // y extent of the polygon inside the slab [xc - 1, xc + 1]
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < n; i++) {
      const float ax = ex0[i], ay = ey0[i], bx = ex1[i], by = ey1[i];
      const float s0 = xc - 1.0f, s1 = xc + 1.0f;
      if (bx < s0 || ax > s1) continue;
      float ya_ = ay, yb_ = by;
      if (einv[i] > 0.0f) {
        const float ta = fmaxf_(0.0f, (s0 - ax) * einv[i]), tb = fminf_(1.0f, (s1 - ax) * einv[i]);
        ya_ = ay + (by - ay) * ta;
        yb_ = ay + (by - ay) * tb;
      }
      lo = fminf_(lo, fminf_(ya_, yb_));
      hi = fmaxf_(hi, fmaxf_(ya_, yb_));
    }
    if (lo > hi) continue;
    const int ya = imax(ifloor(lo - 1.0f), s_cy0), yb = imin(iceil(hi + 1.0f) + 1, s_cy1);
    for (int y = ya; y < yb; y++) {
      const float yc = y + 0.5f;
      float d = -1e9f;
      for (int i = 0; i < ne; i++) {
        const float e = nx[i] * xc + ny[i] * yc - nc[i];
        if (e > d) d = e;
      }
      const float cov = 0.5f - d;
      if (cov > 0.0f) paint.px(x, y, cov > 1.0f ? 1.0f : cov);
    }
  }
}

void fillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c,
                  uint8_t alpha) {
  const float xy[6] = {x0, y0, x1, y1, x2, y2};
  fillPolygon(xy, 3, c, alpha);
}

// ============================================================================
// Images
// ============================================================================
void image565(int x, int y, int w, int h, const uint16_t *src, int32_t key) {
  if (!s_fb || !src) return;
  const int x0 = imax(x, s_cx0), x1 = imin(x + w, s_cx1);
  const int y0 = imax(y, s_cy0), y1 = imin(y + h, s_cy1);
  if (x0 >= x1 || y0 >= y1) return;
  const uint8_t alpha = effAlpha(255);
  const uint32_t a32 = (alpha * 32 + 127) / 255;
  for (int xx = x0; xx < x1; xx++) {
    const uint16_t *s = src + (y0 - y) * w + (xx - x);
    uint16_t *p = pix(xx, y0);
    for (int yy = y0; yy < y1; yy++, p--, s += w) {
      const uint16_t c = *s;
      if (key >= 0 && c == (uint16_t)key) continue;
      *p = alpha == 255 ? c : blendX(expand(c), *p, a32);
    }
  }
}

void imageNative(int x, int y, int w, int h, const uint16_t *cols) {
  if (!s_fb || !cols) return;
  const int x0 = imax(x, s_cx0), x1 = imin(x + w, s_cx1);
  const int y0 = imax(y, s_cy0), y1 = imin(y + h, s_cy1);
  if (x0 >= x1 || y0 >= y1) return;
  const uint8_t alpha = effAlpha(255);
  const uint32_t a32 = (alpha * 32 + 127) / 255;
  const int n = y1 - y0;
  for (int xx = x0; xx < x1; xx++) {
    // column entry k is row y + h - 1 - k; start at the lowest visible row
    const uint16_t *s = cols + (size_t)(xx - x) * h + (y + h - y1);
    uint16_t *p = pix(xx, y1 - 1);
    if (alpha == 255) {
      memcpy(p, s, (size_t)n * 2);
    } else {
      for (int i = 0; i < n; i++) p[i] = blendX(expand(s[i]), p[i], a32);
    }
  }
}

void mask4(int x, int y, int w, int h, const uint8_t *a4, uint16_t c, uint8_t alpha) {
  if (!s_fb || !a4) return;
  alpha = effAlpha(alpha);
  if (!alpha) return;
  const int x0 = imax(x, s_cx0), x1 = imin(x + w, s_cx1);
  const int y0 = imax(y, s_cy0), y1 = imin(y + h, s_cy1);
  if (x0 >= x1 || y0 >= y1) return;
  const int stride = (w + 1) >> 1;
  const uint32_t fx = expand(c);
  uint8_t a32[16];
  for (int i = 0; i < 16; i++) a32[i] = (uint8_t)((i * 17 * alpha * 32 + 32512) / 65025);
  for (int xx = x0; xx < x1; xx++) {
    const int col = xx - x;
    const uint8_t *s = a4 + (y0 - y) * stride + (col >> 1);
    const int sh = (col & 1) ? 0 : 4;
    uint16_t *p = pix(xx, y0);
    for (int yy = y0; yy < y1; yy++, p--, s += stride) {
      const uint32_t a = a32[(*s >> sh) & 15];
      if (a) *p = a >= 32 ? c : blendX(fx, *p, a);
    }
  }
}

// ============================================================================
// Text
// ============================================================================
uint32_t utf8Next(const char **ps) {
  const uint8_t *s = (const uint8_t *)*ps;
  uint32_t c = *s;
  if (!c) return 0;
  int n = 0;
  if (c < 0x80) {
    *ps += 1;
    return c;
  } else if ((c & 0xE0) == 0xC0) {
    c &= 0x1F; n = 1;
  } else if ((c & 0xF0) == 0xE0) {
    c &= 0x0F; n = 2;
  } else if ((c & 0xF8) == 0xF0) {
    c &= 0x07; n = 3;
  } else {
    *ps += 1;
    return 0xFFFD;
  }
  for (int i = 1; i <= n; i++) {
    if ((s[i] & 0xC0) != 0x80) {  // truncated sequence: consume the bytes seen
      *ps += i;
      return 0xFFFD;
    }
    c = (c << 6) | (s[i] & 0x3F);
  }
  *ps += n + 1;
  return c;
}

const Glyph *findGlyph(const Font &f, uint32_t cp) {
  int lo = 0, hi = (int)f.count - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) >> 1;
    const uint32_t v = f.glyphs[mid].cp;
    if (v == cp) return &f.glyphs[mid];
    if (v < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

namespace {
inline const Glyph *glyphOrFallback(const Font &f, uint32_t cp) {
  const Glyph *g = findGlyph(f, cp);
  if (!g && cp != ' ') g = findGlyph(f, '?');
  return g;
}
inline bool isDigit(uint32_t cp) { return cp >= '0' && cp <= '9'; }
inline int glyphAdvance(const Font &f, const Glyph *g, uint32_t cp, uint8_t flags) {
  if ((flags & TABULAR) && isDigit(cp)) return f.digitAdvance;
  if (g) return g->adv;
  return cp == ' ' ? f.spaceAdvance : 0;
}

void drawGlyph(const Font &f, const Glyph &g, int penX, int baseY, uint32_t fx, uint16_t c,
               const uint8_t *a32) {
  if (!g.w || !g.h) return;
  const int gx = penX + g.xoff, gy = baseY + g.yoff;
  const int x0 = imax(gx, s_cx0), x1 = imin(gx + g.w, s_cx1);
  const int y0 = imax(gy, s_cy0), y1 = imin(gy + g.h, s_cy1);
  if (x0 >= x1 || y0 >= y1) return;
  const uint8_t *bits = f.bits + g.off;
  for (int x = x0; x < x1; x++) {
    uint32_t idx = (uint32_t)(x - gx) * g.h + (uint32_t)(y0 - gy);  // nibble index
    uint16_t *p = pix(x, y0);
    for (int y = y0; y < y1; y++, p--, idx++) {
      const uint8_t b = bits[idx >> 1];
      const uint32_t a = a32[(idx & 1) ? (b & 15) : (b >> 4)];
      if (a) *p = a >= 32 ? c : blendX(fx, *p, a);
    }
  }
}
}  // namespace

int baselineFor(int y, const Font &f, uint8_t flags) {
  switch (flags & V_MASK) {
    case TOP: return y + f.capHeight;
    case MIDDLE: return y + (f.capHeight + 1) / 2;
    case BOTTOM: return y - f.descent;
    default: return y;
  }
}

int textWidth(const char *s, const Font &f, uint8_t flags, int tracking) {
  if (!s) return 0;
  int w = 0, n = 0;
  while (*s) {
    const uint32_t cp = utf8Next(&s);
    const Glyph *g = glyphOrFallback(f, cp);
    w += glyphAdvance(f, g, cp, flags);
    n++;
  }
  if (n > 1) w += tracking * (n - 1);
  return w;
}

int text(int x, int y, const char *s, const Font &f, uint16_t c, uint8_t flags, uint8_t alpha,
         int tracking) {
  if (!s) return 0;
  const int w = textWidth(s, f, flags, tracking);
  alpha = effAlpha(alpha);
  if (!s_fb || !alpha) return w;
  int pen = x;
  if ((flags & H_MASK) == CENTER) pen = x - w / 2;
  else if ((flags & H_MASK) == RIGHT) pen = x - w;
  const int base = baselineFor(y, f, flags);
  uint8_t a32[16];
  for (int i = 0; i < 16; i++) a32[i] = (uint8_t)((i * 17 * alpha * 32 + 32512) / 65025);
  const uint32_t fx = expand(c);
  while (*s) {
    const uint32_t cp = utf8Next(&s);
    const Glyph *g = glyphOrFallback(f, cp);
    const int adv = glyphAdvance(f, g, cp, flags);
    if (g) {
      int gx = pen;
      if ((flags & TABULAR) && isDigit(cp)) gx += (f.digitAdvance - g->adv) / 2;
      drawGlyph(f, *g, gx, base, fx, c, a32);
    }
    pen += adv + tracking;
  }
  return w;
}

bool textFit(char *out, size_t n, const char *s, const Font &f, int maxw, int tracking,
             uint8_t flags) {
  if (!out || n == 0) return false;
  out[0] = 0;
  if (!s) return false;
  strlcpy(out, s, n);
  if (textWidth(out, f, flags, tracking) <= maxw) return false;
  const char *ell = findGlyph(f, 0x2026) ? "\xE2\x80\xA6" : "..";
  const int ew = textWidth(ell, f, flags, tracking) + tracking;
  // Walk codepoints, remember the last cut that still fits.
  const char *p = s;
  size_t cut = 0;
  int wsum = 0, cnt = 0;
  while (*p) {
    const char *q = p;
    const uint32_t cp = utf8Next(&q);
    const int adv = glyphAdvance(f, glyphOrFallback(f, cp), cp, flags) + (cnt ? tracking : 0);
    if (wsum + adv + ew > maxw) break;
    wsum += adv;
    cnt++;
    p = q;
    cut = (size_t)(p - s);
  }
  while (cut > 0 && s[cut - 1] == ' ') cut--;  // no space before the ellipsis
  if (cut + strlen(ell) + 1 > n) cut = n > strlen(ell) + 1 ? n - strlen(ell) - 1 : 0;
  memcpy(out, s, cut);
  out[cut] = 0;
  strlcat(out, ell, n);
  return true;
}

// ============================================================================
// Icons
// ============================================================================
namespace icon {

void wifi(float cx, float cy, float s, uint16_t c, int bars, uint16_t dim, bool drawDim) {
  // Fan of three arcs opening upwards, centred on the dot at the bottom.
  const float by = cy + s * 0.36f;
  const float t = s * 0.13f;
  for (int i = 0; i < 3; i++) {
    const float r = s * (0.30f + 0.24f * i) + t * 0.5f;
    const bool lit = i < bars;
    if (!lit && !drawDim) continue;
    arc(cx, by, r, t, 227.0f, 313.0f, lit ? c : dim, true);
  }
  fillCircle(cx, by - s * 0.02f, s * 0.10f, bars > 0 || !drawDim ? c : dim);
}

void check(float cx, float cy, float s, uint16_t c, float stroke) {
  const float w = stroke > 0.0f ? stroke : s * 0.14f;
  const float x0 = cx - s * 0.36f, y0 = cy + s * 0.02f;
  const float x1 = cx - s * 0.10f, y1 = cy + s * 0.28f;
  const float x2 = cx + s * 0.38f, y2 = cy - s * 0.26f;
  line(x0, y0, x1, y1, w, c);
  line(x1, y1, x2, y2, w, c);
}

void cross(float cx, float cy, float s, uint16_t c, float stroke) {
  const float w = stroke > 0.0f ? stroke : s * 0.14f;
  const float k = s * 0.30f;
  line(cx - k, cy - k, cx + k, cy + k, w, c);
  line(cx - k, cy + k, cx + k, cy - k, w, c);
}

void gear(float cx, float cy, float s, uint16_t c, uint16_t hole) {
  const float rb = s * 0.34f;
  for (int i = 0; i < 8; i++) {
    const float a = i * 45.0f + 22.5f;
    // tooth: short thick capsule-free trapezoid
    float x0, y0, x1, y1;
    polar(cx, cy, rb - s * 0.04f, a, x0, y0);
    polar(cx, cy, s * 0.48f, a, x1, y1);
    const float ang = a * 0.0174532925f;
    const float px = -sinf(ang), py = cosf(ang);
    const float wb = s * 0.095f, wt = s * 0.07f;
    const float q[8] = {x0 + px * wb, y0 + py * wb, x1 + px * wt, y1 + py * wt,
                        x1 - px * wt, y1 - py * wt, x0 - px * wb, y0 - py * wb};
    fillPolygon(q, 4, c);
  }
  fillCircle(cx, cy, rb, c);
  fillCircle(cx, cy, s * 0.14f, hole);
}

void bolt(float cx, float cy, float s, uint16_t c) {
  // Two overlapping triangles make the zig-zag.
  const float t1[6] = {cx + s * 0.16f, cy - s * 0.50f, cx - s * 0.30f, cy + s * 0.08f,
                       cx + s * 0.08f, cy + s * 0.08f};
  const float t2[6] = {cx - s * 0.16f, cy + s * 0.50f, cx + s * 0.30f, cy - s * 0.08f,
                       cx - s * 0.08f, cy - s * 0.08f};
  fillPolygon(t1, 3, c);
  fillPolygon(t2, 3, c);
}

void flag(float cx, float cy, float s, uint16_t c) {
  // Pole + chequered flag (2 rows x 3 columns).
  const float px = cx - s * 0.32f;
  line(px, cy - s * 0.46f, px, cy + s * 0.48f, s * 0.09f, c);
  const float fx0 = px + s * 0.04f, fy0 = cy - s * 0.44f;
  const float cw = s * 0.22f, ch = s * 0.18f;
  strokeRoundRect((int)lroundf(fx0), (int)lroundf(fy0), (int)lroundf(cw * 3), (int)lroundf(ch * 3),
                  1.0f, fmaxf_(1.0f, s * 0.05f), c);
  for (int r = 0; r < 3; r++)
    for (int k = 0; k < 3; k++)
      if (((r + k) & 1) == 0)
        fillRect((int)lroundf(fx0 + k * cw), (int)lroundf(fy0 + r * ch), (int)lroundf(cw),
                 (int)lroundf(ch), c);
}

void stopwatch(float cx, float cy, float s, uint16_t c, uint16_t face) {
  const float r = s * 0.40f;
  const float ccy = cy + s * 0.08f;
  fillCircle(cx, ccy, r, face);
  strokeCircle(cx, ccy, r, s * 0.09f, c);
  fillRoundRect((int)lroundf(cx - s * 0.10f), (int)lroundf(cy - s * 0.50f), (int)lroundf(s * 0.20f),
                (int)lroundf(s * 0.10f), s * 0.03f, c);
  line(cx, ccy - r, cx, cy - s * 0.42f, s * 0.08f, c);
  float hx, hy;
  polar(cx, ccy, r * 0.62f, -60.0f, hx, hy);
  line(cx, ccy, hx, hy, s * 0.08f, c);
  fillCircle(cx, ccy, s * 0.06f, c);
}

void chevron(float cx, float cy, float s, uint16_t c, bool right, float stroke) {
  const float w = stroke > 0.0f ? stroke : s * 0.14f;
  const float k = s * 0.18f * (right ? 1.0f : -1.0f);
  line(cx - k, cy - s * 0.34f, cx + k, cy, w, c);
  line(cx + k, cy, cx - k, cy + s * 0.34f, w, c);
}

}  // namespace icon

}  // namespace gfx
