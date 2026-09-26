// ui_draw.h - shared drawing helpers for the UI (internal to ui_*.cpp).
//
// Geometry follows design/mockups.html so the numbers in DESIGN_SPEC.md can be
// used verbatim:
//   * float coordinates use the canvas convention (pixel (x, y) covers
//     [x, x+1) x [y, y+1)) - identical to gfx's float geometry;
//   * angles are COMPASS degrees (0 = 12 o'clock, clockwise); the *C helpers
//     convert to gfx's 0 = 3 o'clock convention;
//   * arc/ring radii are CENTRE-LINE radii (gfx uses the outer radius);
//   * text x is the anchor of the advance box, y the baseline; glyph layout
//     (rounded advances, fractional tracking, tabular digit cells) replicates
//     the mockup's layout() so text lands on the same pixels.
#pragma once

#include <Arduino.h>

#include "fonts.h"
#include "gfx.h"
#include "ui.h"
#include "ui_theme.h"

namespace ui {

extern const Theme *T;  // theme of the frame being drawn

// Language pick: TR(v, "English", "Magyar")
inline const char *TR(const View &v, const char *en, const char *hu) { return v.hu ? hu : en; }

// ---------------------------------------------------------------- math / easing
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float easeOutCubic(float t) {
  t = clampf(t, 0.0f, 1.0f);
  const float u = 1.0f - t;
  return 1.0f - u * u * u;
}
inline float easeInOutCubic(float t) {
  t = clampf(t, 0.0f, 1.0f);
  return t < 0.5f ? 4.0f * t * t * t : 1.0f - (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * (-2.0f * t + 2.0f) * 0.5f;
}
// JavaScript Math.round (halves towards +infinity), as used by the mockups.
inline int jsRound(float v) { return (int)floorf(v + 0.5f); }

// Point on a circle, compass degrees.
void cpolar(float cx, float cy, float r, float deg, float &x, float &y);

// ---------------------------------------------------------------- text
enum Align : uint8_t { AL_LEFT = 0, AL_CENTER = 1, AL_RIGHT = 2 };

struct FontSpec {
  const gfx::Font *f;
  float track;  // default letter spacing (px, may be fractional)
  bool tab;     // tabular digits
};
extern const FontSpec LABEL, SMALL, BODY, BODYB, TITLE, NUM_M, NUM_L, NUM_XL, NUM_XXL;

constexpr float TRACK_DEFAULT = -1000.0f;

struct TextBox {
  int x0;   // left edge of the advance box
  float w;  // advance width
};
float textW(const char *s, const FontSpec &f, float track = TRACK_DEFAULT);
TextBox text(const char *s, const FontSpec &f, float x, int base, uint16_t c, Align a = AL_LEFT,
             float track = TRACK_DEFAULT, uint8_t alpha = 255);
// Value + unit laid out as one block (mockup valUnit). Returns the block width.
float valUnit(const char *val, const FontSpec &vf, const char *unit, const FontSpec &uf, float x, int base,
              Align a, uint16_t vc, uint16_t uc, float gap = 5.0f);
// Shortens `s` with an ellipsis to fit `maxw`.
void fitText(char *out, size_t n, const char *s, const FontSpec &f, float maxw);

// ---------------------------------------------------------------- shapes
// Background fill of the whole screen except the interiors of up to 8 cards
// (their r x r corner squares are filled): the cards are opaque, so this
// saves writing those pixels twice. The cards must be drawn afterwards.
void fillBgExcept(const Rect *cards, int n, int r);
// Rounded-rect stroke drawn inside the rect (fast path for integer radii).
void strokeRR(int x, int y, int w, int h, float r, float lw, uint16_t c, uint8_t a = 255);
// Card: filled rounded rect + stroke drawn inside.
void card(int x, int y, int w, int h, float r, uint16_t fill, uint16_t stroke, float lw = 1.0f,
          uint8_t strokeA = 255);
// Horizontal capsule x..x+w centred on y (mockup hcapsule).
void hcapsule(float x, float y, float w, float th, uint16_t c, uint8_t a = 255);
// Capsule (round-capped line) of full width w; axis-aligned pixel-aligned
// capsules are drawn as pills (fillRoundRect), which is much cheaper.
void capsule(float x0, float y0, float x1, float y1, float w, uint16_t c, uint8_t a = 255);
// Arc with centre-line radius r, compass angles d0 -> d1. Zero-length round
// arcs draw a dot.
void arcC(float cx, float cy, float r, float th, float d0, float d1, uint16_t c, bool round = true,
          uint8_t a = 255);
void arcGradC(float cx, float cy, float r, float th, float d0, float d1, const gfx::Stop *st, int n,
              float g0, float g1, bool round = true);
// Ring with centre-line radius r and line width lw.
inline void ring(float cx, float cy, float r, float lw, uint16_t c, uint8_t a = 255) {
  gfx::strokeCircle(cx, cy, r + lw * 0.5f, lw, c, a);
}
// Spec glow: alpha(d) = A * glowK * (1 - d/r)^2.
void glow(float cx, float cy, float r, uint16_t c, float A);
void poly(const float *xy, int n, uint16_t c, uint8_t a = 255);
// Motorsport panel: top-left and bottom-right corners cut by `ch` px.
void chamferPanel(int x, int y, int w, int h, float ch, uint16_t fill, uint16_t stroke);

// ---------------------------------------------------------------- icons & images
enum Icon : uint8_t {
  IC_WIFI18, IC_WIFI20, IC_SYSTEM20, IC_SKIN20, IC_SPEED20, IC_ODO20, IC_LANG20,
  IC_RESET20, IC_WARN20, IC_CHECK18, IC_CHECK20, IC_SUN16, IC_SUN22, IC_COUNT
};
void icon(Icon id, int x, int y, uint16_t c, uint8_t a = 255);  // top-left
// Skin thumbnail 144x96 with r=8 corners masked to `bg` (the card fill).
void thumb(int skin, bool light, int x, int y, uint16_t bg);
// Boot logo 320x167 (0x0000 transparent) with global alpha.
void logo(int x, int y, uint8_t alpha);

// ---------------------------------------------------------------- components
// Hold-to-reset progress ring (r=7, th=3) at centre (cx, cy).
void holdRing(float cx, float cy, float p);
// Stat label: LABEL text; while `hold` >= 0 it turns accent with a hold ring
// 14 px after it (before it for right-aligned labels).
TextBox statLabel(const char *label, float x, int base, Align a, float hold, uint16_t c);

// Top bar (page dots at fractional page position, title, WiFi + DEMO status).
void topBar(float pagePos, const char *title, bool wifi, bool wifiErr, bool demo);

// ---------------------------------------------------------------- static layers
// Expensive static backgrounds (CHRONO dial, APEX panels...) are rendered once
// into a PSRAM layer (2 cached slots, keyed by content) and copied each frame.
// Returns nullptr if no layer could be allocated (caller draws directly).
typedef void (*StaticFn)(const View &v);
uint16_t *staticLayer(uint32_t key, StaticFn render, const View &v);
// Key helper: content id + everything the static content depends on.
inline uint32_t staticKey(uint8_t id, const View &v) {
  return ((uint32_t)id << 8) | (v.light ? 1u : 0u) | (v.imperial ? 2u : 0u) | (v.hu ? 4u : 0u);
}

// ---------------------------------------------------------------- page renderers
// Animated display values shared by the pages (owned by ui_screens.cpp).
struct Anim {
  float gauge;       // eased speed for arcs / bars (user units)
  float needle;      // CHRONO needle (spring, may overshoot)
  float g;           // eased G
  int spd;           // displayed integer speed (<= 10 updates/s)
  float holdMax;     // hold ring progress incl. rewind, < 0 = hidden
  float holdTrip;
  float holdOdo;     // odometer reset fill (0..1)
  float knob;        // WiFi toggle knob 0 (off) .. 1 (on)
  float pulse;       // race READY dot glow 0..1 (1 Hz)
  float otaPct;      // eased upload percentage
};
void drawDash(const View &v, const Anim &a);         // ui_dash.cpp
bool dashTarget(const View &v, uint8_t t, Rect &r);  // ui_dash.cpp (touch rects of the skins)

// "RESET" flash after a hold-to-reset completes (800 ms).
inline bool resetFlash(const View &v, uint32_t at) { return at && v.now_ms - at < 800; }

// ---------------------------------------------------------------- formatting
// "1 284" / "1 284.6": `decimals` 0 or 1, space as thousands separator.
void fmtGrouped(char *out, size_t n, double v, int decimals);
// Ride time: m:ss below 1 h, h:mm from 1 h.
void fmtRide(char *out, size_t n, uint32_t s);

}  // namespace ui
