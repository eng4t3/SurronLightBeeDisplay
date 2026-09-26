# gfx — anti-aliased rendering engine (reference)

Everything the new UI needs to draw: `gfx.h` (engine), `fonts.h` (generated fonts),
`lcd_flush.h` (panel transfer), `debug_console.h` (serial console). Namespace `gfx`.

## 1. Frame loop (already wired in `Main.ino` → `UITask`)

```cpp
gfx::begin(display.gfx->getFramebuffer());   // once, after display.begin()
lcd::begin(display);                          // fast async flush (double buffered)
dbg::begin(appDebugCommand, display.gfx);     // serial console
// every frame (33 ms period):
dbg::poll();  updateTelemetry();  handleTouch();
if (!dbg::render()) renderUI();               // your drawing, ALL of it via gfx::
dbg::frameRendered(draw_us);                  // serves `shot`
lcd::present();                               // hand frame to core 0, draw next one
```

* **Repaint the whole screen every frame.** `lcd::present()` is double buffered: the
  buffer you draw into holds the frame *before last*. Start each frame with `clear()`,
  `radialGradient()`, `bgRestore()` or a full-screen fill.
* The panel transfer (≈25 ms, SPI clock is 26.7 MHz) runs on core 0 in parallel with
  drawing, so the **drawing budget is ≈28 ms per frame at 30 fps** (drawing runs ~10 %
  slower while the transfer competes for PSRAM; the bench numbers below are without it).
* UI task only — nothing in gfx is thread safe.
* Don't call `display.flush()` or Arduino_GFX drawing: after `lcd::present()` the library
  flush would race with the flush task. All UI drawing goes through `gfx::` (`ui_*.cpp`).

## 2. Conventions

| Topic | Rule |
|---|---|
| Coordinates | Landscape 480×320 (`gfx::W`, `gfx::H`), x → right, y → down. |
| Integer rects | `(x, y)` = top-left pixel, `(w, h)` = size. Pixel-aligned, crisp edges. |
| Float geometry | Pixel (x, y) covers [x, x+1)×[y, y+1); its centre is (x+.5, y+.5). A circle at `cx = 100.0` is centred on the pixel corner; use `100.5` to centre on pixel 100. |
| Angles | **Degrees, 0 = 3 o'clock, clockwise on screen** (90 = 6 o'clock, 180 = 9, 270 = 12). `a1` may exceed 360; `a1 < a0` is swapped. Classic gauge: `135 → 405` (270° sweep, gap at the bottom). `polar(cx,cy,r,deg,x,y)` uses the same convention. |
| Arc radius | `rOuter` is the OUTER edge; the band goes `thickness` px inwards. Same for `strokeCircle`. |
| Alpha | 0..255 everywhere, multiplied by the global `setAlpha()`. |
| AA | Every curved/diagonal edge has a ~1 px linear coverage ramp (SDF). Axis-aligned integer rect edges are exact. A 1 px wide diagonal line looks lighter than a 1 px horizontal one (normal for AA) — use ≥1.5 px for hairlines. |
| Colors | RGB565 as stored in the framebuffer (= Arduino_GFX `color565`). Build them with `gfx::rgb(r,g,b)` or `gfx::hex(0xRRGGBB)` (both `constexpr`). Verified on device: R/B not swapped. |
| Clipping | `clip(x,y,w,h)` / `clipReset()` / `getClip()` / `setClip()`; everything respects it (except `clear`, `bg*`). |

## 3. API

### Colors
```cpp
constexpr uint16_t rgb(r, g, b);  constexpr uint16_t hex(0xRRGGBB);
uint8_t red8(c), green8(c), blue8(c);          // expand to 0..255
uint16_t mix(a, b, uint8_t t);                 // t 0 → a, 255 → b
uint16_t lerp(a, b, float t);                  // t 0..1
uint16_t hsv(float hDeg, uint8_t s, uint8_t v);
uint16_t scale(c, uint16_t k);                 // brightness, 256 = unchanged
struct Stop { float pos; uint16_t color; };    // gradient stop, pos 0..1, sorted
uint16_t gradientAt(const Stop*, int n, float t);
```

### State / background
```cpp
void begin(uint16_t *fb);  uint16_t *framebuffer();   // current screen back buffer
void setAlpha(uint8_t);   uint8_t getAlpha();
void clear(uint16_t c);                               // full screen, ~6.5 ms
bool bgCapture();  bool bgRestore();  void bgFree();  // full-screen PSRAM snapshot, restore ~9.8 ms
```

### Rects, gradients (integer)
```cpp
void pixel(x, y, c, alpha=255);
void fillRect(x, y, w, h, c);            void fillRectAlpha(x, y, w, h, c, alpha);
void hLine(x, y, w, c, alpha=255);       void vLine(x, y, h, c, alpha=255);
void vGradient(x, y, w, h, top, bottom, alpha=255);     // 4x4 Bayer dithered
void hGradient(x, y, w, h, left, right, alpha=255);     // dithered
void radialGradient(x, y, w, h, float cx, float cy, float r, inner, outer);  // t = dist/r
```

### Rounded rects & circles (AA)
```cpp
void fillRoundRect(x, y, w, h, float r, c, alpha=255);          // r clamped to min(w,h)/2; r = h/2 → pill
void fillRoundRectGradient(x, y, w, h, float r, top, bottom, alpha=255);
void strokeRoundRect(x, y, w, h, float r, float thickness, c, alpha=255);  // border INSIDE the rect
void fillCircle(float cx, float cy, float r, c, alpha=255);
void strokeCircle(float cx, float cy, float rOuter, float thickness, c, alpha=255);
```

### Arcs / rings
```cpp
void arc(cx, cy, rOuter, thickness, a0, a1, c, bool roundCaps=false, alpha=255);
void arcGradient(cx, cy, rOuter, thickness, a0, a1, const Stop*, int n,
                 float ga0, float ga1, bool roundCaps=false, alpha=255);
void arcGradientFn(cx, cy, rOuter, thickness, a0, a1, ColorFn fn, void *user,
                   float ga0, float ga1, bool roundCaps=false, alpha=255);  // fn(t 0..1, user)
void arcGlow(cx, cy, rOuter, thickness, a0, a1, c, float spread, uint8_t intensity, bool roundCaps=true);
void glow(cx, cy, r, c, uint8_t intensity);     // radial, alpha = I·(1-d²/r²)²
void glowSoft(cx, cy, r, c, uint8_t intensity); // radial, alpha = I·(1-d/r)² (design spec falloff)
```
* Round caps are semicircles of radius `thickness/2` centred on the mid radius at a0/a1 —
  they extend `thickness/2` **beyond** a0/a1 along the arc.
* `arcGradient`: the color is a function of the ANGLE. Stop pos 0 sits at `ga0`, pos 1 at
  `ga1`, independent of the drawn sweep → a growing value arc keeps each color at a fixed
  place on the dial (`ga0=135, ga1=405` for a 270° gauge). For a gradient over just the
  drawn part pass `ga0=a0, ga1=a1`. Outside ga0..ga1 the end colors are used. A full
  360° gradient ring is seamless if the first and last stop colors match.
* One call = one seamless band. Don't build a continuous arc from several adjacent
  `arc()` calls (AA edges of neighbours overlap); segmented gauges with gaps are fine.
* Tested at thickness 3–28, radius 8–150, spans 0–360° incl. crossing 0°/360°.

### Lines, needles, polygons (AA)
```cpp
void line(x0, y0, x1, y1, float width, c, alpha=255);           // round caps (capsule)
void needle(cx, cy, deg, rBack, rTip, wBack, wTip, c, alpha=255);  // tapered, round ends; rBack<0 = tail
void fillPolygon(const float *xy, int n, c, alpha=255);         // CONVEX, 3..16 vertices, any winding
void fillTriangle(x0, y0, x1, y1, x2, y2, c, alpha=255);
void polar(cx, cy, r, deg, float &x, float &y);
```
Concave shapes: split into convex pieces (see `icon::bolt`).

### Layers (off-screen buffers, same layout as the screen, 300 KB PSRAM each)
```cpp
uint16_t *layerCreate();  void layerFree(uint16_t*);
void drawTo(uint16_t *layer);   // redirect ALL drawing; drawTo(nullptr) = back to the screen
uint16_t *drawTarget();
void copyRect(const uint16_t *layer, x, y, w, h);       // ~12 ns/px (memcpy per column)
void arcCopy(const uint16_t *layer, cx, cy, rOuter, thickness, a0, a1, roundCaps=false, alpha=255);
```
Use for complex static content (dial face with ticks and labels, a gauge ring with its glow):
render once into a layer, then `copyRect()` it each frame. `arcCopy()` reveals an arc-shaped
part of a layer with AA edges — about the cost of `arcGradient`, but the layer can contain
anything (gradient + glow + ticks baked in).

### Images
```cpp
void image565(x, y, w, h, const uint16_t *px, int32_t key=-1);  // row-major RGB565; key = transparent color
void imageNative(x, y, w, h, const uint16_t *cols);              // native order: columns, bottom row first
                                                                 // (1 memcpy per column, fast from flash)
void mask4(x, y, w, h, const uint8_t *a4, c, alpha=255);         // 4-bit alpha, row-major,
                                                                 // (w+1)/2 bytes per row, high nibble = left
```

### Text
```cpp
int  text(x, y, const char *utf8, const Font &f, c, uint8_t flags=LEFT, alpha=255, int tracking=0);  // returns width
int  textWidth(utf8, f, flags=LEFT, tracking=0);
bool textFit(char *out, size_t n, utf8, f, int maxw, tracking=0, flags=LEFT);  // ellipsis "…"
int  baselineFor(y, f, flags);   const Glyph *findGlyph(f, cp);   uint32_t utf8Next(const char**);
```
Flags (combine with `|`):

| Horizontal | `LEFT` (default), `CENTER`, `RIGHT` — relative to x |
|---|---|
| Vertical | `BASELINE` (default, y = baseline), `TOP` (y = top of capitals: baseline = y + capHeight), `MIDDLE` (y = middle of capitals), `BOTTOM` (y = bottom of descent) |
| `TABULAR` | digits 0–9 all advance `digitAdvance` (glyph centred in the cell) → numbers don't jiggle. Use for every changing number. |

`tracking` = extra px between characters (small-caps labels: `F_LABEL` with 1–3).
UTF-8 is decoded; missing glyphs draw `?`. Width = sum of advances + tracking×(n-1).

### Icons (`gfx::icon`, centre + nominal size in px)
`wifi(cx,cy,size,c,bars=3,dim,drawDim)`, `check(cx,cy,size,c,stroke=0)`, `cross(...)`,
`gear(cx,cy,size,c,holeColor)`, `bolt(cx,cy,size,c)`, `flag(cx,cy,size,c)` (chequered),
`stopwatch(cx,cy,size,c,faceColor)`, `chevron(cx,cy,size,c,right=true,stroke=0)`.
Stroke defaults to 0.14·size.

## 4. Fonts (`#include "fonts.h"`)

Barlow Semi Condensed (OFL, `tools/fonts/OFL.txt`), 4-bit anti-aliased, hinting-free
(rendered at 4× and box-filtered). **Size = em size in px (CSS/Figma font-size)**; Barlow
capitals are ~0.70 em. All metrics in px:

| Font | Weight | em | ascent | descent | lineHeight | capHeight | digitHeight | digitAdvance | Charset | Flash |
|---|---|---|---|---|---|---|---|---|---|---|
| `F_LABEL` | SemiBold | 13 | 13 | 3 | 16 | 9 | 9 | 7 | full | 4.8 KB |
| `F_SMALL` | Medium | 15 | 15 | 3 | 18 | 10 | 10 | 8 | full | 5.7 KB |
| `F_BODY` | Medium | 18 | 18 | 4 | 22 | 12 | 13 | 9 | full | 7.2 KB |
| `F_BODYB` | SemiBold | 18 | 18 | 4 | 22 | 13 | 13 | 9 | full | 7.5 KB |
| `F_TITLE` | SemiBold | 24 | 24 | 5 | 29 | 17 | 17 | 13 | full | 11.3 KB |
| `F_NUM_M` | Bold | 32 | 32 | 7 | 39 | 22 | 23 | 17 | full | 19.2 KB |
| `F_NUM_L` | Bold | 56 | 56 | 12 | 68 | 39 | 40 | 30 | num | 6.5 KB |
| `F_NUM_XL` | Bold | 128 | 128 | 26 | 154 | 90 | 91 | 70 | num | 31.8 KB |
| `F_NUM_XXL` | Bold | 168 | 168 | 34 | 202 | 118 | 120 | 91 | digits | 42.6 KB |

Total 134 KB flash. *full* = ASCII 32–126 + `° · – — … −` + `áéíóöőúüűÁÉÍÓÖŐÚÜŰ`;
*num* = `0-9 . : - % +` and space; *digits* = `0-9` only.
`ascent` equals the em size in this family, so position text with `TOP`/`MIDDLE`
(capHeight based) rather than ascent. Every `gfx::Font` also has `xHeight`, `spaceAdvance`, `size`.

Regenerate / change sizes: edit `FONTS` in `tools/fontgen.py`, run `python tools/fontgen.py`
(`--preview` writes PNGs to `tools/fonts/preview/`). Output: `Main/font_*.h` + `Main/fonts.h`.

## 5. Performance (measured on the device, `bench`, µs per call, no concurrent flush)

| Primitive | µs | | Primitive | µs |
|---|---|---|---|---|
| `clear` (full screen) | 6 560 | | `arc` r150 t28 270° round | 2 900 |
| `fillRect` 480×320 | 6 330 | | `arc` r150 t28 360° | 2 370 |
| `vGradient` 480×320 | 6 660 | | `arcGradient` r150 t28 270° | 6 950 |
| `radialGradient` 480×320 | 10 500 | | `arcGradient` r150 t28 180° | 5 250 |
| `bgRestore` | 9 760 | | `arcGlow` r150 t28 180° spread 12 | 12 900 |
| `fillRoundRect` 140×70 r14 | 300 | | `glow` r40 | 950 |
| `fillRoundRectGradient` 140×70 | 450 | | `arcCopy` r162 t52 270° | 6 850 |
| `strokeRoundRect` 140×70 t1 | 540 | | `copyRect` 140×70 | 115 |
| `fillCircle` r60 | 375 | | `line` 120 px w3 | 390 |
| `strokeCircle` r100 t2 | 1 260 | | `needle` 120 px | 610 |
| text `F_NUM_XL` "188" | 1 070 | | `fillTriangle` ~40 px | 420 |
| text `F_NUM_XXL` "88" | 1 390 | | `icon::wifi` 26 | 400–490 |
| text `F_NUM_M` "1234.5" | 170 | | text `F_BODY` 30 chars | 300 |
| text `F_LABEL` 8 chars | 74 | | test card 0 (everything) | 41 000 |

Rules of thumb: PSRAM-bound fills cost ~40 ns/px; AA edge pixels ~40 cycles each; the
FPU is slow (sqrt ≈ 50–90 cycles, division ≈ 70), so the engine avoids per-pixel sqrt/div.
Cost of an arc ∝ its area; `arcGlow` costs ~0.5 µs per halo pixel — keep spreads small,
use `glow()` at the tip, or bake the glow into a layer. A typical dashboard frame
(clear 6.5 + track arc 3 + value arcGradient 5 + glow tip 1 + hero digits 1 + 4 cards 2 +
labels 1) ≈ 20 ms → fits 30 fps.

Panel: the old `display.flush()` took 46 ms (CPU swap then wait, no overlap → 14 fps).
`lcd::present()` does it on core 0 (swap 14 ms + bus 25 ms at 26.7 MHz QSPI, overlapped)
and returns in ~0 ms.

## 6. Debug console (USB CDC `Serial`, 115200, one command per line)

Every command ends with `OK <cmd>` or `ERR <reason>`.

| Command | Effect |
|---|---|
| `shot` | `SHOT 480 320\n` + 307 200 bytes RGB565 **little endian**, row-major landscape + `\nEND\n` (frame as rendered, before transfer) |
| `fps [reset]` | avg/max frame period, draw time, present time over 64 frames + panel transfer stats |
| `screen dash\|race\|settings`, `tab 0-5` (also opens settings), `skin 0-3`, `theme dark\|light`, `lang en\|hu`, `units metric\|imperial`, `wifi on\|off`, `state` | UI state (not saved to NVS by the console) |
| `tap x y`, `hold x y ms`, `swipe left\|right [y=190]` | injected touch (acts exactly like a finger — a tap on a reset button resets!) |
| `speed <kmh\|off>`, `accel <g\|off>` | fake display speed / G. Never touches odometer, ride time, max speed, race timer or NVS |
| `gfxtest [0-3\|off]` | engine test cards (0 dashboard primitives, 1 fonts, 2 misc, 3 arc edge cases); held until a UI command |
| `bench` | the table above |
| `selftest` | verifies landscape→framebuffer mapping and color565 == `gfx::rgb` |
| `flush [fast\|lib]` | transfer stats; `lib` switches back to the library flush (sync, 46 ms) |
| `mock on\|off\|wifi\|demo`, `mock race ready\|pulling\|finished\|stop\|off`, `mock ota progress\|success\|error\|off`, `mock hold max\|trip\|odo <0..1\|off>` | display-only sample data of the design mockups (ride 42:18, trip 18.4, odo 1 284.6, max 74, peak G 0.52), fake status icons, race / update overlay states and hold-to-reset progress, for 1:1 comparison with `design/png` |
| `help`, `ping` | |

Screenshot tool (pyserial + Pillow; opening the port does not reset the board):
```
python tools/devshot.py COM9 out.png --cmd "screen race" --cmd "speed 57"
python tools/devshot.py COM9 --all shots/     # every screen/skin at 0/35/70 km/h, every tab, dark+light
python tools/devshot.py COM9 --cmd fps --cmd bench
```
New screens: keep the app handler `appDebugCommand()` in `Main.ino` in sync with the state
variables so `screen/tab/skin/...` and `devshot --all` keep working.
