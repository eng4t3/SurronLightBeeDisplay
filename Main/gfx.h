// gfx.h - anti-aliased 2D engine for the 480x320 landscape dashboard.
//
// Draws straight into the Arduino_Canvas framebuffer (RGB565, native 320x480
// portrait, in PSRAM). All API coordinates are LANDSCAPE:
//   x: 0..479 left -> right, y: 0..319 top -> bottom.
// Landscape pixel (x, y) lives at native index  x * 320 + (319 - y), i.e. one
// landscape COLUMN is one contiguous native row. Every rasterizer therefore
// walks column by column (outer loop x, inner loop y) so memory access stays
// linear - the PSRAM cache would thrash on landscape rows.
//
// Conventions (see GFX_API.md for the full reference):
//   * Integer rect args: (x, y) = top-left pixel, (w, h) = size in pixels.
//   * Float geometry (circles, arcs, lines, polygons): pixel (x, y) covers the
//     square [x, x+1) x [y, y+1); its centre is (x+0.5, y+0.5). A circle at
//     cx=100.0 is centred on the corner between pixels 99 and 100; use 100.5
//     to centre it on pixel 100.
//   * Angles in DEGREES, 0 = 3 o'clock, increasing CLOCKWISE on screen
//     (90 = 6 o'clock, 180 = 9 o'clock, 270 = 12 o'clock). a1 may exceed 360
//     and a1 < a0 is swapped. Typical gauge: 135 .. 405 (270 deg, gap at the bottom).
//   * Alpha 0..255 everywhere; multiplied by the global alpha (setAlpha()).
//   * All edges are anti-aliased with a ~1 px linear coverage ramp (SDF).
//   * Colors are RGB565 as stored in the framebuffer (what Arduino_GFX
//     color565() returns); use gfx::rgb()/hex() to build them.
//
// Not thread safe: call from the UI task only.
#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace gfx {

constexpr int W = 480;  // landscape width
constexpr int H = 320;  // landscape height

// ============================================================================
// Colors
// ============================================================================
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
// 0xRRGGBB -> RGB565
constexpr uint16_t hex(uint32_t rgb888) {
  return rgb((uint8_t)(rgb888 >> 16), (uint8_t)(rgb888 >> 8), (uint8_t)rgb888);
}
// Channel expansion to 8 bits (bit replication, so 0x1F -> 255)
constexpr uint8_t red8(uint16_t c) { return (uint8_t)(((c >> 11) << 3) | (c >> 13)); }
constexpr uint8_t green8(uint16_t c) { return (uint8_t)((((c >> 5) & 63) << 2) | (((c >> 5) & 63) >> 4)); }
constexpr uint8_t blue8(uint16_t c) { return (uint8_t)(((c & 31) << 3) | ((c & 31) >> 2)); }

constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t WHITE = 0xFFFF;

// Linear blend: t = 0 -> a, t = 255 -> b (8-bit precision per channel).
uint16_t mix(uint16_t a, uint16_t b, uint8_t t);
// Same with a float t in 0..1 (clamped).
uint16_t lerp(uint16_t a, uint16_t b, float t);
// HSV -> RGB565. h in degrees (any value, wraps), s and v 0..255.
uint16_t hsv(float h, uint8_t s, uint8_t v);
// Multiply brightness: k = 256 keeps the color, 128 halves it (max 511).
uint16_t scale(uint16_t c, uint16_t k);

// Gradient stop: pos 0..1 along the gradient, stops sorted by pos.
struct Stop {
  float pos;
  uint16_t color;
};
// Color at t (0..1) of a multi-stop gradient (clamped at the ends).
uint16_t gradientAt(const Stop *stops, int n, float t);

// ============================================================================
// Setup, clipping, global alpha
// ============================================================================
// fb = Arduino_Canvas::getFramebuffer() (native 320x480 RGB565).
void begin(uint16_t *fb);
uint16_t *framebuffer();
// Re-point drawing to another buffer, keeping clip/alpha (used by the
// double-buffered lcd::present()).
void setFramebuffer(uint16_t *fb);

struct ClipRect {
  int16_t x0, y0, x1, y1;  // x1/y1 exclusive
};
// Restrict drawing to a rect (intersected with the screen, NOT with the
// previous clip). Save/restore with getClip()/setClip().
void clip(int x, int y, int w, int h);
void clipReset();
ClipRect getClip();
void setClip(const ClipRect &c);

// Global alpha multiplied into every draw call (255 = opaque, default).
void setAlpha(uint8_t a);
uint8_t getAlpha();

// ============================================================================
// Background
// ============================================================================
void clear(uint16_t c);  // whole draw target, ignores clip and alpha
// Copy the current frame into a PSRAM snapshot / copy it back (full screen,
// ignores clip). Typical: draw the static background once, bgCapture(),
// then start each frame with bgRestore(). Returns false if the 300 KB
// PSRAM buffer can't be allocated / no snapshot exists yet.
bool bgCapture();
bool bgRestore();
void bgFree();

// ============================================================================
// Layers (off-screen full-screen buffers, same layout as the screen)
// ============================================================================
// Use them to pre-render expensive static content once (a dial face with
// ticks and labels, a gauge ring with its glow...) and copy the needed part
// to the screen every frame (copyRect ~12 ns/px; arcCopy costs about as much
// as arcGradient but the layer may contain anything):
//   L = layerCreate(); drawTo(L); clear(bg); arcGradient(...); arcGlow(...);
//   drawTo(nullptr);                                     // back to the screen
//   per frame: arcCopy(L, cx, cy, r + spread, t + 2*spread, a0, aValue, true);
uint16_t *layerCreate();          // 300 KB PSRAM, nullptr if out of memory
void layerFree(uint16_t *layer);
void drawTo(uint16_t *layer);     // redirect all drawing; nullptr = the screen
uint16_t *drawTarget();           // current target (screen or layer)
// Copy a rect / an arc-shaped region (AA edges, same geometry rules as arc())
// from a layer to the current target.
void copyRect(const uint16_t *layer, int x, int y, int w, int h);
void arcCopy(const uint16_t *layer, float cx, float cy, float rOuter, float thickness, float a0,
             float a1, bool roundCaps = false, uint8_t alpha = 255);

// ============================================================================
// Rectangles and gradients (integer, pixel aligned)
// ============================================================================
void pixel(int x, int y, uint16_t c, uint8_t alpha = 255);
void fillRect(int x, int y, int w, int h, uint16_t c);
void fillRectAlpha(int x, int y, int w, int h, uint16_t c, uint8_t alpha);
void hLine(int x, int y, int w, uint16_t c, uint8_t alpha = 255);  // 1 px
void vLine(int x, int y, int h, uint16_t c, uint8_t alpha = 255);  // 1 px
// Linear gradients, ordered-dithered (4x4 Bayer) to hide RGB565 banding.
void vGradient(int x, int y, int w, int h, uint16_t top, uint16_t bottom, uint8_t alpha = 255);
void hGradient(int x, int y, int w, int h, uint16_t left, uint16_t right, uint8_t alpha = 255);
// Radial gradient over a rect: t = distance/r (clamped to 1) maps inner->outer.
// Dithered, integer LUT based (~11 ms full screen, clear() is ~6 ms).
void radialGradient(int x, int y, int w, int h, float cx, float cy, float r,
                    uint16_t inner, uint16_t outer);

// ============================================================================
// Rounded rectangles and circles (AA)
// ============================================================================
// r is clamped to min(w,h)/2. r = h/2 gives a pill.
void fillRoundRect(int x, int y, int w, int h, float r, uint16_t c, uint8_t alpha = 255);
// Vertical gradient fill (dithered).
void fillRoundRectGradient(int x, int y, int w, int h, float r, uint16_t top, uint16_t bottom,
                           uint8_t alpha = 255);
// Border of `thickness` px drawn INSIDE the rect (inner radius = r - thickness).
void strokeRoundRect(int x, int y, int w, int h, float r, float thickness, uint16_t c,
                     uint8_t alpha = 255);

void fillCircle(float cx, float cy, float r, uint16_t c, uint8_t alpha = 255);
// Ring with OUTER radius r, `thickness` px wide towards the centre
// (same convention as arc()).
void strokeCircle(float cx, float cy, float r, float thickness, uint16_t c, uint8_t alpha = 255);

// ============================================================================
// Arcs / rings (AA on both radii and on the caps)
// ============================================================================
// Band between radii (rOuter - thickness) .. rOuter, from angle a0 to a1
// clockwise. roundCaps adds semicircular caps of radius thickness/2 centred
// on the mid radius at a0/a1 (they extend BEYOND a0/a1 by thickness/2 along
// the arc - shorten the sweep if the cap must end exactly on a tick).
void arc(float cx, float cy, float rOuter, float thickness, float a0, float a1, uint16_t c,
         bool roundCaps = false, uint8_t alpha = 255);

// Color as a function of ANGLE. The gradient runs from ga0 (stop pos 0) to
// ga1 (stop pos 1) and is independent of the drawn sweep a0..a1, so a
// growing value arc keeps each color at a fixed position on the dial.
// Angles outside ga0..ga1 clamp to the end colors. For a gradient over just
// the drawn sweep pass ga0 = a0, ga1 = a1.
void arcGradient(float cx, float cy, float rOuter, float thickness, float a0, float a1,
                 const Stop *stops, int nStops, float ga0, float ga1, bool roundCaps = false,
                 uint8_t alpha = 255);
// Callback variant: color(t, user) with t 0..1 over ga0..ga1 (sampled into a
// 256-entry LUT once per call, so the callback may be slow).
typedef uint16_t (*ColorFn)(float t, void *user);
void arcGradientFn(float cx, float cy, float rOuter, float thickness, float a0, float a1,
                   ColorFn fn, void *user, float ga0, float ga1, bool roundCaps = false,
                   uint8_t alpha = 255);

// Soft halo around an arc: full `intensity` inside the band, fading to 0 at
// `spread` px outside it (quadratic falloff). Draw BEFORE the arc itself.
// EXPENSIVE (~13 ms for r150/t28/180deg/spread 12, ~0.5 us per halo pixel).
// Prefer glow() at the arc tip, a small spread, or pre-render into a layer.
void arcGlow(float cx, float cy, float rOuter, float thickness, float a0, float a1, uint16_t c,
             float spread, uint8_t intensity, bool roundCaps = true);

// Radial glow: alpha = intensity * (1 - d^2/r^2)^2 for d < r. Cheap (no sqrt).
void glow(float cx, float cy, float r, uint16_t c, uint8_t intensity);
// Radial glow with the design spec's falloff: alpha = intensity * (1 - d/r)^2
// (tighter core than glow()). LUT based, about the cost of glow().
void glowSoft(float cx, float cy, float r, uint16_t c, uint8_t intensity);

// ============================================================================
// Lines and polygons (AA)
// ============================================================================
// Thick line with round caps (a capsule). width is the full width.
void line(float x0, float y0, float x1, float y1, float width, uint16_t c, uint8_t alpha = 255);
// Convex polygon, xy = {x0,y0, x1,y1, ...}, n = vertex count (3..16), any winding.
void fillPolygon(const float *xy, int n, uint16_t c, uint8_t alpha = 255);
void fillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c,
                  uint8_t alpha = 255);
// Tapered needle from radius rBack (negative = behind the centre) to rTip at
// `deg`, widths wBack at the back and wTip at the tip (round tip if wTip > 0).
void needle(float cx, float cy, float deg, float rBack, float rTip, float wBack, float wTip,
            uint16_t c, uint8_t alpha = 255);
// Point on a circle (degrees, same convention as arcs).
void polar(float cx, float cy, float r, float deg, float &x, float &y);

// ============================================================================
// Images
// ============================================================================
// Row-major landscape RGB565 image. key >= 0: pixels equal to key are skipped.
void image565(int x, int y, int w, int h, const uint16_t *px, int32_t key = -1);
// RGB565 image stored in the framebuffer's NATIVE order: w columns, each h
// pixels from the bottom row up. Every column is one memcpy, so this is much
// faster than image565() for images in flash (no strided reads). Respects the
// clip and the global alpha.
void imageNative(int x, int y, int w, int h, const uint16_t *cols);
// 4-bit alpha mask, row-major, rows padded to whole bytes ((w+1)/2 bytes per
// row), high nibble = left pixel. 0 = transparent, 15 = opaque.
void mask4(int x, int y, int w, int h, const uint8_t *a4, uint16_t c, uint8_t alpha = 255);

// ============================================================================
// Text
// ============================================================================
// Generated by tools/fontgen.py (see fonts.h). Bitmaps are 4 bpp,
// column-major, continuous nibble stream (high nibble first).
struct Glyph {
  uint32_t off;      // byte offset of the glyph's first nibble pair in Font::bits
  uint16_t cp;       // Unicode codepoint
  uint8_t w, h;      // bitmap size (0 for blank glyphs such as space)
  int16_t xoff;      // bitmap left relative to the pen x
  int16_t yoff;      // bitmap top relative to the baseline (negative = above)
  uint16_t adv;      // pen advance in px
};
struct Font {
  const uint8_t *bits;
  const Glyph *glyphs;  // sorted by cp
  uint16_t count;
  uint16_t size;          // em size in px (the "font-size")
  uint16_t ascent;        // baseline to top of line box
  uint16_t descent;       // baseline to bottom of line box
  uint16_t lineHeight;    // ascent + descent: baseline-to-baseline distance
  uint16_t capHeight;     // height of 'H' above the baseline
  uint16_t xHeight;       // height of 'x'
  uint16_t digitHeight;   // height of '0'
  uint16_t digitAdvance;  // widest digit advance (used by TABULAR)
  uint16_t spaceAdvance;
  const char *name;
};

// Text flags (combine with |). Horizontal: LEFT/CENTER/RIGHT relative to x.
// Vertical: BASELINE (default, y = baseline), TOP (y = top of capitals),
// MIDDLE (y = middle of capitals), BOTTOM (y = bottom of the descent).
// TABULAR: every digit 0-9 advances by Font::digitAdvance (glyph centred in
// its cell) so changing numbers don't jiggle.
enum TextFlags : uint8_t {
  LEFT = 0x00,
  CENTER = 0x01,
  RIGHT = 0x02,
  BASELINE = 0x00,
  TOP = 0x04,
  MIDDLE = 0x08,
  BOTTOM = 0x0C,
  TABULAR = 0x10,
};
constexpr uint8_t H_MASK = 0x03;
constexpr uint8_t V_MASK = 0x0C;

// Draws UTF-8 text. `tracking` = extra px between characters (letter
// spacing, may be negative). Unknown characters render as '?' (or are
// skipped if the font has no '?'). Returns the advance width.
int text(int x, int y, const char *utf8, const Font &f, uint16_t c, uint8_t flags = LEFT,
         uint8_t alpha = 255, int tracking = 0);
// Advance width (sum of advances + tracking between characters).
int textWidth(const char *utf8, const Font &f, uint8_t flags = LEFT, int tracking = 0);
// Copy `utf8` into `out`, shortened with an ellipsis so it fits `maxw` px.
// Returns true if it had to be shortened.
bool textFit(char *out, size_t n, const char *utf8, const Font &f, int maxw, int tracking = 0,
             uint8_t flags = LEFT);
// Baseline y for a vertical flag at y (what text() uses internally).
int baselineFor(int y, const Font &f, uint8_t flags);
// Glyph lookup (binary search); nullptr if missing.
const Glyph *findGlyph(const Font &f, uint32_t cp);
// Decodes one UTF-8 codepoint and advances *s (invalid bytes -> U+FFFD).
uint32_t utf8Next(const char **s);

// ============================================================================
// Vector icons. (cx, cy) = centre, `size` = nominal height in px.
// ============================================================================
namespace icon {
// bars = number of lit arcs (0..3); unlit arcs use `dim` (skip if dim == c).
void wifi(float cx, float cy, float size, uint16_t c, int bars = 3, uint16_t dim = 0,
          bool drawDim = false);
void check(float cx, float cy, float size, uint16_t c, float stroke = 0);
void cross(float cx, float cy, float size, uint16_t c, float stroke = 0);
void gear(float cx, float cy, float size, uint16_t c, uint16_t hole);
void bolt(float cx, float cy, float size, uint16_t c);
void flag(float cx, float cy, float size, uint16_t c);
void stopwatch(float cx, float cy, float size, uint16_t c, uint16_t face);
void chevron(float cx, float cy, float size, uint16_t c, bool right = true, float stroke = 0);
}  // namespace icon

}  // namespace gfx
