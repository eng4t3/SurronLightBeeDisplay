// ui_draw.cpp - shared drawing helpers (see ui_draw.h).
#include "ui_draw.h"

#include <math.h>

#include "logo.h"
#include "ui_assets.h"

namespace ui {

const Theme *T = &THEME_DARK;

// Font roles (DESIGN_SPEC §2.2): LABEL is tracked +1.5 px, the hero numbers
// are tightened, all number fonts use tabular digits.
const FontSpec LABEL = {&F_LABEL, 1.5f, false};
const FontSpec SMALL = {&F_SMALL, 0.0f, false};
const FontSpec BODY = {&F_BODY, 0.0f, false};
const FontSpec BODYB = {&F_BODYB, 0.0f, false};
const FontSpec TITLE = {&F_TITLE, 0.0f, false};
const FontSpec NUM_M = {&F_NUM_M, 0.0f, true};
const FontSpec NUM_L = {&F_NUM_L, 0.0f, true};
const FontSpec NUM_XL = {&F_NUM_XL, -2.0f, true};
const FontSpec NUM_XXL = {&F_NUM_XXL, -4.0f, true};

void cpolar(float cx, float cy, float r, float deg, float &x, float &y) {
  const float a = deg * 0.0174532925f;
  x = cx + r * sinf(a);
  y = cy - r * cosf(a);
}

// ============================================================================
// Text
// ============================================================================
namespace {

inline bool isDigit(uint32_t cp) { return cp >= '0' && cp <= '9'; }

// Advance of one codepoint (same fallback rules as gfx::text).
int advOf(const gfx::Font &f, uint32_t cp) {
  const gfx::Glyph *g = gfx::findGlyph(f, cp);
  if (!g && cp != ' ') g = gfx::findGlyph(f, '?');
  if (g) return g->adv;
  return cp == ' ' ? f.spaceAdvance : 0;
}

// Encodes one codepoint as a NUL-terminated UTF-8 string.
void utf8Put(char *b, uint32_t cp) {
  if (cp < 0x80) {
    b[0] = (char)cp;
    b[1] = 0;
  } else if (cp < 0x800) {
    b[0] = (char)(0xC0 | (cp >> 6));
    b[1] = (char)(0x80 | (cp & 0x3F));
    b[2] = 0;
  } else {
    b[0] = (char)(0xE0 | (cp >> 12));
    b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    b[2] = (char)(0x80 | (cp & 0x3F));
    b[3] = 0;
  }
}

}  // namespace

float textW(const char *s, const FontSpec &f, float track) {
  if (!s) return 0.0f;
  const float tr = track == TRACK_DEFAULT ? f.track : track;
  float x = 0.0f;
  int n = 0;
  while (*s) {
    const uint32_t cp = gfx::utf8Next(&s);
    x += (f.tab && isDigit(cp) ? f.f->digitAdvance : advOf(*f.f, cp)) + tr;
    n++;
  }
  return n ? x - tr : 0.0f;
}

// Glyph-by-glyph layout exactly like the mockup's layout(): rounded advances,
// tracking between glyphs (may be fractional), tabular digits centred in the
// widest-digit cell, block origin rounded, glyph origins rounded.
TextBox text(const char *s, const FontSpec &f, float x, int base, uint16_t c, Align a, float track,
             uint8_t alpha) {
  TextBox tb = {0, 0.0f};
  if (!s) return tb;
  const float tr = track == TRACK_DEFAULT ? f.track : track;
  tb.w = textW(s, f, tr);
  tb.x0 = jsRound(a == AL_CENTER ? x - tb.w * 0.5f : (a == AL_RIGHT ? x - tb.w : x));
  const int cell = f.f->digitAdvance;
  float pen = 0.0f;
  char buf[5];
  while (*s) {
    const uint32_t cp = gfx::utf8Next(&s);
    const int adv = advOf(*f.f, cp);
    const bool dig = f.tab && isDigit(cp);
    if (cp != ' ') {
      const float ix = pen + (dig ? (float)jsRound((cell - adv) * 0.5f) : 0.0f);
      utf8Put(buf, cp);
      gfx::text(jsRound(tb.x0 + ix), base, buf, *f.f, c, gfx::LEFT, alpha);
    }
    pen += (dig ? cell : adv) + tr;
  }
  return tb;
}

float valUnit(const char *val, const FontSpec &vf, const char *unit, const FontSpec &uf, float x, int base,
              Align a, uint16_t vc, uint16_t uc, float gap) {
  const bool hasUnit = unit && *unit;
  const float wv = textW(val, vf);
  const float wt = wv + (hasUnit ? gap + textW(unit, uf) : 0.0f);
  const float x0 = a == AL_CENTER ? x - wt * 0.5f : (a == AL_RIGHT ? x - wt : x);
  text(val, vf, (float)jsRound(x0), base, vc);
  if (hasUnit) text(unit, uf, (float)jsRound(x0 + wv + gap), base, uc);
  return wt;
}

void fitText(char *out, size_t n, const char *s, const FontSpec &f, float maxw) {
  gfx::textFit(out, n, s, *f.f, (int)maxw, (int)f.track);
}

// ============================================================================
// Shapes
// ============================================================================
// Rounded-rect stroke inside the rect. For integer radii the straight sides
// are plain rect fills (1 px rows/columns are exact anyway) and only the four
// corners are rasterized as quarter arcs: ~10x cheaper than a full
// strokeRoundRect on the big cards. Fractional widths get a partial-alpha row.
void strokeRR(int x, int y, int w, int h, float r, float lw, uint16_t c, uint8_t a) {
  const float m = (w < h ? w : h) * 0.5f;
  if (r > m) r = m;
  const int ri = (int)r;
  if (lw <= 0.0f) return;
  if ((float)ri != r || r < lw + 1.0f || lw > 4.0f) {
    gfx::strokeRoundRect(x, y, w, h, r, lw, c, a);
    return;
  }
  const int full = (int)lw;
  const uint8_t pa = (uint8_t)(a * (lw - full) + 0.5f);
  const int sw = w - 2 * ri, sh = h - 2 * ri;
  if (sw > 0) {
    gfx::fillRectAlpha(x + ri, y, sw, full, c, a);
    gfx::fillRectAlpha(x + ri, y + h - full, sw, full, c, a);
    if (pa) {
      gfx::fillRectAlpha(x + ri, y + full, sw, 1, c, pa);
      gfx::fillRectAlpha(x + ri, y + h - full - 1, sw, 1, c, pa);
    }
  }
  if (sh > 0) {
    gfx::fillRectAlpha(x, y + ri, full, sh, c, a);
    gfx::fillRectAlpha(x + w - full, y + ri, full, sh, c, a);
    if (pa) {
      gfx::fillRectAlpha(x + full, y + ri, 1, sh, c, pa);
      gfx::fillRectAlpha(x + w - full - 1, y + ri, 1, sh, c, pa);
    }
  }
  gfx::arc(x + r, y + r, r, lw, 180, 270, c, false, a);
  gfx::arc(x + w - r, y + r, r, lw, 270, 360, c, false, a);
  gfx::arc(x + w - r, y + h - r, r, lw, 0, 90, c, false, a);
  gfx::arc(x + r, y + h - r, r, lw, 90, 180, c, false, a);
}

void fillBgExcept(const Rect *cards, int n, int r) {
  // Vertical bands between all card edges and corner-square edges; in each
  // band the rows covered by a card interior are skipped.
  int xs[2 + 4 * 8];
  int nx = 0;
  xs[nx++] = 0;
  xs[nx++] = gfx::W;
  for (int i = 0; i < n && i < 8; i++) {
    const Rect &c = cards[i];
    xs[nx++] = c.x;
    xs[nx++] = c.x + r;
    xs[nx++] = c.x + c.w - r;
    xs[nx++] = c.x + c.w;
  }
  for (int i = 1; i < nx; i++)  // insertion sort (tiny)
    for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
      const int t = xs[j];
      xs[j] = xs[j - 1];
      xs[j - 1] = t;
    }
  for (int b = 0; b + 1 < nx; b++) {
    const int xa = xs[b], xb = xs[b + 1];
    if (xb <= xa) continue;
    // covered row intervals in this band (at most one per card), sorted by y
    int ya[8], yb[8], m = 0;
    for (int i = 0; i < n && i < 8; i++) {
      const Rect &c = cards[i];
      if (xa < c.x || xb > c.x + c.w) continue;
      const bool corner = xa < c.x + r || xb > c.x + c.w - r;
      int k = m++;
      ya[k] = corner ? c.y + r : c.y;
      yb[k] = corner ? c.y + c.h - r : c.y + c.h;
      while (k > 0 && ya[k - 1] > ya[k]) {
        int t = ya[k]; ya[k] = ya[k - 1]; ya[k - 1] = t;
        t = yb[k]; yb[k] = yb[k - 1]; yb[k - 1] = t;
        k--;
      }
    }
    int y = 0;
    for (int k = 0; k < m; k++) {
      if (ya[k] > y) gfx::fillRect(xa, y, xb - xa, ya[k] - y, T->bg);
      if (yb[k] > y) y = yb[k];
    }
    if (y < gfx::H) gfx::fillRect(xa, y, xb - xa, gfx::H - y, T->bg);
  }
}

void card(int x, int y, int w, int h, float r, uint16_t fill, uint16_t stroke, float lw, uint8_t strokeA) {
  gfx::fillRoundRect(x, y, w, h, r, fill);
  if (lw > 0.0f) strokeRR(x, y, w, h, r, lw, stroke, strokeA);
}

void hcapsule(float x, float y, float w, float th, uint16_t c, uint8_t a) {
  if (w <= th) {
    gfx::fillCircle(x + th * 0.5f, y, th * 0.5f, c, a);
    return;
  }
  const float top = y - th * 0.5f;
  if (top == floorf(top) && th == floorf(th)) {  // pixel-aligned rows: a pill (much cheaper than line())
    const int x0 = jsRound(x);
    gfx::fillRoundRect(x0, (int)top, jsRound(x + w) - x0, (int)th, th * 0.5f, c, a);
  } else {
    gfx::line(x + th * 0.5f, y, x + w - th * 0.5f, y, th, c, a);
  }
}

void capsule(float x0, float y0, float x1, float y1, float w, uint16_t c, uint8_t a) {
  const float h = w * 0.5f;
  auto integral = [](float v) { return v == floorf(v); };
  if (y0 == y1 && integral(y0 - h) && integral(w) && integral(fminf(x0, x1) - h) && integral(fmaxf(x0, x1) + h)) {
    const int xa = (int)(fminf(x0, x1) - h);
    gfx::fillRoundRect(xa, (int)(y0 - h), (int)(fmaxf(x0, x1) + h) - xa, (int)w, h, c, a);
  } else if (x0 == x1 && integral(x0 - h) && integral(w) && integral(fminf(y0, y1) - h) &&
             integral(fmaxf(y0, y1) + h)) {
    const int ya = (int)(fminf(y0, y1) - h);
    gfx::fillRoundRect((int)(x0 - h), ya, (int)w, (int)(fmaxf(y0, y1) + h) - ya, h, c, a);
  } else {
    gfx::line(x0, y0, x1, y1, w, c, a);
  }
}

void arcC(float cx, float cy, float r, float th, float d0, float d1, uint16_t c, bool round, uint8_t a) {
  if (d1 - d0 < 0.01f) {
    if (!round) return;
    float x, y;
    cpolar(cx, cy, r, d0, x, y);
    gfx::fillCircle(x, y, th * 0.5f, c, a);
    return;
  }
  gfx::arc(cx, cy, r + th * 0.5f, th, d0 - 90.0f, d1 - 90.0f, c, round, a);
}

void arcGradC(float cx, float cy, float r, float th, float d0, float d1, const gfx::Stop *st, int n, float g0,
              float g1, bool round) {
  if (d1 - d0 < 0.01f) {  // zero length: a dot in the first stop colour
    if (!round) return;
    float x, y;
    cpolar(cx, cy, r, d0, x, y);
    gfx::fillCircle(x, y, th * 0.5f, st[0].color);
    return;
  }
  gfx::arcGradient(cx, cy, r + th * 0.5f, th, d0 - 90.0f, d1 - 90.0f, st, n, g0 - 90.0f, g1 - 90.0f, round);
}

void glow(float cx, float cy, float r, uint16_t c, float A) {
  const float a = A * T->glowK * 255.0f;
  if (a >= 1.0f) gfx::glowSoft(cx, cy, r, c, (uint8_t)(a > 255.0f ? 255.0f : a));
}

void poly(const float *xy, int n, uint16_t c, uint8_t a) { gfx::fillPolygon(xy, n, c, a); }

void chamferPanel(int x, int y, int w, int h, float ch, uint16_t fill, uint16_t stroke) {
  auto pts = [](float *p, float x, float y, float w, float h, float c) {
    const float v[12] = {x + c, y, x + w, y, x + w, y + h - c, x + w - c, y + h, x, y + h, x, y + c};
    memcpy(p, v, sizeof(v));
  };
  float p[12];
  pts(p, x, y, w, h, ch);
  gfx::fillPolygon(p, 6, stroke);
  pts(p, x + 1, y + 1, w - 2, h - 2, ch - 0.4f);
  gfx::fillPolygon(p, 6, fill);
}

// ============================================================================
// Icons & images
// ============================================================================
namespace {
const ui_assets::IconMask *const ICONS[IC_COUNT] = {
    &ui_assets::ICON_WIFI_18, &ui_assets::ICON_WIFI_20, &ui_assets::ICON_SYSTEM_20, &ui_assets::ICON_SKIN_20,
    &ui_assets::ICON_SPEED_20, &ui_assets::ICON_ODO_20, &ui_assets::ICON_LANG_20, &ui_assets::ICON_RESET_20,
    &ui_assets::ICON_WARN_20, &ui_assets::ICON_CHECK_18, &ui_assets::ICON_CHECK_20, &ui_assets::ICON_SUN_16,
    &ui_assets::ICON_SUN_22,
};

// Coverage (0..255) of an r = 8 rounded corner for the 8x8 pixels of the
// top-left corner; the other corners are mirrored.
uint8_t s_corner[8][8];
bool s_cornerInit = false;
void initCorner() {
  for (int j = 0; j < 8; j++) {
    for (int i = 0; i < 8; i++) {
      const float dx = 8.0f - (i + 0.5f), dy = 8.0f - (j + 0.5f);
      const float d = sqrtf(dx * dx + dy * dy);
      s_corner[j][i] = (uint8_t)(clampf(8.0f - d + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
  }
  s_cornerInit = true;
}
}  // namespace

void icon(Icon id, int x, int y, uint16_t c, uint8_t a) {
  if (id >= IC_COUNT) return;
  const ui_assets::IconMask &m = *ICONS[id];
  gfx::mask4(x, y, m.w, m.h, m.a4, c, a);
}

void thumb(int skin, bool light, int x, int y, uint16_t bg) {
  if (skin < 0 || skin >= SKIN_COUNT) return;
  if (!s_cornerInit) initCorner();
  constexpr int TW = ui_assets::THUMB_W, TH = ui_assets::THUMB_H;
  gfx::imageNative(x, y, TW, TH, ui_assets::THUMBS[skin][light ? 1 : 0]);
  for (int j = 0; j < 8; j++) {
    for (int i = 0; i < 8; i++) {
      const uint8_t cov = s_corner[j][i];
      if (cov == 255) continue;
      const uint8_t a = 255 - cov;
      gfx::pixel(x + i, y + j, bg, a);
      gfx::pixel(x + TW - 1 - i, y + j, bg, a);
      gfx::pixel(x + i, y + TH - 1 - j, bg, a);
      gfx::pixel(x + TW - 1 - i, y + TH - 1 - j, bg, a);
    }
  }
}

void logo(int x, int y, uint8_t alpha) {
  if (!alpha) return;
  const uint8_t saved = gfx::getAlpha();
  gfx::setAlpha(alpha);
  gfx::image565(x, y, LOGO_WIDTH, LOGO_HEIGHT, logo_bitmap, 0x0000);
  gfx::setAlpha(saved);
}

// ============================================================================
// Components
// ============================================================================
void holdRing(float cx, float cy, float p) {
  ring(cx, cy, 7.0f, 3.0f, T->track);
  if (p > 0.0f) arcC(cx, cy, 7.0f, 3.0f, 0.0f, 360.0f * clampf(p, 0.0f, 1.0f), T->accent);
}

TextBox statLabel(const char *label, float x, int base, Align a, float hold, uint16_t c) {
  const bool holding = hold >= 0.0f;
  const TextBox tb = text(label, LABEL, x, base, holding ? T->accent : c, a);
  if (holding) {
    const float hx = a == AL_RIGHT ? tb.x0 - 14.0f : tb.x0 + tb.w + 14.0f;
    holdRing(hx, base - 4.5f, hold);
  }
  return tb;
}

void topBar(float pos, const char *title, bool wifi, bool wifiErr, bool demo) {
  // Page dots: the active page is an 18x6 accent capsule, the others 6x6
  // dots; at fractional positions width and colour interpolate.
  float w[PAGE_COUNT], sum = 0.0f;
  for (int i = 0; i < PAGE_COUNT; i++) {
    const float k = clampf(1.0f - fabsf(pos - i), 0.0f, 1.0f);
    w[i] = 6.0f + 12.0f * k;
    sum += w[i];
  }
  float x = 240.0f - (sum + 2 * 8.0f) * 0.5f;
  for (int i = 0; i < PAGE_COUNT; i++) {
    const float k = clampf(1.0f - fabsf(pos - i), 0.0f, 1.0f);
    hcapsule(x, 15.0f, w[i], 6.0f, gfx::lerp(T->text_faint, T->accent, k));
    x += w[i] + 8.0f;
  }
  if (title && *title) text(title, LABEL, 16, 20, T->text_dim);

  int rx = 464;
  if (wifi) {
    icon(IC_WIFI18, rx - 18, 6, wifiErr ? T->red : T->accent);
    rx -= 18 + 12;
  }
  if (demo) {
    const int bw = jsRound(textW("DEMO", LABEL)) + 16;
    gfx::fillRoundRect(rx - bw, 5, bw, 20, 10, T->amber);
    text("DEMO", LABEL, rx - bw * 0.5f, 20, T->on_amber, AL_CENTER);
  }
}

// ============================================================================
// Static layers
// ============================================================================
namespace {
struct Slot {
  uint16_t *buf;
  uint32_t key;
  uint32_t used;  // LRU stamp
  bool valid;
};
Slot s_slots[2] = {};
uint32_t s_stamp = 0;
}  // namespace

uint16_t *staticLayer(uint32_t key, StaticFn render, const View &v) {
  ++s_stamp;
  for (Slot &s : s_slots) {
    if (s.valid && s.key == key) {
      s.used = s_stamp;
      return s.buf;
    }
  }
  Slot *s = &s_slots[0];
  for (Slot &c : s_slots) {
    if (!c.valid) { s = &c; break; }
    if (c.used < s->used) s = &c;
  }
  if (!s->buf) s->buf = gfx::layerCreate();
  if (!s->buf) return nullptr;
  // Render with a full clip into the layer, then restore target and clip.
  uint16_t *target = gfx::drawTarget();
  const gfx::ClipRect clip = gfx::getClip();
  gfx::drawTo(s->buf);
  gfx::clipReset();
  render(v);
  gfx::drawTo(target == gfx::framebuffer() ? nullptr : target);
  gfx::setClip(clip);
  s->key = key;
  s->used = s_stamp;
  s->valid = true;
  return s->buf;
}

// ============================================================================
// Formatting
// ============================================================================
void fmtGrouped(char *out, size_t n, double v, int decimals) {
  if (!(v >= 0.0)) v = 0.0;
  unsigned long long ip;
  unsigned frac = 0;
  // Truncated like a mechanical odometer (1 284.6 km reads "1 284"); the
  // epsilon absorbs float noise such as 18.39999.
  if (decimals > 0) {
    const unsigned long long tenths = (unsigned long long)floor(v * 10.0 + 1e-6);
    ip = tenths / 10;
    frac = (unsigned)(tenths % 10);
  } else {
    ip = (unsigned long long)floor(v + 1e-9);
  }
  char digits[24];
  snprintf(digits, sizeof(digits), "%llu", ip);
  const size_t len = strlen(digits);
  size_t o = 0;
  for (size_t i = 0; i < len && o + 2 < n; i++) {
    if (i && (len - i) % 3 == 0) out[o++] = ' ';
    out[o++] = digits[i];
  }
  out[o] = 0;
  if (decimals > 0 && o + 3 < n) snprintf(out + o, n - o, ".%u", frac);
}

void fmtRide(char *out, size_t n, uint32_t s) {
  if (s < 3600) snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  else snprintf(out, n, "%u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60));
}

}  // namespace ui
