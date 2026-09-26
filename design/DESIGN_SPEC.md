# WheelieAssist UI 5 — Design Spec (implementation contract)

Target: Guition JC3248W535, 3.5" IPS, **480×320 landscape, RGB565**, capacitive touch, 30 fps full-frame redraw.
Source of truth: `design/mockups.html` (Canvas2D). Every number in the generated tables below comes out of the
same constants and draw calls the mockups use (`python design/serve.py`, open
`http://127.0.0.1:8765/design/mockups.html`, press **Export PNGs + spec**). Reference renders (RGB565-quantised,
1:1) are in `design/png/`.

Sections marked *generated* are rewritten on export. Don't edit them by hand; change the mockup code and re-export.

---

## 0. Conventions

| Thing | Rule |
|---|---|
| Coordinates | Integer px, origin top-left, x → right, y → down. Screen 480×320. |
| Text position | `x` = anchor of the **advance box** (left / centre / right edge per `align`), `base` = **baseline** y. The advance box = sum of rounded glyph advances + tracking between glyphs (not after the last one). Glyph origin x is rounded to an integer. No kerning. |
| Tabular digits | For fonts marked *tabular*, every digit 0–9 occupies the same cell (= widest digit advance, see font table) and is centred in it. `.` `:` `-` `%` `+` space keep their own advance. So the text never wobbles while numbers change. |
| Angles | **Compass degrees**: 0° = 12 o'clock, positive = clockwise. `polar(cx,cy,r,d) = (cx + r·sin d, cy − r·cos d)`. |
| Arc radius `r` | Centre-line radius; the band spans r ± th/2. Round caps = half-discs of diameter th at both ends. A zero-length round-cap arc is a dot. |
| Angle gradient | Colour interpolated linearly in RGB along the **full sweep** of the gauge (stops in % of the full sweep, not of the value), so the colour at a given speed never changes. Cap pixels use the colour of the nearest arc end. |
| Stroke of rrect | Drawn **inside** the rect (outer edge = rect edge). |
| Glow | `alpha(d) = A · (1 − d/r)²` for d < r, blended over what's below. Light theme uses A × 0.55. Keep glows small (r ≤ 34) — large low-alpha glows band in RGB565 (they were removed from backgrounds for this reason). |
| `@NN%` | Colour token drawn with alpha NN %. |
| Polygons | All polygons are convex; draw order as listed. |

## 1. Concept

One restrained visual language across every screen: near-black blue-tinted background, dark cards with 1 px
hairline strokes, white hero numerals, **one accent (Ion Blue)**, and three signal colours (green / amber / red)
that only ever mean something. Labels are small uppercase tracked SemiBold; values are Bold with tabular digits.

### Skins

| # | Name | Concept |
|---|---|---|
| 1 | **HALO** | Premium 270° ring gauge on the left with an angle-gradient value arc and a glowing comet head, plus a clean "spec sheet" column (MAX / RIDE TIME / TRIP / ODO) on the right. |
| 2 | **PURE** | Ultra-minimal: one enormous 168 px number, a hairline zone-gradient speed bar, and a single quiet row of four stats. Nothing else. |
| 3 | **CHRONO** | Luxury-watch dial: brushed-steel bezel, applied baton indices with lume lines, minute track, redline arcs, tapered accent needle with shadow, digital "date window" speed, data in the four corners like a watch case. |
| 4 | **APEX** | Track telemetry HUD: 32-segment shift-light bar, big right-aligned speed, longitudinal-G semicircle with peak marker and a 4 s G-trace sparkline, chamfered motorsport panels. |

Gauge full scale: **85 km/h** (metric) / **50 mph** (imperial). `pct = clamp(v / fullScale, 0, 1)`.
Scale numerals for imperial: HALO/CHRONO ticks every 5 mph with majors every 10 (0…50), CHRONO numerals 0,10…50;
APEX scale labels 0/10/20/30/40/50.

Speed text: integer, `lroundf`. Values > 99 render "99" (the LBX can't get there; keeps layouts fixed).

## 2. Foundations

### 2.1 Colour tokens *(generated)*

<!-- GEN:tokens -->
| token | dark hex | dark RGB565 | light hex | light RGB565 | use |
|---|---|---|---|---|---|
| `bg` | `#080C10` | `0x0862` | `#EFF3F7` | `0xEF9E` | Screen background |
| `surface` | `#101418` | `0x10A3` | `#FFFFFF` | `0xFFFF` | Cards, tiles, panels |
| `surface_hi` | `#181C21` | `0x18E4` | `#E7EBEF` | `0xE75D` | Pressed / raised / inactive knob |
| `border` | `#212831` | `0x2146` | `#CED3DE` | `0xCE9B` | Hairlines, card strokes |
| `track` | `#182029` | `0x1905` | `#DEE3E7` | `0xDF1C` | Gauge / slider / bar tracks, unlit segments |
| `text_hi` | `#F7FBFF` | `0xF7DF` | `#000408` | `0x0021` | Hero numbers, primary values |
| `text` | `#D6DBDE` | `0xD6DB` | `#182431` | `0x1926` | Body text |
| `text_dim` | `#8C9AA5` | `0x8CD4` | `#4A5563` | `0x4AAC` | Labels, units, secondary |
| `text_faint` | `#525D6B` | `0x52ED` | `#8C96A5` | `0x8CB4` | Decorative / disabled only (never info) |
| `accent` | `#21C7FF` | `0x263F` | `#0079C6` | `0x03D8` | Primary accent, speed zone 1, selection |
| `on_accent` | `#001018` | `0x0083` | `#FFFFFF` | `0xFFFF` | Text/icons on accent fills |
| `green` | `#31D784` | `0x36B0` | `#089A4A` | `0x0CC9` | OK / ready / success |
| `amber` | `#FFB221` | `0xFD84` | `#C67500` | `0xC3A0` | Speed zone 2, best, warnings |
| `red` | `#FF5152` | `0xFA8A` | `#D62439` | `0xD127` | Speed zone 3, destructive, errors |
| `on_amber` | `#181000` | `0x1880` | `#FFFFFF` | `0xFFFF` | Text on amber (DEMO badge) |
| `face` | `#101418` | `0x10A3` | `#FFFFFF` | `0xFFFF` | CHRONO dial face |
| `metal_hi` | `#C6CBD6` | `0xC65A` | `#EFF3F7` | `0xEF9E` | CHRONO bezel highlight |
| `metal_lo` | `#39414A` | `0x3A09` | `#94A2AD` | `0x9515` | CHRONO bezel shadow |
| `qr_bg` | `#FFFFFF` | `0xFFFF` | `#FFFFFF` | `0xFFFF` | QR quiet zone (both themes) |
| `qr_fg` | `#000000` | `0x0000` | `#000000` | `0x0000` | QR modules |
| `shadow` | `#000000` | `0x0000` | `#4A5563` | `0x4AAC` | Shadow base (used with alpha) |
<!-- /GEN:tokens -->

All values are exact RGB565 (8-bit channels are bit-replicated: `R8 = r5<<3 | r5>>2`, `G8 = g6<<2 | g6>>4`),
so the mockup hex and the device colour are identical.

**Speed zones** (fraction of full scale): `< 0.60` → `accent`, `0.60–0.85` → `amber`, `≥ 0.85` → `red`
(flat-colour uses: tip glow, APEX segments). **Zone gradient** (HALO arc, PURE bar):
`0%:accent 50%:accent 72%:amber 86%:amber 97%:red 100%:red`. APEX G-arc: `0%:accent 55%:accent 80%:amber 100%:amber`.

Alpha tints used (pre-blend them if the engine prefers opaque fills): `accent@12%` selected option fill,
`accent@16%` sparkline area / selected monogram, `green|amber|text_dim@14%` race status chip & OTA warning chip,
`red@10%` / `red@38%` odometer reset idle / progress, `shadow@25–55%` knob & needle shadows, `green|red@18%` OTA halo rings.

### 2.2 Typography *(generated metrics)*

<!-- GEN:fonts -->
| token | file | px | tracking | digits | cap height (H) | digit height (0) | digit cell (tabular) | advance of space | glyph set |
|---|---|---|---|---|---|---|---|---|---|
| `F_LABEL` | BarlowSemiCondensed-SemiBold | 13 | 1.5 (UPPERCASE) | proportional | 9 | 9 | – | 3 | ASCII + HU + extra (see §2.3) |
| `F_SMALL` | BarlowSemiCondensed-Medium | 15 | 0 | proportional | 11 | 11 | – | 3 | ASCII + HU + extra (see §2.3) |
| `F_BODY` | BarlowSemiCondensed-Medium | 18 | 0 | proportional | 12 | 12 | – | 4 | ASCII + HU + extra (see §2.3) |
| `F_BODYB` | BarlowSemiCondensed-SemiBold | 18 | 0 | proportional | 12 | 12 | – | 4 | ASCII + HU + extra (see §2.3) |
| `F_TITLE` | BarlowSemiCondensed-SemiBold | 24 | 0 | proportional | 18 | 18 | – | 5 | ASCII + HU + extra (see §2.3) |
| `F_NUM_M` | BarlowSemiCondensed-Bold | 32 | 0 | tabular | 23 | 23 | 17 | 6 | ASCII + HU + extra (see §2.3) |
| `F_NUM_L` | BarlowSemiCondensed-Bold | 56 | 0 | tabular | 39 | 39 | 30 | 11 | `0123456789.:-%+␠` |
| `F_NUM_XL` | BarlowSemiCondensed-Bold | 128 | -2 | tabular | 90 | 91 | 70 | 26 | `0123456789.:-%+␠` |
| `F_NUM_XXL` | BarlowSemiCondensed-Bold | 168 | -4 | tabular | 118 | 119 | 91 | 34 | `0123456789` |
<!-- /GEN:fonts -->

| Role | Token | Usage |
|---|---|---|
| Label | `F_LABEL` | All uppercase captions (MAX, TRIP, CALIBRATION…), rail labels (tracking 1), top-bar title, unit under speed (KM/H), DEMO badge. |
| Small | `F_SMALL` | Units after values (km, km/h, s), subtitles, hints, split labels, MB counter. |
| Body | `F_BODY` | Sentences: WiFi steps, OTA sub-lines. |
| Body bold | `F_BODYB` | Row titles, option names, button text, list values (splits, WiFi credentials), CHRONO numerals. |
| Title | `F_TITLE` | Screen/overlay titles, language names, APEX peak value. |
| Num M | `F_NUM_M` | Secondary numbers everywhere (stats, BEST/LAST, G). |
| Num L | `F_NUM_L` | Race live speed, calibration, odometer value, CHRONO window. |
| Num XL | `F_NUM_XL` | Speed in HALO / APEX, race timer, OTA %. Tracking −2. |
| Num XXL | `F_NUM_XXL` | PURE speed only. Tracking −4. |

### 2.3 Glyphs needed beyond ASCII + Hungarian

`–` (U+2013 en dash) in LABEL/SMALL · `—` (U+2014) in BODY/BODYB/SMALL · `…` (U+2026) in BODY/BODYB/SMALL ·
`·` (U+00B7) in LABEL/SMALL · `−` (U+2212 minus) in BODYB. The multiplication sign after the calibration value is
drawn with two capsules, so no `×` glyph is needed. NUM_L / NUM_XL need a space advance (thousands separator in
"1 284.6"); use ¼ em if the font has none.

### 2.4 Spacing, radii, surfaces

* Outer margin **16 px** (content x 16…464). Settings content column **x 112…468**.
* Vertical rhythm on a 2 px grid; gaps between cards **8 px**.
* Radii: cards/tiles **14**, buttons with icons 12–16, segmented control 12 (selected pill 9, inset 4), pills = h/2.
* Card = `surface` fill + 1 px inside stroke `border`. Selected card = 2 px `accent` stroke. Inset areas (segmented
  control, filter options, CHRONO window) use `bg` so they read as recessed.
* APEX uses **chamfered** panels instead (top-left and bottom-right corners cut 10–14 px): outer polygon in `border`,
  inner polygon inset 1 px in `surface`.
* Touch: every target ≥ 48 px in its smaller dimension; primary actions ≥ 56 px. Touch rects may be larger than the
  drawn shape (see `TOUCH` rows). Pressed state for any card/button: fill → `surface_hi` (dark) / `surface_hi` (light)
  for 120 ms or while held.

## 3. Chrome (all main pages) *(generated)*

Top bar is 30 px tall, drawn directly on `bg` (no panel). Page order: **0 Settings · 1 Dashboard · 2 Race**.
Right-side status cluster, from the right edge x=464: WiFi icon (18 px, `accent`, only while the hotspot is on),
then 12 px gap, then the DEMO pill (only in demo mode). Left: page title in `F_LABEL text_dim` (none on the dashboard).

<!-- GEN:chrome -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `chrome.pageDot` | capsule (h) | x=217 y=15 w=6 th=6 | `text_faint` |  | inactive: 6×6 dot; row centred on x=240, gap 8 |
| `chrome.pageDotActive` | capsule (h) | x=231 y=15 w=18 th=6 | `accent` |  | active page: 18×6 capsule |
| `chrome.title` | text | left x=16 base=20 | `text_dim` | `LABEL` “TITLE” |  |
| `chrome.wifi` | icon | wifi 18px at x=446 y=6 (top-left) | `accent` |  |  |
| `chrome.demoBadge` | rrect | x=384 y=5 w=50 h=20 r=10 | `amber` |  | pill, width = text + 16 |
| `chrome.demoText` | text | center x=409 base=20 | `on_amber` | `LABEL` “DEMO” |  |
<!-- /GEN:chrome -->

Swipe: horizontal drag > 60 px or fling > 0.35 px/ms changes page; content slides with the finger and settles with
`easeOutCubic` 220 ms. Top bar does not move; the active page capsule animates its x and width (220 ms, same easing).

## 4. Dashboard

Common: `bg` fill, top bar (page 1), then the skin. Sample data in the mockups: max 74 km/h, ride 42:18, trip
18.4 km, odo 1 284 km. Formats: ride `m:ss` below 1 h, `h:mm` from 1 h (e.g. `1:07`); trip `0.0` (one decimal,
< 1000); odo integer with a space as thousands separator. Units: `km/h`/`km` metric, `mph`/`mi` imperial; the unit
label under/next to the speed is uppercase (`KM/H`, `MPH`).

**Hold-to-reset (MAX and TRIP, 1.0 s)** — same on every skin: while the finger is down on the touch rect the label
turns `accent`, a progress ring (r=7, th=3, `track` + `accent` arc from 0° clockwise, end = 360°·t/1000 ms) appears
14 px after the label (before it for right-aligned labels), and the value turns `text_dim`. Release before 1 s →
ring runs back to 0 in 150 ms. At 1 s: value becomes 0 / current speed, label text shows `RESET` in `green` for 800 ms.
See `dash_halo_trip_hold.png`, `dash_pure_max_hold.png`, `dash_apex_trip_hold.png`.

### 4.1 HALO *(generated, 35 km/h, dark)*
<!-- GEN:screen_dash_halo -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `halo.ticks` | note |  |  |  | every 5 km/h (every 5 mph imperial, scale 0..50): minor capsule w=1.5 r 137..142 `text_faint`; every 10: major capsule w=2 r 132..142 `text_dim`. angle = -135° + 270°·v/85 |
| `halo.track` | arc (round caps) | c=(150,176) r=118 th=18 -135°→135° | `track` |  |  |
| `halo.innerLine` | ring | c=(150,176) r=99 th=1 | `border` |  |  |
| `halo.value` | arc (round caps) | c=(150,176) r=118 th=18 -135°→-23.8° | angle-gradient 0%:accent 50%:accent 72%:amber 86%:amber 97%:red 100%:red over -135°..135° |  | end = A0 + 270°·pct; drawn even at 0 (dot) |
| `halo.tipGlow` | glow | c=(102.3,68.1) r=34 | `accent` A=55% (light ×0.55) |  | colour = flat zone colour of pct |
| `halo.tipDot` | circle | c=(102.3,68.1) r=4.5 | `text_hi` |  | comet head at arc end |
| `halo.maxMark` | poly (convex) | (254.4,194.5) (247.4,187) (245.2,199.1) | `amber` |  | tell-tale: tip r=106 (pointing at the ring), base r=98 ±3.6°, at session-max angle |
| `halo.speed` | text | center x=150 base=211 | `text_hi` | `NUM_XL` “35” |  |
| `halo.unit` | text | center x=150 base=243 | `text_dim` | `LABEL` “KM/H” |  |
| `halo.max.label` | text | left x=306 base=64 | `text_dim` | `LABEL` “MAX” |  |
| `halo.max.mark` | poly (convex) | (339,55) (347,55) (343,62) | `amber` |  |  |
| `halo.max.value` | text | left x=306 base=99 | `text_hi` | `NUM_M` “74” | block left-aligned at x=306 (value+5px+unit) |
| `halo.max.unit` | text | left x=345 base=99 | `text_dim` | `SMALL` “km/h” |  |
| `halo.touchMax` | TOUCH | x=298 y=38 w=182 h=70 |  |  | hold 1 s: reset session max |
| `halo.sep1` | hline 1px | x=306 y=108 w=158 | `border` |  |  |
| `halo.ride.label` | text | left x=306 base=134 | `text_dim` | `LABEL` “RIDE TIME” |  |
| `halo.ride.value` | text | left x=306 base=169 | `text_hi` | `NUM_M` “42:18” | block left-aligned at x=306 (value+5px+unit) |
| `halo.sep2` | hline 1px | x=306 y=178 w=158 | `border` |  |  |
| `halo.trip.label` | text | left x=306 base=204 | `text_dim` | `LABEL` “TRIP” |  |
| `halo.trip.value` | text | left x=306 base=239 | `text_hi` | `NUM_M` “18.4” | block left-aligned at x=306 (value+5px+unit) |
| `halo.trip.unit` | text | left x=370 base=239 | `text_dim` | `SMALL` “km” |  |
| `halo.touchTrip` | TOUCH | x=298 y=178 w=182 h=70 |  |  | hold 1 s: reset trip |
| `halo.sep3` | hline 1px | x=306 y=248 w=158 | `border` |  |  |
| `halo.odo.label` | text | left x=306 base=274 | `text_dim` | `LABEL` “ODO” |  |
| `halo.odo.value` | text | left x=306 base=309 | `text_hi` | `NUM_M` “1 284” | block left-aligned at x=306 (value+5px+unit) |
| `halo.odo.unit` | text | left x=385 base=309 | `text_dim` | `SMALL` “km” |  |
<!-- /GEN:screen_dash_halo -->

### 4.2 PURE *(generated)*
<!-- GEN:screen_dash_pure -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `pure.speed` | text | center x=240 base=196 | `text_hi` | `NUM_XXL` “35” |  |
| `pure.unit` | text | center x=240 base=226 | `text_dim` | `LABEL` “KM/H” |  |
| `pure.barTrack` | capsule (h) | x=28 y=248 w=424 th=6 | `track` |  |  |
| `pure.barFill` | capsule + h-gradient | x=28 y=248 w=barW·pct (min 6) th=6 | h-gradient 0%:accent 50%:accent 72%:amber 86%:amber 97%:red 100%:red over full bar width |  | gradient anchored to the full bar, fill clipped to value |
| `pure.tipGlow` | glow | c=(199.6,248) r=22 | `accent` A=60% (light ×0.55) |  | alpha = A·(1−d/r)² |
| `pure.tipDot` | circle | c=(199.6,248) r=5 | `text_hi` |  |  |
| `pure.maxMark` | capsule | (397.1,240)→(397.1,256) w=2 | `amber` |  | session max marker |
| `pure.max.label` | text | center x=72 base=280 | `text_dim` | `LABEL` “MAX” |  |
| `pure.max.value` | text | left x=55 base=312 | `text_hi` | `NUM_M` “74” | block center-aligned at x=72 (value+5px+unit) |
| `pure.touchmax` | TOUCH | x=16 y=262 w=112 h=58 |  |  | hold 1 s: reset max |
| `pure.sep1` | vline 1px | x=128 y=272 h=34 | `border` |  |  |
| `pure.ride.label` | text | center x=184 base=280 | `text_dim` | `LABEL` “RIDE” |  |
| `pure.ride.value` | text | left x=145 base=312 | `text_hi` | `NUM_M` “42:18” | block center-aligned at x=184 (value+5px+unit) |
| `pure.sep2` | vline 1px | x=240 y=272 h=34 | `border` |  |  |
| `pure.trip.label` | text | center x=296 base=280 | `text_dim` | `LABEL` “TRIP” |  |
| `pure.trip.value` | text | left x=255 base=312 | `text_hi` | `NUM_M` “18.4” | block center-aligned at x=296 (value+5px+unit) |
| `pure.trip.unit` | text | left x=319 base=312 | `text_dim` | `SMALL` “km” |  |
| `pure.touchtrip` | TOUCH | x=240 y=262 w=112 h=58 |  |  | hold 1 s: reset trip |
| `pure.sep3` | vline 1px | x=352 y=272 h=34 | `border` |  |  |
| `pure.odo.label` | text | center x=408 base=280 | `text_dim` | `LABEL` “ODO” |  |
| `pure.odo.value` | text | left x=360 base=312 | `text_hi` | `NUM_M` “1 284” | block center-aligned at x=408 (value+5px+unit) |
| `pure.odo.unit` | text | left x=439 base=312 | `text_dim` | `SMALL` “km” |  |
<!-- /GEN:screen_dash_pure -->

### 4.3 CHRONO *(generated)*
<!-- GEN:screen_dash_chrono -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `chrono.bezel` | arc (butt caps) | c=(240,175) r=139 th=8 0°→360° | angle-gradient 0%:metal_lo 13%:metal_hi 30%:metal_lo 50%:metal_lo 63%:metal_hi 80%:metal_lo 100%:metal_lo over -90°..270° |  | full ring; highlights at -45° and +135° (brushed-steel sheen) |
| `chrono.face` | circle | c=(240,175) r=135 | `face` |  |  |
| `chrono.chapter` | ring | c=(240,175) r=131 th=1 | `border` |  |  |
| `chrono.zoneAmber` | arc (butt caps) | c=(240,175) r=129 th=3 27°→93.7° | `amber` |  | 51..72 km/h (60–85 % of scale) |
| `chrono.zoneRed` | arc (butt caps) | c=(240,175) r=129 th=3 93.7°→135° | `red` |  | 72..85 km/h |
| `chrono.minuteTrack` | note |  |  |  | every 1 km/h: capsule w=1 r 121..126 `text_faint`; every 5: w=2 r 116..126 `text_dim` (skip multiples of 10) |
| `chrono.indices` | note |  |  |  | every 10 km/h: baton quad r 99..124, ±1.7° outer / ±2.0° inner, `text_hi`, with lume line capsule w=1.5 r 104..120 `accent` @90%. Numerals BODYB `text` centred at r=83, baseline = centre.y+6 |
| `chrono.brand` | text | center x=240 base=146 track=3 | `text_dim` | `LABEL` “SUR-RON” |  |
| `chrono.window` | rrect | x=196 y=205 w=88 h=54 r=10 | `bg` |  |  |
| `chrono.window.stroke` | rrect stroke 1px (inside) | x=196 y=205 w=88 h=54 r=10 | `border` |  |  |
| `chrono.speed` | text | center x=240 base=252 | `text_hi` | `NUM_L` “35” |  |
| `chrono.unit` | text | center x=240 base=280 | `text_dim` | `LABEL` “KM/H” |  |
| `chrono.maxMark` | poly (convex) | (350.3,194.6) (343.2,187.8) (341.3,198.5) | `amber` |  | tip r=112, base r=104 ±3° |
| `chrono.needleShadow` | poly (convex) | (190.7,62.8) (241.7,169) (254.7,199.4) (248.5,202.1) (234.8,172) | `shadow` @55% |  | same polygon offset (+2,+4) |
| `chrono.needle` | poly (convex) | (188.7,58.8) (239.7,165) (252.7,195.4) (246.5,198.1) (232.8,168) | `accent` |  | tip r=127; shoulders r=10 at ±22°; tail r=24 at 180°±8° |
| `chrono.hub` | circle | c=(240,175) r=10 | `metal_lo` |  |  |
| `chrono.hubRing` | ring | c=(240,175) r=9 th=2 | `metal_hi` |  |  |
| `chrono.hubDot` | circle | c=(240,175) r=3.5 | `accent` |  |  |
| `chrono.ride.label` | text | left x=16 base=60 | `text_dim` | `LABEL` “RIDE” |  |
| `chrono.ride.value` | text | left x=16 base=94 | `text_hi` | `NUM_M` “42:18” | block left-aligned at x=16 (value+5px+unit) |
| `chrono.trip.label` | text | right x=464 base=60 | `text_dim` | `LABEL` “TRIP” |  |
| `chrono.trip.value` | text | left x=382 base=94 | `text_hi` | `NUM_M` “18.4” | block right-aligned at x=464 (value+5px+unit) |
| `chrono.trip.unit` | text | left x=446 base=94 | `text_dim` | `SMALL` “km” |  |
| `chrono.max.label` | text | left x=16 base=272 | `text_dim` | `LABEL` “MAX” |  |
| `chrono.max.value` | text | left x=16 base=306 | `text_hi` | `NUM_M` “74” | block left-aligned at x=16 (value+5px+unit) |
| `chrono.max.unit` | text | left x=55 base=306 | `text_dim` | `SMALL` “km/h” |  |
| `chrono.odo.label` | text | right x=464 base=272 | `text_dim` | `LABEL` “ODO” |  |
| `chrono.odo.value` | text | left x=367 base=306 | `text_hi` | `NUM_M` “1 284” | block right-aligned at x=464 (value+5px+unit) |
| `chrono.odo.unit` | text | left x=446 base=306 | `text_dim` | `SMALL` “km” |  |
| `chrono.touchTrip` | TOUCH | x=380 y=36 w=100 h=72 |  |  | hold 1 s: reset trip |
| `chrono.touchMax` | TOUCH | x=0 y=248 w=100 h=72 |  |  | hold 1 s: reset session max |
<!-- /GEN:screen_dash_chrono -->

Notes: minute track, indices and numerals are static — pre-render the whole dial (bezel → face → track → indices →
numerals → brand → window frame) into a PSRAM layer per theme (480×320×2 = 300 KB) and blit it each frame; then
draw zone arcs, max tell-tale, window digits, needle shadow, needle, hub, corner data.

### 4.4 APEX *(generated)*
<!-- GEN:screen_dash_apex -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `apex.segments` | note |  |  |  | 32 parallelograms, x = 16 + i·14, y=38, w=11, h=12, top edge skewed +3px. lit = round(pct·32); lit colour = zone colour of segment centre ((i+.5)/32); unlit `track`. At ≥97 % the last 4 lit segments blink `red`/`text_hi` at 6 Hz (shift-light) |
| `apex.scale` | note |  |  |  | scale numerals 0/20/40/60/80 LABEL (track 0.5) `text_faint`, baseline 70, centred at x = 16 + v/85·448 (0 left-aligned) |
| `apex.speed` | text | right x=206 base=176 | `text_hi` | `NUM_XL` “35” |  |
| `apex.unit` | text | left x=216 base=176 | `text_dim` | `LABEL` “KM/H” |  |
| `apex.peak.edge` | poly (convex) | (26,196) (244,196) (244,228) (234,238) (16,238) (16,206) | `border` |  |  |
| `apex.peak` | poly (convex) | (26.6,197) (243,197) (243,227.4) (233.4,237) (17,237) (17,206.6) | `surface` |  | outer poly in `border`, inner poly inset 1px in fill = 1px stroke |
| `apex.peak.label` | text | left x=30 base=222 | `amber` | `LABEL` “PEAK” |  |
| `apex.peak.value` | text | left x=170 base=226 | `text_hi` | `TITLE` “74” | block right-aligned at x=230 (value+5px+unit) |
| `apex.peak.unit` | text | left x=199 base=226 | `text_dim` | `SMALL` “km/h” |  |
| `apex.touchPeak` | TOUCH | x=16 y=190 w=228 h=54 |  |  | hold 1 s: reset session max |
| `apex.gPanel.edge` | poly (convex) | (270,78) (464,78) (464,222) (450,236) (256,236) (256,92) | `border` |  |  |
| `apex.gPanel` | poly (convex) | (270.6,79) (463,79) (463,221.4) (449.4,235) (257,235) (257,92.6) | `surface` |  | outer poly in `border`, inner poly inset 1px in fill = 1px stroke |
| `apex.gLabel` | text | left x=272 base=100 | `text_dim` | `LABEL` “LONG. G” |  |
| `apex.gPeak` | text | right x=450 base=100 | `amber` | `LABEL` “PEAK 0.52” |  |
| `apex.gTicks` | note |  |  |  | 7 ticks at 0.0,0.1..0.6 G: r 65..69 (0/0.3/0.6: ..71, w=2 `text_dim`), others w=1.5 `text_faint` |
| `apex.gTrack` | arc (round caps) | c=(360,176) r=56 th=10 -90°→90° | `track` |  |  |
| `apex.gValue` | arc (round caps) | c=(360,176) r=56 th=10 -90°→3° | angle-gradient 0%:accent 55%:accent 80%:amber 100%:amber over -90°..90° |  | end = -90° + 180°·(G/0.60) |
| `apex.gGlow` | glow | c=(362.9,120.1) r=24 | `accent` A=50% (light ×0.55) |  | alpha = A·(1−d/r)² |
| `apex.gTip` | circle | c=(362.9,120.1) r=3.5 | `text_hi` |  |  |
| `apex.gPeakMark` | circle | c=(411.2,153.2) r=3 | `amber` |  | peak-G tell-tale on the arc centre-line, drawn after the value arc |
| `apex.g.value` | text | left x=325 base=174 | `text_hi` | `NUM_M` “0.31” | block center-aligned at x=360 (value+4px+unit) |
| `apex.g.unit` | text | left x=388 base=174 | `text_dim` | `LABEL` “G” |  |
| `apex.sparkBase` | hline 1px | x=272 y=222 w=176 | `border` |  |  |
| `apex.spark` | note |  |  |  | G history sparkline: x 272..448, baseline y=222 (hline `border`), height 30px = 0.60 G, 40 samples (10 Hz, 4 s). Area = one convex quad per segment `accent` @16%; line = capsules w=2 `accent`; newest point dot r=3 `text_hi` |
| `apex.strip.edge` | poly (convex) | (28,248) (464,248) (464,300) (452,312) (16,312) (16,260) | `border` |  |  |
| `apex.strip` | poly (convex) | (28.6,249) (463,249) (463,299.4) (451.4,311) (17,311) (17,260.6) | `surface` |  | outer poly in `border`, inner poly inset 1px in fill = 1px stroke |
| `apex.ride.label` | text | left x=34 base=270 | `text_dim` | `LABEL` “RIDE” |  |
| `apex.ride.value` | text | left x=34 base=302 | `text_hi` | `NUM_M` “42:18” | block left-aligned at x=34 (value+5px+unit) |
| `apex.stripSep1` | vline 1px | x=165 y=260 h=40 | `border` |  |  |
| `apex.trip.label` | text | left x=183 base=270 | `text_dim` | `LABEL` “TRIP” |  |
| `apex.trip.value` | text | left x=183 base=302 | `text_hi` | `NUM_M` “18.4” | block left-aligned at x=183 (value+5px+unit) |
| `apex.trip.unit` | text | left x=247 base=302 | `text_dim` | `SMALL` “km” |  |
| `apex.stripSep2` | vline 1px | x=314 y=260 h=40 | `border` |  |  |
| `apex.odo.label` | text | left x=332 base=270 | `text_dim` | `LABEL` “ODO” |  |
| `apex.odo.value` | text | left x=332 base=302 | `text_hi` | `NUM_M` “1 284” | block left-aligned at x=332 (value+5px+unit) |
| `apex.odo.unit` | text | left x=411 base=302 | `text_dim` | `SMALL` “km” |  |
| `apex.touchTrip` | TOUCH | x=165 y=248 w=149 h=64 |  |  | hold 1 s: reset trip |
<!-- /GEN:screen_dash_apex -->

G source: longitudinal acceleration, forward only, clamp 0.00–0.60. Readout `%.2f`. `PEAK` = session max G
(resets together with session max). Sparkline ring buffer: 40 samples at 10 Hz.

## 5. Race / launch timer *(generated, READY state)*

<!-- GEN:screen_race -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `race.chip` | rrect | x=16 y=44 w=161 h=32 r=16 | `green` @14% |  | width = text + 44 |
| `race.chipDotGlow` | glow | c=(32,60) r=12 | `green` A=80% (light ×0.55) |  | pulses 1 Hz (A 0→80 %) |
| `race.chipDot` | circle | c=(32,60) r=4.5 | `green` |  |  |
| `race.chipText` | text | left x=44 base=66 | `green` | `BODYB` “Ready to launch” |  |
| `race.reset` | rrect | x=196 y=36 w=96 h=48 r=12 | `surface` |  |  |
| `race.reset.stroke` | rrect stroke 1px (inside) | x=196 y=36 w=96 h=48 r=12 | `border` |  |  |
| `race.resetIcon` | icon | reset 20px at x=212 y=50 (top-left) | `text_dim` |  |  |
| `race.resetText` | text | left x=240 base=66 | `text` | `BODYB` “Reset” |  |
| `race.touchReset` | TOUCH | x=196 y=32 w=104 h=56 |  |  | tap: reset / re-arm run |
| `race.timer` | text | left x=14 base=186 | `text_hi` | `NUM_XL` “0.00” | running: updates every frame, 2 decimals |
| `race.timerUnit` | text | left x=256 base=186 | `text_dim` | `TITLE` “s” |  |
| `race.barTrack` | capsule (h) | x=16 y=208 w=276 th=8 | `track` |  |  |
| `race.best` | rrect | x=16 y=228 w=134 h=84 r=14 | `surface` |  |  |
| `race.best.stroke` | rrect stroke 1px (inside) | x=16 y=228 w=134 h=84 r=14 | `border` |  |  |
| `race.best.label` | text | left x=32 base=254 | `amber` | `LABEL` “BEST” |  |
| `race.best.value` | text | left x=32 base=292 | `amber` | `NUM_M` “3.58” | block left-aligned at x=32 (value+4px+unit) |
| `race.best.unit` | text | left x=95 base=292 | `text_dim` | `SMALL` “s” |  |
| `race.last` | rrect | x=158 y=228 w=134 h=84 r=14 | `surface` |  |  |
| `race.last.stroke` | rrect stroke 1px (inside) | x=158 y=228 w=134 h=84 r=14 | `border` |  |  |
| `race.last.label` | text | left x=174 base=254 | `text_dim` | `LABEL` “LAST” |  |
| `race.last.value` | text | left x=174 base=292 | `text_hi` | `NUM_M` “3.71” | block left-aligned at x=174 (value+4px+unit) |
| `race.last.unit` | text | left x=237 base=292 | `text_dim` | `SMALL` “s” |  |
| `race.speedTile` | rrect | x=304 y=36 w=160 h=92 r=14 | `surface` |  |  |
| `race.speedTile.stroke` | rrect stroke 1px (inside) | x=304 y=36 w=160 h=92 r=14 | `border` |  |  |
| `race.speedLabel` | text | left x=320 base=60 | `text_dim` | `LABEL` “SPEED” |  |
| `race.speed.value` | text | left x=320 base=112 | `text_hi` | `NUM_L` “0” | block left-aligned at x=320 (value+8px+unit) |
| `race.speed.unit` | text | left x=358 base=112 | `text_dim` | `LABEL` “KM/H” |  |
| `race.splits` | rrect | x=304 y=136 w=160 h=176 r=14 | `surface` |  |  |
| `race.splits.stroke` | rrect stroke 1px (inside) | x=304 y=136 w=160 h=176 r=14 | `border` |  |  |
| `race.splitsLabel` | text | left x=320 base=160 | `text_dim` | `LABEL` “SPLITS” |  |
| `race.split0.label` | text | left x=320 base=192 | `text_dim` | `SMALL` “50–60” |  |
| `race.split0.value` | text | left x=407 base=192 | `text` | `BODYB` “0.88” | block right-aligned at x=448 (value+3px+unit) |
| `race.split0.unit` | text | left x=441 base=192 | `text_dim` | `SMALL` “s” |  |
| `race.split1.label` | text | left x=320 base=222 | `text_dim` | `SMALL` “60–70” |  |
| `race.split1.value` | text | left x=410 base=222 | `text` | `BODYB` “1.05” | block right-aligned at x=448 (value+3px+unit) |
| `race.split1.unit` | text | left x=441 base=222 | `text_dim` | `SMALL` “s” |  |
| `race.split2.label` | text | left x=320 base=252 | `text_dim` | `SMALL` “70–80” |  |
| `race.split2.value` | text | left x=413 base=252 | `text` | `BODYB` “1.41” | block right-aligned at x=448 (value+3px+unit) |
| `race.split2.unit` | text | left x=441 base=252 | `text_dim` | `SMALL` “s” |  |
| `race.splitSep` | hline 1px | x=320 y=266 w=128 | `border` |  |  |
| `race.totalLabel` | text | left x=320 base=294 | `amber` | `SMALL` “0–80 total” |  |
| `race.total.value` | text | left x=407 base=294 | `amber` | `BODYB` “6.92” | block right-aligned at x=448 (value+3px+unit) |
| `race.total.unit` | text | left x=441 base=294 | `amber` | `SMALL` “s” |  |
<!-- /GEN:screen_race -->

State table (only these elements change):

| State | Chip text EN / HU | Chip colour | Timer text | Timer colour | Bar |
|---|---|---|---|---|---|
| ARMED / READY | Ready to launch / Rajtra kész | `green` (dot glow pulses 1 Hz) | `0.00` | `text_hi` | empty |
| RUNNING | Pulling… / Gyorsítás… | `amber` | live `t` (2 decimals) | `accent` | `accent`, w = 276·v/50, glow at tip |
| FINISHED | Run finished / Futam kész | `green` | final 0–50 | `green` | full, `green` |
| WAIT_STOP | Stop to arm / Állj meg | `text_dim` | `0.00` | `text_faint` | empty |

* Target label in the top bar: `RACE · 0–50 KM/H` (imperial `RACE · 0–30 MPH`), HU `VERSENY · …`.
* Splits rows metric `50–60`, `60–70`, `70–80`, total `0–80 total`; imperial `30–40`, `40–50`, `50–60`, `0–60 total`.
  Unknown split: `—` in `text_faint`, no unit. HU total: `0–80 össz.`
* **NEW** badge (HU `ÚJ`) in the BEST tile only when the finished run set a new best (`race_finished.png`).
* BEST value `amber`, LAST value `text_hi`; when never set show `0.00` in `text_faint`. Values are for the current unit system only.
* Reset: tap (no hold) → clears the current run, re-arms (WAIT_STOP if moving, else READY).

## 6. Settings

Left rail (6 × 48 px items, full-height touch rects x 0…106) + content column x 112…468. Top bar title
`SETTINGS` / `BEÁLLÍTÁSOK`. Rail labels EN: SYSTEM, SKIN, SPEED, ODOMETER, LANGUAGE, WIFI — HU: RENDSZER, STÍLUS,
SEBESSÉG, ODOMÉTER, NYELV, WIFI. Active item: `surface_hi` pill + 3 px `accent` bar at x=7.5 + accent icon + `text_hi`
label. Section switch: content cross-fades 120 ms (or instant).

### 6.1 System *(generated)*
<!-- GEN:screen_set_system -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `set.railActive` | rrect | x=6 y=34 w=94 h=42 r=12 | `surface_hi` |  |  |
| `set.railActive.bar` | capsule | (7.5,45)→(7.5,65) w=3 | `accent` |  | active indicator |
| `set.railActive.icon` | icon | system 20px at x=43 y=37 (top-left) | `accent` |  |  |
| `set.railActive.label` | text | center x=53 base=71 track=1 | `text_hi` | `LABEL` “SYSTEM” |  |
| `set.touchRail` | TOUCH | x=0 y=31 w=106 h=48 |  |  | tap: open section |
| `set.railItem.icon` | icon | skin 20px at x=43 y=85 (top-left) | `text_dim` |  |  |
| `set.railItem.label` | text | center x=53 base=119 track=1 | `text_dim` | `LABEL` “SKIN” |  |
| `set.rail` | note |  |  |  | 6 items, y = 31 + i·48; icon 20px top-left (43, y+6); label LABEL (track 1) centred x=53 baseline y+40; touch rect x=0 w=106 h=48 |
| `sys.bright` | rrect | x=112 y=36 w=356 h=100 r=14 | `surface` |  |  |
| `sys.bright.stroke` | rrect stroke 1px (inside) | x=112 y=36 w=356 h=100 r=14 | `border` |  |  |
| `sys.brightLabel` | text | left x=128 base=60 | `text_dim` | `LABEL` “BRIGHTNESS” |  |
| `sys.brightValue` | text | right x=452 base=61 | `text_hi` | `BODYB` “72%” |  |
| `sys.sunSmall` | icon | sun 16px at x=128 y=94 (top-left) | `text_dim` |  |  |
| `sys.sunBig` | icon | sun 22px at x=430 y=91 (top-left) | `text_dim` |  |  |
| `sys.sliderTrack` | capsule (h) | x=158 y=102 w=264 th=8 | `track` |  |  |
| `sys.sliderFill` | capsule (h) | x=158 y=102 w=184 th=8 | `accent` |  | x = 158 + 264·(b−8)/92 |
| `sys.knobShadow` | circle | c=(342,104) r=15 | `shadow` @30% |  |  |
| `sys.knob` | circle | c=(342,102) r=14 | `text_hi` |  |  |
| `sys.knobRing` | ring | c=(342,102) r=13 th=2 | `accent` |  |  |
| `sys.touchSlider` | TOUCH | x=112 y=74 w=356 h=56 |  |  | drag / tap: brightness 8–100 % |
| `sys.units` | rrect | x=112 y=144 w=356 h=80 r=14 | `surface` |  |  |
| `sys.units.stroke` | rrect stroke 1px (inside) | x=112 y=144 w=356 h=80 r=14 | `border` |  |  |
| `sys.units.title` | text | left x=128 base=178 | `text_hi` | `BODYB` “Units” |  |
| `sys.units.sub` | text | left x=128 base=201 | `text_dim` | `SMALL` “Speed & distance” |  |
| `sys.units.seg` | rrect | x=288 y=160 w=164 h=48 r=12 | `bg` |  |  |
| `sys.units.seg.stroke` | rrect stroke 1px (inside) | x=288 y=160 w=164 h=48 r=12 | `border` |  |  |
| `sys.units.seg.sel` | rrect | x=292 y=164 w=74 h=40 r=9 | `accent` |  |  |
| `sys.units.seg.opt0` | text | center x=329 base=190 | `on_accent` | `BODYB` “km/h” |  |
| `sys.units.seg.t0` | TOUCH | x=288 y=160 w=82 h=48 |  |  | select "km/h" |
| `sys.units.seg.opt1` | text | center x=411 base=190 | `text_dim` | `BODYB` “mph” |  |
| `sys.units.seg.t1` | TOUCH | x=370 y=160 w=82 h=48 |  |  | select "mph" |
| `sys.theme` | rrect | x=112 y=232 w=356 h=80 r=14 | `surface` |  |  |
| `sys.theme.stroke` | rrect stroke 1px (inside) | x=112 y=232 w=356 h=80 r=14 | `border` |  |  |
| `sys.theme.title` | text | left x=128 base=266 | `text_hi` | `BODYB` “Theme” |  |
| `sys.theme.sub` | text | left x=128 base=289 | `text_dim` | `SMALL` “Night / sunlight” |  |
| `sys.theme.seg` | rrect | x=288 y=248 w=164 h=48 r=12 | `bg` |  |  |
| `sys.theme.seg.stroke` | rrect stroke 1px (inside) | x=288 y=248 w=164 h=48 r=12 | `border` |  |  |
| `sys.theme.seg.sel` | rrect | x=292 y=252 w=74 h=40 r=9 | `accent` |  |  |
| `sys.theme.seg.opt0` | text | center x=329 base=278 | `on_accent` | `BODYB` “Dark” |  |
| `sys.theme.seg.t0` | TOUCH | x=288 y=248 w=82 h=48 |  |  | select "Dark" |
| `sys.theme.seg.opt1` | text | center x=411 base=278 | `text_dim` | `BODYB` “Light” |  |
| `sys.theme.seg.t1` | TOUCH | x=370 y=248 w=82 h=48 |  |  | select "Light" |
<!-- /GEN:screen_set_system -->
Brightness slider: drag anywhere in the touch rect; value = 8 + 92·(x−158)/264, clamped, step 1 %. Knob grows to r=16
while dragged.

### 6.2 Skin *(generated)*
<!-- GEN:screen_set_skin -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `set.railItem.icon` | icon | system 20px at x=43 y=37 (top-left) | `text_dim` |  |  |
| `set.railItem.label` | text | center x=53 base=71 track=1 | `text_dim` | `LABEL` “SYSTEM” |  |
| `set.touchRail` | TOUCH | x=0 y=31 w=106 h=48 |  |  | tap: open section |
| `set.railActive` | rrect | x=6 y=82 w=94 h=42 r=12 | `surface_hi` |  |  |
| `set.railActive.bar` | capsule | (7.5,93)→(7.5,113) w=3 | `accent` |  | active indicator |
| `set.railActive.icon` | icon | skin 20px at x=43 y=85 (top-left) | `accent` |  |  |
| `set.railActive.label` | text | center x=53 base=119 track=1 | `text_hi` | `LABEL` “SKIN” |  |
| `set.rail` | note |  |  |  | 6 items, y = 31 + i·48; icon 20px top-left (43, y+6); label LABEL (track 1) centred x=53 baseline y+40; touch rect x=0 w=106 h=48 |
| `skin.grid` | note |  |  |  | 2×2 cards 174×134, origins (112,36) (294,36) (112,178) (294,178); order HALO, PURE, CHRONO, APEX. Unselected card: 1 px `border` stroke, name `text_dim`, no check |
| `skin.card` | rrect | x=112 y=36 w=174 h=134 r=14 | `surface` |  |  |
| `skin.card.stroke` | rrect stroke 2px (inside) | x=112 y=36 w=174 h=134 r=14 | `accent` |  |  |
| `skin.thumb` | bitmap 144×96 (r=8 corner mask) | x=card.x+15 y=card.y+10 | png/thumbs/skin_<name>_<theme>.png |  | pre-rendered, RGB565 27.6 KB each |
| `skin.card.name` | text | left x=128 base=158 track=2 | `text_hi` | `LABEL` “HALO” |  |
| `skin.card.desc` | text | right x=270 base=159 | `text_dim` | `SMALL` “Ring” |  |
| `skin.check` | circle | c=(257,60) r=11 | `accent` |  |  |
| `skin.checkIcon` | icon | check 18px at x=248 y=51 (top-left) | `on_accent` |  |  |
| `skin.touchCard` | TOUCH | x=112 y=36 w=174 h=134 |  |  | tap: select skin |
<!-- /GEN:screen_set_skin -->
Thumbnails: `design/png/thumbs/skin_<halo|pure|chrono|apex>_<dark|light>.png` (144×96, already RGB565). Store as
RGB565 arrays (27 648 B each, 8 total ≈ 221 KB flash) and blit; rounded corners = 4 precomputed 8×8 alpha corner
masks in `surface`, then the 1 px `border` stroke. Descriptions EN Ring/Minimal/Watch/HUD, HU Gyűrű/Minimál/Óra/HUD.

### 6.3 Speed *(generated)*
<!-- GEN:screen_set_speed -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `set.railItem.icon` | icon | system 20px at x=43 y=37 (top-left) | `text_dim` |  |  |
| `set.railItem.label` | text | center x=53 base=71 track=1 | `text_dim` | `LABEL` “SYSTEM” |  |
| `set.touchRail` | TOUCH | x=0 y=31 w=106 h=48 |  |  | tap: open section |
| `set.railActive` | rrect | x=6 y=130 w=94 h=42 r=12 | `surface_hi` |  |  |
| `set.railActive.bar` | capsule | (7.5,141)→(7.5,161) w=3 | `accent` |  | active indicator |
| `set.railActive.icon` | icon | speed 20px at x=43 y=133 (top-left) | `accent` |  |  |
| `set.railActive.label` | text | center x=53 base=167 track=1 | `text_hi` | `LABEL` “SPEED” |  |
| `set.rail` | note |  |  |  | 6 items, y = 31 + i·48; icon 20px top-left (43, y+6); label LABEL (track 1) centred x=53 baseline y+40; touch rect x=0 w=106 h=48 |
| `spd.cal` | rrect | x=112 y=36 w=356 h=116 r=14 | `surface` |  |  |
| `spd.cal.stroke` | rrect stroke 1px (inside) | x=112 y=36 w=356 h=116 r=14 | `border` |  |  |
| `spd.calLabel` | text | left x=128 base=60 | `text_dim` | `LABEL` “CALIBRATION” |  |
| `spd.calRange` | text | right x=452 base=60 | `text_dim` | `SMALL` “0.50 – 2.00” |  |
| `spd.minus` | rrect | x=128 y=74 w=64 h=64 r=16 | `surface_hi` |  |  |
| `spd.minus.stroke` | rrect stroke 1px (inside) | x=128 y=74 w=64 h=64 r=16 | `border` |  |  |
| `spd.minus.h` | capsule | (150,106)→(170,106) w=4 | `text_hi` |  |  |
| `spd.plus` | rrect | x=388 y=74 w=64 h=64 r=16 | `surface_hi` |  |  |
| `spd.plus.stroke` | rrect stroke 1px (inside) | x=388 y=74 w=64 h=64 r=16 | `border` |  |  |
| `spd.plus.h` | capsule | (410,106)→(430,106) w=4 | `text_hi` |  |  |
| `spd.plus.v` | capsule | (420,96)→(420,116) w=4 | `text_hi` |  |  |
| `spd.touchMinus` | TOUCH | x=120 y=70 w=80 h=72 |  |  | tap −0.01, hold: repeat (400 ms delay, then 12/s, after 2 s 40/s) |
| `spd.touchPlus` | TOUCH | x=380 y=70 w=80 h=72 |  |  | tap +0.01, hold: repeat |
| `spd.calValue` | text | left x=230 base=126 | `text_hi` | `NUM_L` “1.04” | value + 7 px + 10×10 “×” drawn as 2 capsules; block centred on x=290 |
| `spd.calX1` | capsule | (341,110)→(351,120) w=3 | `text_dim` |  |  |
| `spd.calX2` | capsule | (351,110)→(341,120) w=3 | `text_dim` |  |  |
| `spd.filter` | rrect | x=112 y=160 w=356 h=152 r=14 | `surface` |  |  |
| `spd.filter.stroke` | rrect stroke 1px (inside) | x=112 y=160 w=356 h=152 r=14 | `border` |  |  |
| `spd.filterLabel` | text | left x=128 base=184 | `text_dim` | `LABEL` “FILTER MODE” |  |
| `spd.optSel` | rrect | x=124 y=196 w=332 h=50 r=12 | `accent` @12% |  |  |
| `spd.optSel.stroke` | rrect stroke 1.5px (inside) | x=124 y=196 w=332 h=50 r=12 | `accent` |  |  |
| `spd.optSel.radio` | ring | c=(146,221) r=9 th=2 | `accent` |  |  |
| `spd.optSel.radioDot` | circle | c=(146,221) r=4.5 | `accent` |  |  |
| `spd.optSel.title` | text | left x=166 base=227 | `text_hi` | `BODYB` “Fast / adaptive” |  |
| `spd.optSel.sub` | text | right x=440 base=226 | `text_dim` | `SMALL` “Zero lag, instant stop” |  |
| `spd.optSel.touch` | TOUCH | x=124 y=196 w=332 h=50 |  |  | tap: select filter |
| `spd.opt` | rrect | x=124 y=252 w=332 h=50 r=12 | `bg` |  |  |
| `spd.opt.radio` | ring | c=(146,277) r=9 th=2 | `text_dim` |  |  |
| `spd.opt.title` | text | left x=166 base=283 | `text` | `BODYB` “OEM smooth” |  |
| `spd.opt.sub` | text | right x=440 base=282 | `text_dim` | `SMALL` “12-pulse average” |  |
| `spd.opt.touch` | TOUCH | x=124 y=252 w=332 h=50 |  |  | tap: select filter |
<!-- /GEN:screen_set_speed -->
Calibration step 0.01, range 0.50–2.00, `%.2f`. Hold-to-repeat: first repeat after 400 ms, then 12/s, after 2 s held
40/s. Filter option subtitles HU: `Késés nélkül`, `12 impulzus átlaga`; titles `Gyors / adaptív`, `Gyári simítás`.

### 6.4 Odometer *(generated)*
<!-- GEN:screen_set_odo -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `set.railItem.icon` | icon | system 20px at x=43 y=37 (top-left) | `text_dim` |  |  |
| `set.railItem.label` | text | center x=53 base=71 track=1 | `text_dim` | `LABEL` “SYSTEM” |  |
| `set.touchRail` | TOUCH | x=0 y=31 w=106 h=48 |  |  | tap: open section |
| `set.railActive` | rrect | x=6 y=178 w=94 h=42 r=12 | `surface_hi` |  |  |
| `set.railActive.bar` | capsule | (7.5,189)→(7.5,209) w=3 | `accent` |  | active indicator |
| `set.railActive.icon` | icon | odo 20px at x=43 y=181 (top-left) | `accent` |  |  |
| `set.railActive.label` | text | center x=53 base=215 track=1 | `text_hi` | `LABEL` “ODOMETER” |  |
| `set.rail` | note |  |  |  | 6 items, y = 31 + i·48; icon 20px top-left (43, y+6); label LABEL (track 1) centred x=53 baseline y+40; touch rect x=0 w=106 h=48 |
| `odo.card` | rrect | x=112 y=36 w=356 h=156 r=14 | `surface` |  |  |
| `odo.card.stroke` | rrect stroke 1px (inside) | x=112 y=36 w=356 h=156 r=14 | `border` |  |  |
| `odo.label` | text | left x=128 base=60 | `text_dim` | `LABEL` “TOTAL DISTANCE” |  |
| `odo.value.value` | text | left x=184 base=118 | `text_hi` | `NUM_L` “1 284.6” | block center-aligned at x=290 (value+8px+unit) |
| `odo.value.unit` | text | left x=367 base=118 | `text_dim` | `TITLE` “km” |  |
| `odo.minus` | rrect | x=128 y=132 w=156 h=48 r=16 | `surface_hi` |  |  |
| `odo.minus.stroke` | rrect stroke 1px (inside) | x=128 y=132 w=156 h=48 r=16 | `border` |  |  |
| `odo.minus.text` | text | center x=206 base=162 | `text_hi` | `BODYB` “−10 km” |  |
| `odo.plus` | rrect | x=296 y=132 w=156 h=48 r=16 | `surface_hi` |  |  |
| `odo.plus.stroke` | rrect stroke 1px (inside) | x=296 y=132 w=156 h=48 r=16 | `border` |  |  |
| `odo.plus.text` | text | center x=374 base=162 | `text_hi` | `BODYB` “+10 km” |  |
| `odo.touchMinus` | TOUCH | x=128 y=132 w=156 h=48 |  |  | tap −10 km, hold: repeat (400 ms, then 8/s) |
| `odo.touchPlus` | TOUCH | x=296 y=132 w=156 h=48 |  |  | tap +10 km, hold: repeat |
| `odo.resetCard` | rrect | x=112 y=200 w=356 h=112 r=14 | `surface` |  |  |
| `odo.resetCard.stroke` | rrect stroke 1px (inside) | x=112 y=200 w=356 h=112 r=14 | `border` |  |  |
| `odo.resetLabel` | text | left x=128 base=224 | `text_dim` | `LABEL` “DANGER ZONE” |  |
| `odo.resetBtn` | rrect | x=128 y=238 w=324 h=60 r=14 | `red` @10% |  |  |
| `odo.resetBtn.stroke` | rrect stroke 1.5px (inside) | x=128 y=238 w=324 h=60 r=14 | `red` |  |  |
| `odo.resetText` | text | center x=290 base=274 | `red` | `BODYB` “Hold 2 s to reset odometer” | idle `red`, holding `text_hi` |
| `odo.touchReset` | TOUCH | x=128 y=238 w=324 h=60 |  |  | hold 2 s: reset odometer |
<!-- /GEN:screen_set_odo -->
Hold 2 s to reset: progress fill grows linearly (`set_odo_hold.png`), text becomes `Keep holding… NN%` in `text_hi`.
Release early → fill shrinks back in 200 ms. At 100 %: value animates to `0.0`, button text `Odometer reset` in `green`
for 1.2 s, short 60 ms vibration-free "flash": card stroke `green` for 300 ms. HU: `Tartsd 2 mp-ig a nullázáshoz`,
`Tartsd nyomva… NN%`, `Odométer nullázva`. ±10 km buttons: tap = one step, hold = repeat (400 ms, then 8/s).

### 6.5 Language *(generated)*
<!-- GEN:screen_set_lang -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `set.railItem.icon` | icon | system 20px at x=43 y=37 (top-left) | `text_dim` |  |  |
| `set.railItem.label` | text | center x=53 base=71 track=1 | `text_dim` | `LABEL` “SYSTEM” |  |
| `set.touchRail` | TOUCH | x=0 y=31 w=106 h=48 |  |  | tap: open section |
| `set.railActive` | rrect | x=6 y=226 w=94 h=42 r=12 | `surface_hi` |  |  |
| `set.railActive.bar` | capsule | (7.5,237)→(7.5,257) w=3 | `accent` |  | active indicator |
| `set.railActive.icon` | icon | lang 20px at x=43 y=229 (top-left) | `accent` |  |  |
| `set.railActive.label` | text | center x=53 base=263 track=1 | `text_hi` | `LABEL` “LANGUAGE” |  |
| `set.rail` | note |  |  |  | 6 items, y = 31 + i·48; icon 20px top-left (43, y+6); label LABEL (track 1) centred x=53 baseline y+40; touch rect x=0 w=106 h=48 |
| `lang.cardSel` | rrect | x=112 y=36 w=356 h=128 r=14 | `surface` |  |  |
| `lang.cardSel.stroke` | rrect stroke 2px (inside) | x=112 y=36 w=356 h=128 r=14 | `accent` |  |  |
| `lang.cardSel.mono` | circle | c=(164,100) r=28 | `accent` @16% |  |  |
| `lang.cardSel.monoText` | text | center x=164 base=109 | `accent` | `TITLE` “EN” |  |
| `lang.cardSel.name` | text | left x=208 base=98 | `text_hi` | `TITLE` “English” |  |
| `lang.cardSel.sub` | text | left x=208 base=122 | `text_dim` | `SMALL` “English interface” |  |
| `lang.cardSel.check` | circle | c=(432,100) r=13 | `accent` |  |  |
| `lang.cardSel.touch` | TOUCH | x=112 y=36 w=356 h=128 |  |  | tap: switch language (applies immediately) |
| `lang.card` | rrect | x=112 y=172 w=356 h=128 r=14 | `surface` |  |  |
| `lang.card.stroke` | rrect stroke 1px (inside) | x=112 y=172 w=356 h=128 r=14 | `border` |  |  |
| `lang.card.mono` | circle | c=(164,236) r=28 | `surface_hi` |  |  |
| `lang.card.monoText` | text | center x=164 base=245 | `text_dim` | `TITLE` “HU” |  |
| `lang.card.name` | text | left x=208 base=234 | `text_hi` | `TITLE` “Magyar” |  |
| `lang.card.sub` | text | left x=208 base=258 | `text_dim` | `SMALL` “Magyar nyelvű felület” |  |
| `lang.card.radio` | ring | c=(432,236) r=12 th=2 | `text_faint` |  |  |
| `lang.card.touch` | TOUCH | x=112 y=172 w=356 h=128 |  |  | tap: switch language (applies immediately) |
<!-- /GEN:screen_set_lang -->

### 6.6 WiFi update — on *(generated)*
<!-- GEN:screen_set_wifi_on -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `set.railItem.icon` | icon | system 20px at x=43 y=37 (top-left) | `text_dim` |  |  |
| `set.railItem.label` | text | center x=53 base=71 track=1 | `text_dim` | `LABEL` “SYSTEM” |  |
| `set.touchRail` | TOUCH | x=0 y=31 w=106 h=48 |  |  | tap: open section |
| `set.railActive` | rrect | x=6 y=274 w=94 h=42 r=12 | `surface_hi` |  |  |
| `set.railActive.bar` | capsule | (7.5,285)→(7.5,305) w=3 | `accent` |  | active indicator |
| `set.railActive.icon` | icon | wifi 20px at x=43 y=277 (top-left) | `accent` |  |  |
| `set.railActive.label` | text | center x=53 base=311 track=1 | `text_hi` | `LABEL` “WIFI” |  |
| `set.rail` | note |  |  |  | 6 items, y = 31 + i·48; icon 20px top-left (43, y+6); label LABEL (track 1) centred x=53 baseline y+40; touch rect x=0 w=106 h=48 |
| `wifi.toggleCard` | rrect | x=112 y=36 w=356 h=72 r=14 | `surface` |  |  |
| `wifi.toggleCard.stroke` | rrect stroke 1px (inside) | x=112 y=36 w=356 h=72 r=14 | `accent` @60% |  |  |
| `wifi.title` | text | left x=128 base=66 | `text_hi` | `BODYB` “Update hotspot” |  |
| `wifi.statusDot` | circle | c=(132,85) r=4 | `green` |  |  |
| `wifi.status` | text | left x=142 base=90 | `green` | `SMALL` “On · 1 device connected” |  |
| `wifi.toggle` | rrect | x=384 y=52 w=68 h=40 r=20 | `accent` |  |  |
| `wifi.toggle.knobShadow` | circle | c=(432,73.5) r=16 | `shadow` @25% |  |  |
| `wifi.toggle.knob` | circle | c=(432,72) r=15 | `on_accent` |  | knob animates x over 160 ms ease-out |
| `wifi.touchToggle` | TOUCH | x=112 y=36 w=356 h=72 |  |  | tap: hotspot on/off |
| `wifi.body` | rrect | x=112 y=116 w=356 h=196 r=14 | `surface` |  |  |
| `wifi.body.stroke` | rrect stroke 1px (inside) | x=112 y=116 w=356 h=196 r=14 | `border` |  |  |
| `wifi.qrBox` | rrect | x=124 y=128 w=132 h=132 r=10 | `qr_bg` |  | white in both themes |
| `wifi.qrBox.stroke` | rrect stroke 1px (inside) | x=124 y=128 w=132 h=132 r=10 | `border` |  |  |
| `wifi.qr` | QR modules (rects, no AA) | x=132 y=136 29×29 modules × 4px = 116px | `qr_fg` |  | payload WIFI:T:WPA;S:<ssid>;P:<pass>;; — version 3, ECC L, byte mode (46 B) |
| `wifi.qrHint` | text | center x=190 base=296 | `text_dim` | `SMALL` “Scan to join” |  |
| `wifi.row0.label` | text | left x=274 base=144 | `text_dim` | `LABEL` “NETWORK” |  |
| `wifi.row0.value` | text | left x=274 base=166 | `text_hi` | `BODYB` “WheelieAssist-1A2B” |  |
| `wifi.row1.label` | text | left x=274 base=188 | `text_dim` | `LABEL` “PASSWORD” |  |
| `wifi.row1.value` | text | left x=274 base=210 | `text_hi` | `BODYB` “k7m2qx9vtp” |  |
| `wifi.row2.label` | text | left x=274 base=232 | `text_dim` | `LABEL` “ADDRESS” |  |
| `wifi.row2.value` | text | left x=274 base=254 | `accent` | `BODYB` “http://192.168.4.1” |  |
| `wifi.sep` | hline 1px | x=274 y=268 w=178 | `border` |  |  |
| `wifi.fw` | text | left x=274 base=294 | `text_dim` | `SMALL` “Firmware v4.1.1” |  |
<!-- /GEN:screen_set_wifi_on -->
Status line: `On · N device(s) connected` / `Be · N eszköz csatlakozva`, `green`; on hotspot error:
`On · last update failed` / `Be · a frissítés sikertelen` in `red`. QR: payload `WIFI:T:WPA;S:<ssid>;P:<pass>;;`
(escape `\ ; , : "`), version chosen by length (ECC L); module size = floor(116 / modules) → v3 = 29 modules × 4 px
= 116 px; centre it in the 132×132 white box. SSID in BODYB fits up to 176 px; fall back to BODY if wider.

### 6.7 WiFi update — off *(generated)*
<!-- GEN:screen_set_wifi_off -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `set.railItem.icon` | icon | system 20px at x=43 y=37 (top-left) | `text_dim` |  |  |
| `set.railItem.label` | text | center x=53 base=71 track=1 | `text_dim` | `LABEL` “SYSTEM” |  |
| `set.touchRail` | TOUCH | x=0 y=31 w=106 h=48 |  |  | tap: open section |
| `set.railActive` | rrect | x=6 y=274 w=94 h=42 r=12 | `surface_hi` |  |  |
| `set.railActive.bar` | capsule | (7.5,285)→(7.5,305) w=3 | `accent` |  | active indicator |
| `set.railActive.icon` | icon | wifi 20px at x=43 y=277 (top-left) | `accent` |  |  |
| `set.railActive.label` | text | center x=53 base=311 track=1 | `text_hi` | `LABEL` “WIFI” |  |
| `set.rail` | note |  |  |  | 6 items, y = 31 + i·48; icon 20px top-left (43, y+6); label LABEL (track 1) centred x=53 baseline y+40; touch rect x=0 w=106 h=48 |
| `wifi.toggleCard` | rrect | x=112 y=36 w=356 h=72 r=14 | `surface` |  |  |
| `wifi.toggleCard.stroke` | rrect stroke 1px (inside) | x=112 y=36 w=356 h=72 r=14 | `border` |  |  |
| `wifi.title` | text | left x=128 base=66 | `text_hi` | `BODYB` “Update hotspot” |  |
| `wifi.status` | text | left x=128 base=90 | `text_dim` | `SMALL` “Off · firmware v4.1.1” |  |
| `wifi.toggle` | rrect | x=384 y=52 w=68 h=40 r=20 | `track` |  |  |
| `wifi.toggle.stroke` | rrect stroke 1px (inside) | x=384 y=52 w=68 h=40 r=20 | `border` |  |  |
| `wifi.toggle.knobShadow` | circle | c=(404,73.5) r=16 | `shadow` @25% |  |  |
| `wifi.toggle.knob` | circle | c=(404,72) r=15 | `text_dim` |  | knob animates x over 160 ms ease-out |
| `wifi.touchToggle` | TOUCH | x=112 y=36 w=356 h=72 |  |  | tap: hotspot on/off |
| `wifi.body` | rrect | x=112 y=116 w=356 h=196 r=14 | `surface` |  |  |
| `wifi.body.stroke` | rrect stroke 1px (inside) | x=112 y=116 w=356 h=196 r=14 | `border` |  |  |
| `wifi.howLabel` | text | left x=128 base=140 | `text_dim` | `LABEL` “HOW TO UPDATE” |  |
| `wifi.step0.ring` | ring | c=(142,166) r=12 th=1.5 | `accent` |  |  |
| `wifi.step0.n` | text | center x=142 base=172 | `accent` | `BODYB` “1” |  |
| `wifi.step0.text` | text | left x=166 base=172 | `text` | `BODY` “Download the .bin to your phone” |  |
| `wifi.step1.ring` | ring | c=(142,203) r=12 th=1.5 | `accent` |  |  |
| `wifi.step1.n` | text | center x=142 base=209 | `accent` | `BODYB` “2” |  |
| `wifi.step1.text` | text | left x=166 base=209 | `text` | `BODY` “Turn on the hotspot above” |  |
| `wifi.step2.ring` | ring | c=(142,240) r=12 th=1.5 | `accent` |  |  |
| `wifi.step2.n` | text | center x=142 base=246 | `accent` | `BODYB` “3” |  |
| `wifi.step2.text` | text | left x=166 base=246 | `text` | `BODY` “Scan the QR code to join” |  |
| `wifi.step3.ring` | ring | c=(142,277) r=12 th=1.5 | `accent` |  |  |
| `wifi.step3.n` | text | center x=142 base=283 | `accent` | `BODYB` “4” |  |
| `wifi.step3.text` | text | left x=166 base=283 | `text` | `BODY` “Open 192.168.4.1 and upload” |  |
<!-- /GEN:screen_set_wifi_off -->
HU steps: `Töltsd le a .bin fájlt a telefonra`, `Kapcsold be fent a hotspotot`, `Csatlakozz a QR-kóddal`,
`Nyisd meg: 192.168.4.1, feltöltés`. Label `FRISSÍTÉS LÉPÉSEI`, status `Ki · firmware v4.1.1`.

## 7. Firmware update overlay

Replaces the whole frame (no top bar), input blocked except the error `Close` button.

### 7.1 Uploading *(generated)*
<!-- GEN:screen_ota_progress -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `ota.title` | text | center x=240 base=56 | `text_hi` | `TITLE` “Updating firmware” |  |
| `ota.warnChip` | rrect | x=151.5 y=72 w=177 h=34 r=17 | `amber` @14% |  | width = text + 56, centred |
| `ota.warnIcon` | icon | warn 20px at x=165.5 y=79 (top-left) | `amber` |  |  |
| `ota.warnText` | text | left x=193.5 base=95 | `amber` | `BODYB` “Do not power off” |  |
| `ota.pct` | text | center x=240 base=222 | `text_hi` | `NUM_XL` “64%” |  |
| `ota.barTrack` | capsule (h) | x=64 y=246 w=352 th=12 | `track` |  |  |
| `ota.barFill` | capsule (h) | x=64 y=246 w=225.3 th=12 | `accent` |  | w = 352·p |
| `ota.barGlow` | glow | c=(283.3,246) r=26 | `accent` A=60% (light ×0.55) |  | alpha = A·(1−d/r)² |
| `ota.mb` | text | left x=64 base=284 | `text_dim` | `SMALL` “1.21 / 1.89 MB” |  |
| `ota.fw` | text | right x=416 base=284 | `text_dim` | `SMALL` “Current v4.1.1” |  |
<!-- /GEN:screen_ota_progress -->
Unknown total length: show `NNN KB` is not possible in NUM_XL (no letters) → show the KB number in NUM_XL and `KB`
in TITLE after it; bar becomes an indeterminate 100 px capsule sweeping left↔right (1.2 s period, ease-in-out).
Percentage and bar fill are eased toward the target (τ = 120 ms) so they never jump.

### 7.2 Success *(generated)*
<!-- GEN:screen_ota_success -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `ota.okHalo` | ring | c=(240,118) r=52 th=8 | `green` @18% |  | halo ring, no gradient |
| `ota.okCircle` | circle | c=(240,118) r=44 | `green` |  |  |
| `ota.check1` | capsule | (221,120)→(234,133) w=8 | `on_accent` |  |  |
| `ota.check2` | capsule | (234,133)→(260,105) w=8 | `on_accent` |  |  |
| `ota.okTitle` | text | center x=240 base=206 | `text_hi` | `TITLE` “Update complete” |  |
| `ota.okSub` | text | center x=240 base=236 | `text_dim` | `BODY` “Restarting…” |  |
| `ota.spinner` | arc (round caps) | c=(240,272) r=10 th=3 30°→290° | `accent` |  | spinner, rotates 1 turn / 800 ms |
<!-- /GEN:screen_ota_success -->

### 7.3 Error *(generated)*
<!-- GEN:screen_ota_error -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `ota.errHalo` | ring | c=(240,104) r=48 th=8 | `red` @18% |  | halo ring, no gradient |
| `ota.errCircle` | circle | c=(240,104) r=40 | `red` |  |  |
| `ota.errBar` | capsule | (240,84)→(240,110) w=8 | `on_accent` |  |  |
| `ota.errDot` | circle | c=(240,125) r=5 | `on_accent` |  |  |
| `ota.errTitle` | text | center x=240 base=180 | `text_hi` | `TITLE` “Update failed” |  |
| `ota.errText` | text | center x=240 base=208 | `text` | `BODY` “Invalid image — use the plain .bin, not -full.bin” | g_ota.error, wrap to max 2 lines × 400 px, line pitch 24 |
| `ota.errHint` | text | center x=240 base=234 | `text_dim` | `SMALL` “Hotspot stays on — try again” |  |
| `ota.close` | rrect | x=168 y=254 w=144 h=52 r=14 | `surface_hi` |  |  |
| `ota.close.stroke` | rrect stroke 1px (inside) | x=168 y=254 w=144 h=52 r=14 | `border` |  |  |
| `ota.closeText` | text | center x=240 base=286 | `text_hi` | `BODYB` “Close” |  |
| `ota.touchClose` | TOUCH | x=160 y=250 w=160 h=60 |  |  | tap: dismiss overlay |
<!-- /GEN:screen_ota_error -->

## 8. Boot *(generated)*
<!-- GEN:screen_boot -->
| id | primitive | geometry | colour | font / text | notes |
|---|---|---|---|---|---|
| `boot.logo` | bitmap 320×167 (0x0000 = transparent) | x=80 y=52 | logo_bitmap |  | fade in 0→100 % over 400 ms, ease-out |
| `boot.lineTrack` | capsule (h) | x=180 y=236 w=120 th=3 | `track` |  |  |
| `boot.line` | capsule (h) | x=200 y=236 w=80 th=3 | `accent` |  | loader: grows from centre to 120 px while booting |
| `boot.version` | text | center x=240 base=276 | `text_dim` | `SMALL` “WheelieAssist v4.1.1” |  |
<!-- /GEN:screen_boot -->
Sequence (≈1.2 s total, same as today): 0–400 ms logo alpha 0→1 (`easeOutCubic`); 200–1100 ms loader capsule grows
symmetrically from the centre 0→120 px; version text fades in at 300 ms. Hand-over: dashboard fades in over 200 ms and
the active skin performs a **needle/arc sweep** 0 → full scale → current speed (700 ms, `easeInOutCubic`) — the
classic ignition check.

## 9. Motion & dynamics

| Element | Behaviour |
|---|---|
| Speed digits | Update at most 10×/s (display value), no roll animation (readability on a vibrating bike). |
| Gauge arcs / needle / bars | Critically damped follow of the filtered speed: `x += (target − x)·(1 − e^(−dt/τ))`, τ = 90 ms (FAST filter) / 180 ms (OEM filter). Render every frame. |
| CHRONO needle | Same follow + tiny overshoot: spring ω = 18 rad/s, ζ = 0.75. |
| APEX shift-light | ≥ 97 % of scale: last 4 lit segments alternate `red` / `text_hi` at 6 Hz. |
| Race timer | Recomputed every frame while RUNNING. |
| Race READY dot | Glow alpha pulses 0→80 %→0, 1 Hz sine. |
| Hold rings / progress fills | Linear in time; release → rewind 150–200 ms. |
| Toggle knob | x animates 160 ms `easeOutCubic`; track colour cross-fades. |
| Segmented / option / skin selection | Selection pill slides 160 ms, or instant. |
| Page swipe | See §3. |
| Theme switch | Instant. |

## 10. What the engine needs beyond the listed capabilities

1. **Clip rectangle** for fills (PURE gradient bar fill, odometer reset progress, skin thumbnail corners) — or draw
   those as a rounded rect of the clipped width (equivalent result for the shapes used here).
2. **Horizontal gradient along a capsule** (PURE bar) — a horizontal linear gradient masked by the capsule shape.
3. **RGB565 bitmap blit** for skin thumbnails (144×96) and the QR (plain rects are fine).
4. **Icon masks** 16 px and 20 px, 4bpp: `design/png/icons/icon_<name>_<16|20>.png` (white + alpha; use alpha).
   Names: wifi, system, skin, speed, odo, lang, reset, check, warn, sun, moon, phone, bolt.
5. Extra glyphs listed in §2.3; space advance in NUM_L / NUM_XL.
6. Optional but recommended: a cached static layer (PSRAM framebuffer copy) for CHRONO's dial and HALO's ticks.

No blur, no image scaling at runtime, no conic fills other than arcs.

## 11. Assets

* `design/png/*.png` — every mockup screen, 480×320, RGB565-quantised (compare 1:1 with device screenshots).
* `design/png/thumbs/` — skin picker thumbnails. `design/png/icons/` — icon masks.
* `design/assets/surron_logo.png` — the existing 320×167 logo (transparent where the firmware skips 0x0000).
* `design/serve.py` — dev server + exporter endpoint.
