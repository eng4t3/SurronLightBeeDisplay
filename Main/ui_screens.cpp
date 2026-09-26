// ui_screens.cpp - race timer, settings, firmware update overlay, boot splash,
// frame composition (page swipe), animations and hit testing.
// Coordinates follow design/DESIGN_SPEC.md (generated from design/mockups.html).
#include <math.h>
#include <qrcode_helper.h>  // ricmoo QRCode (ships with the JC3248W535EN library)

#include "ui_draw.h"
#include "version.h"

namespace ui {
namespace {

// ============================================================================
// Animation state (display-only; the app state lives in Main.ino)
// ============================================================================
Anim s_a;
bool s_init = false;
uint32_t s_lastMs = 0;
float s_needleVel = 0.0f;
uint32_t s_spdMs = 0;

// Page position (0 settings .. 2 race); fractional while swiping/settling.
float s_pos = PAGE_DASH;
bool s_settling = false;
float s_settleFrom = 0.0f, s_settleTo = 0.0f;
uint32_t s_settleT0 = 0;
constexpr uint32_t SETTLE_MS = 220;

// WiFi toggle knob transition (160 ms ease-out)
float s_knobFrom = 0.0f, s_knobTo = 0.0f;
uint32_t s_knobT0 = 0;

// Dashboard intro after the boot splash: 200 ms fade in + 700 ms sweep
uint32_t s_introT0 = 0;
bool s_intro = false;
constexpr uint32_t INTRO_FADE_MS = 200, INTRO_SWEEP_MS = 700;

// Screen framebuffer re-based by `dx` columns: drawing at logical x lands on
// screen column x + dx. The framebuffer is column-major (one landscape column
// = H contiguous pixels), so a horizontal translation is just a pointer
// offset; together with a clip to the visible logical columns no write ever
// leaves the real buffer. Integer arithmetic keeps the out-of-range base
// address well-defined.
inline uint16_t *shiftedFb(uint16_t *fb, int dx) {
  return reinterpret_cast<uint16_t *>(reinterpret_cast<uintptr_t>(fb) + (intptr_t)dx * gfx::H * 2);
}

// Exponential follow: x += (target - x)(1 - e^(-dt/tau)), snapping to the
// target once within `eps` (so a stopped gauge really reads 0).
inline float follow(float x, float target, float dt, float tau, float eps) {
  x += (target - x) * (1.0f - expf(-dt / tau));
  return fabsf(target - x) < eps ? target : x;
}

// Hold ring with a linear rewind of `rewindMs` for a full ring after release.
inline float holdAnim(float cur, float in, float dt, float rewindMs) {
  if (in >= 0.0f) return in;
  if (cur <= 0.0f) return -1.0f;
  cur -= dt / rewindMs;
  return cur > 0.0f ? cur : -1.0f;
}

void updateAnim(const View &v) {
  const float dt = s_init ? clampf((float)(v.now_ms - s_lastMs), 0.0f, 100.0f) : 0.0f;
  s_lastMs = v.now_ms;
  if (!s_init) {
    s_a = Anim{};
    s_a.gauge = s_a.needle = v.speed;
    s_a.spd = (int)lroundf(v.speed);
    s_a.holdMax = s_a.holdTrip = -1.0f;
    s_a.knob = s_knobFrom = s_knobTo = v.ota.state != WEBOTA_OFF ? 1.0f : 0.0f;
    s_pos = v.page;
    s_init = true;
  }

  // Gauge follow (critically damped, tau 90 / 180 ms)
  s_a.gauge = follow(s_a.gauge, v.speed, dt, v.gauge_tau_ms, 0.02f);
  // CHRONO needle: spring w = 18 rad/s, zeta = 0.75 (a hint of overshoot)
  {
    constexpr float WN = 18.0f, ZETA = 0.75f;
    const float h = dt / 1000.0f / 4.0f;
    for (int i = 0; i < 4; i++) {
      const float acc = WN * WN * (v.speed - s_a.needle) - 2.0f * ZETA * WN * s_needleVel;
      s_needleVel += acc * h;
      s_a.needle += s_needleVel * h;
    }
  }
  s_a.g = follow(s_a.g, v.g, dt, 90.0f, 0.001f);

  // Speed digits: at most 10 updates/s, with 0.65 hysteresis against flicker
  if (v.now_ms - s_spdMs >= 100) {
    s_spdMs = v.now_ms;
    const int r = (int)lroundf(v.speed);
    if (fabsf(v.speed - (float)s_a.spd) >= 0.65f || r == 0) s_a.spd = r;
  }

  // Ignition sweep after boot: 0 -> full scale -> current (easeInOutCubic)
  if (v.intro) {
    s_intro = true;
    s_introT0 = v.now_ms;
  }
  if (s_intro) {
    const uint32_t t = v.now_ms - s_introT0;
    if (t >= INTRO_SWEEP_MS) {
      s_intro = false;
    } else {
      const float half = INTRO_SWEEP_MS * 0.5f;
      const float g = t < half ? v.full_scale * easeInOutCubic(t / half)
                               : v.full_scale + (v.speed - v.full_scale) * easeInOutCubic((t - half) / half);
      s_a.gauge = s_a.needle = g;
      s_needleVel = 0.0f;
    }
  }

  s_a.holdMax = holdAnim(s_a.holdMax, v.hold_max, dt, 150.0f);
  s_a.holdTrip = holdAnim(s_a.holdTrip, v.hold_trip, dt, 150.0f);
  s_a.holdOdo = v.hold_odo >= 0.0f ? v.hold_odo : fmaxf(0.0f, s_a.holdOdo - dt / 200.0f);

  // WiFi toggle knob
  const float kt = v.ota.state != WEBOTA_OFF ? 1.0f : 0.0f;
  if (kt != s_knobTo) {
    s_knobFrom = s_a.knob;
    s_knobTo = kt;
    s_knobT0 = v.now_ms;
  }
  s_a.knob = s_knobFrom + (s_knobTo - s_knobFrom) * easeOutCubic((v.now_ms - s_knobT0) / 160.0f);

  s_a.pulse = 0.5f - 0.5f * cosf(v.now_ms * (2.0f * (float)M_PI / 1000.0f));
  s_a.otaPct = follow(s_a.otaPct, v.ota.progress_pct, dt, 120.0f, 0.3f);

  // Page position: follows the finger, then settles with easeOutCubic 220 ms
  if (v.dragging) {
    s_pos = clampf(v.page - v.drag_px / 480.0f, -0.35f, (float)PAGE_COUNT - 0.65f);
    s_settling = false;
  } else if (fabsf(s_pos - v.page) > 0.0005f) {
    if (!s_settling || s_settleTo != v.page) {
      s_settling = true;
      s_settleFrom = s_pos;
      s_settleTo = v.page;
      s_settleT0 = v.now_ms;
    }
    const float t = (v.now_ms - s_settleT0) / (float)SETTLE_MS;
    s_pos = t >= 1.0f ? s_settleTo : s_settleFrom + (s_settleTo - s_settleFrom) * easeOutCubic(t);
  } else {
    s_pos = v.page;
    s_settling = false;
  }
}

// ============================================================================
// Race / launch timer (§5)
// ============================================================================
void drawRaceImpl(const View &v, const Anim &a) {
  static const Rect CARDS[5] = {{196, 36, 96, 48}, {16, 228, 134, 84}, {158, 228, 134, 84},
                                {304, 36, 160, 92}, {304, 136, 160, 176}};
  fillBgExcept(CARDS, 5, 14);
  const char *chip;
  uint16_t cc, tc;
  float bar = 0.0f;
  uint16_t bc = T->accent;
  switch (v.race_state) {
    case RACE_RUNNING:
      chip = TR(v, "Pulling…", "Gyorsítás…");
      cc = T->amber;
      tc = v.race_t_done ? T->green : T->accent;
      bar = clampf(v.speed / v.race_target, 0.0f, 1.0f);
      break;
    case RACE_FINISHED:
      chip = TR(v, "Run finished", "Futam kész");
      cc = T->green;
      tc = T->green;
      bar = 1.0f;
      bc = T->green;
      break;
    case RACE_WAIT_STOP:
      chip = TR(v, "Stop to arm", "Állj meg");
      cc = T->text_dim;
      tc = T->text_faint;
      break;
    default:
      chip = TR(v, "Ready to launch", "Rajtra kész");
      cc = T->green;
      tc = T->text_hi;
      break;
  }

  // Status chip
  const int cw = jsRound(textW(chip, BODYB)) + 44;
  gfx::fillRoundRect(16, 44, cw, 32, 16, cc, pctA(0.14f));
  if (v.race_state == RACE_READY) glow(32, 60, 12, T->green, 0.8f * a.pulse);
  gfx::fillCircle(32, 60, 4.5f, cc);
  text(chip, BODYB, 44, 66, cc);

  // Reset button
  card(196, 36, 96, 48, 12, v.pressed == TGT_RACE_RESET ? T->surface_hi : T->surface, T->border);
  icon(IC_RESET20, 212, 50, T->text_dim);
  text(TR(v, "Reset", "Újra"), BODYB, 240, 66, T->text);

  // Timer + progress to the target speed
  char b[16];
  const bool live = v.race_state == RACE_RUNNING || v.race_state == RACE_FINISHED;
  snprintf(b, sizeof(b), "%.2f", live ? clampf(v.race_t, 0.0f, 99.99f) : 0.0f);
  const TextBox tt = text(b, NUM_XL, 14, 186, tc);
  text("s", TITLE, tt.x0 + tt.w + 6, 186, T->text_dim);
  hcapsule(16, 208, 276, 8, T->track);
  if (bar > 0.0f) {
    hcapsule(16, 208, fmaxf(8.0f, 276 * bar), 8, bc);
    if (v.race_state == RACE_RUNNING) glow(16 + 276 * bar - 4, 208, 20, T->accent, 0.6f);
  }

  // BEST / LAST tiles
  auto tile = [&](int x, const char *lab, uint16_t lc, float val, uint16_t vc) {
    card(x, 228, 134, 84, 14, T->surface, T->border);
    text(lab, LABEL, x + 16, 254, lc);
    char t[12];
    snprintf(t, sizeof(t), "%.2f", val > 0.0f ? val : 0.0f);
    valUnit(t, NUM_M, "s", SMALL, x + 16, 292, AL_LEFT, val > 0.0f ? vc : T->text_faint, T->text_dim, 4);
  };
  tile(16, TR(v, "BEST", "LEGJOBB"), T->amber, v.best, T->amber);
  if (v.new_best) {
    const char *nb = TR(v, "NEW", "ÚJ");
    const int bw = jsRound(textW(nb, LABEL, 1.0f)) + 14;
    gfx::fillRoundRect(16 + 134 - 12 - bw, 239, bw, 20, 10, T->amber);
    text(nb, LABEL, 16 + 134 - 12 - bw * 0.5f, 254, T->on_amber, AL_CENTER, 1.0f);
  }
  tile(158, TR(v, "LAST", "UTOLSÓ"), T->text_dim, v.last, T->text_hi);

  // Speed tile
  card(304, 36, 160, 92, 14, T->surface, T->border);
  text(TR(v, "SPEED", "SEBESSÉG"), LABEL, 320, 60, T->text_dim);
  snprintf(b, sizeof(b), "%d", a.spd < 0 ? 0 : (a.spd > 99 ? 99 : a.spd));
  valUnit(b, NUM_L, v.imperial ? "MPH" : "KM/H", LABEL, 320, 112, AL_LEFT, T->text_hi, T->text_dim, 8);

  // Splits
  card(304, 136, 160, 176, 14, T->surface, T->border);
  text(TR(v, "SPLITS", "RÉSZIDŐK"), LABEL, 320, 160, T->text_dim);
  static const char *const LM[3] = {"50–60", "60–70", "70–80"};
  static const char *const LI[3] = {"30–40", "40–50", "50–60"};
  for (int i = 0; i < 3; i++) {
    const int y = 192 + i * 30;
    text(v.imperial ? LI[i] : LM[i], SMALL, 320, y, T->text_dim);
    const bool known = v.split[i] > 0.0f;
    if (known) snprintf(b, sizeof(b), "%.2f", v.split[i]);
    valUnit(known ? b : "—", BODYB, known ? "s" : "", SMALL, 448, y, AL_RIGHT, known ? T->text : T->text_faint,
            T->text_dim, 3);
  }
  gfx::hLine(320, 266, 128, T->border);
  const char *tl = v.imperial ? TR(v, "0–60 total", "0–60 össz.") : TR(v, "0–80 total", "0–80 össz.");
  text(tl, SMALL, 320, 294, T->amber);
  const bool tk = v.total > 0.0f;
  if (tk) snprintf(b, sizeof(b), "%.2f", v.total);
  valUnit(tk ? b : "—", BODYB, tk ? "s" : "", SMALL, 448, 294, AL_RIGHT, tk ? T->amber : T->text_faint, T->amber, 3);
}

// ============================================================================
// Settings (§6): left rail + content column x 112..468
// ============================================================================
constexpr int CX = 112, CW = 356;

void rail(const View &v) {
  static const Icon IC[SEC_COUNT] = {IC_SYSTEM20, IC_SKIN20, IC_SPEED20, IC_ODO20, IC_LANG20, IC_WIFI20};
  static const char *const EN[SEC_COUNT] = {"SYSTEM", "SKIN", "SPEED", "ODOMETER", "LANGUAGE", "WIFI"};
  static const char *const HU[SEC_COUNT] = {"RENDSZER", "STÍLUS", "SEBESSÉG", "ODOMÉTER", "NYELV", "WIFI"};
  for (int i = 0; i < SEC_COUNT; i++) {
    const int y = 31 + i * 48;
    const bool on = v.section == i;
    if (on || v.pressed == TGT_TAB0 + i) gfx::fillRoundRect(6, y + 3, 94, 42, 12, T->surface_hi);
    if (on) capsule(7.5f, y + 14, 7.5f, y + 34, 3, T->accent);
    icon(IC[i], 43, y + 6, on ? T->accent : T->text_dim);
    text(v.hu ? HU[i] : EN[i], LABEL, 53, y + 40, on ? T->text_hi : T->text_dim, AL_CENTER, 1.0f);
  }
}

// Segmented control 164x48 with two options.
void seg(int x, int y, const char *o0, const char *o1, int sel, uint8_t t0, const View &v) {
  card(x, y, 164, 48, 12, T->bg, T->border);
  const float sw = 164.0f / 2;
  const char *opt[2] = {o0, o1};
  for (int i = 0; i < 2; i++) {
    const float sx = x + i * sw;
    if (i == sel) gfx::fillRoundRect(jsRound(sx + 4), y + 4, jsRound(sw - 8), 40, 9, T->accent);
    else if (v.pressed == t0 + i) gfx::fillRoundRect(jsRound(sx + 4), y + 4, jsRound(sw - 8), 40, 9, T->surface_hi);
    text(opt[i], BODYB, (float)jsRound(sx + sw / 2), y + 30, i == sel ? T->on_accent : T->text_dim, AL_CENTER);
  }
}

void setSystem(const View &v) {
  // Brightness
  card(CX, 36, CW, 100, 14, T->surface, T->border);
  text(TR(v, "BRIGHTNESS", "FÉNYERŐ"), LABEL, CX + 16, 60, T->text_dim);
  char b[8];
  snprintf(b, sizeof(b), "%u%%", (unsigned)v.bright_pct);
  text(b, BODYB, CX + CW - 16, 61, T->text_hi, AL_RIGHT);
  const float sx0 = 158, sx1 = 422, sy = 102;
  const float kx = (float)jsRound(sx0 + (sx1 - sx0) * (v.bright_pct - 8) / 92.0f);
  icon(IC_SUN16, CX + 16, (int)sy - 8, T->text_dim);
  icon(IC_SUN22, CX + CW - 16 - 22, (int)sy - 11, T->text_dim);
  hcapsule(sx0, sy, sx1 - sx0, 8, T->track);
  hcapsule(sx0, sy, kx - sx0, 8, T->accent);
  const float kr = v.bright_drag ? 16.0f : 14.0f;  // knob grows while dragged
  gfx::fillCircle(kx, sy + 2, kr + 1, T->shadow, pctA(0.3f));
  gfx::fillCircle(kx, sy, kr, T->dark ? T->text_hi : T->surface);
  ring(kx, sy, kr - 1, 2, T->accent);

  // Units + theme rows
  auto row = [&](int y, const char *title, const char *sub) {
    card(CX, y, CW, 80, 14, T->surface, T->border);
    text(title, BODYB, CX + 16, y + 34, T->text_hi);
    text(sub, SMALL, CX + 16, y + 57, T->text_dim);
  };
  row(144, TR(v, "Units", "Mértékegység"), TR(v, "Speed & distance", "Sebesség és táv"));
  seg(CX + CW - 16 - 164, 160, "km/h", "mph", v.imperial ? 1 : 0, TGT_UNITS_KMH, v);
  row(232, TR(v, "Theme", "Téma"), TR(v, "Night / sunlight", "Éjszakai / nappali"));
  seg(CX + CW - 16 - 164, 248, TR(v, "Dark", "Sötét"), TR(v, "Light", "Világos"), v.light ? 1 : 0, TGT_THEME_DARK,
      v);
}

void setSkin(const View &v) {
  static const char *const NAME[4] = {"HALO", "PURE", "CHRONO", "APEX"};
  static const char *const EN[4] = {"Ring", "Minimal", "Watch", "HUD"};
  static const char *const HU[4] = {"Gyűrű", "Minimál", "Óra", "HUD"};
  constexpr int cw = 174, ch = 134;
  for (int i = 0; i < 4; i++) {
    const int x = CX + (i % 2) * (cw + 8), y = 36 + (i / 2) * (ch + 8);
    const bool on = v.skin == i;
    const uint16_t fill = v.pressed == TGT_SKIN0 + i ? T->surface_hi : T->surface;
    card(x, y, cw, ch, 14, fill, on ? T->accent : T->border, on ? 2.0f : 1.0f);
    const int px = x + 15, py = y + 10;
    thumb(i, v.light, px, py, fill);
    strokeRR(px, py, 144, 96, 8, 1, T->border);
    text(NAME[i], LABEL, x + 16, y + ch - 12, on ? T->text_hi : T->text_dim, AL_LEFT, 2.0f);
    text(v.hu ? HU[i] : EN[i], SMALL, x + cw - 16, y + ch - 11, T->text_dim, AL_RIGHT);
    if (on) {
      gfx::fillCircle(px + 144 - 14, py + 14, 11, T->accent);
      icon(IC_CHECK18, px + 144 - 23, py + 5, T->on_accent);
    }
  }
}

// +/- and text buttons (surface_hi, darker while pressed)
void roundBtn(int x, int y, int w, int h, bool pressed) {
  card(x, y, w, h, 16, pressed ? T->border : T->surface_hi, T->border);
}

void setSpeed(const View &v) {
  card(CX, 36, CW, 116, 14, T->surface, T->border);
  text(TR(v, "CALIBRATION", "KALIBRÁCIÓ"), LABEL, CX + 16, 60, T->text_dim);
  text("0.50 – 2.00", SMALL, CX + CW - 16, 60, T->text_dim, AL_RIGHT);
  roundBtn(CX + 16, 74, 64, 64, v.pressed == TGT_CAL_MINUS);
  capsule(150, 106, 170, 106, 4, T->text_hi);
  roundBtn(CX + CW - 16 - 64, 74, 64, 64, v.pressed == TGT_CAL_PLUS);
  capsule(410, 106, 430, 106, 4, T->text_hi);
  capsule(420, 96, 420, 116, 4, T->text_hi);
  char b[12];
  snprintf(b, sizeof(b), "%.2f", v.cal);
  const float cwv = textW(b, NUM_L);
  const int cx0 = jsRound(CX + CW / 2.0f - (cwv + 16) / 2);
  text(b, NUM_L, cx0, 126, T->text_hi);
  capsule(cx0 + cwv + 7, 110, cx0 + cwv + 17, 120, 3, T->text_dim);  // "×" as two capsules
  capsule(cx0 + cwv + 17, 110, cx0 + cwv + 7, 120, 3, T->text_dim);

  card(CX, 160, CW, 152, 14, T->surface, T->border);
  text(TR(v, "FILTER MODE", "SZŰRÉS"), LABEL, CX + 16, 184, T->text_dim);
  const char *title[2] = {TR(v, "Fast / adaptive", "Gyors / adaptív"), TR(v, "OEM smooth", "Gyári simítás")};
  const char *sub[2] = {TR(v, "Zero lag, instant stop", "Késés nélkül"), TR(v, "12-pulse average", "12 impulzus átlaga")};
  for (int i = 0; i < 2; i++) {
    const int y = 196 + i * 56;
    const bool on = (i == 1) == v.oem;
    if (on) {
      gfx::fillRoundRect(CX + 12, y, CW - 24, 50, 12, T->accent, pctA(0.12f));
      strokeRR(CX + 12, y, CW - 24, 50, 12, 1.5f, T->accent);
    } else {
      gfx::fillRoundRect(CX + 12, y, CW - 24, 50, 12, v.pressed == TGT_FILTER_FAST + i ? T->surface_hi : T->bg);
    }
    ring(CX + 34, y + 25, 9, 2, on ? T->accent : T->text_dim);
    if (on) gfx::fillCircle(CX + 34, y + 25, 4.5f, T->accent);
    text(title[i], BODYB, CX + 54, y + 31, on ? T->text_hi : T->text);
    text(sub[i], SMALL, CX + CW - 28, y + 30, T->text_dim, AL_RIGHT);
  }
}

void setOdo(const View &v, const Anim &a) {
  card(CX, 36, CW, 156, 14, T->surface, T->border);
  text(TR(v, "TOTAL DISTANCE", "ÖSSZES MEGTETT TÁV"), LABEL, CX + 16, 60, T->text_dim);
  char b[24];
  fmtGrouped(b, sizeof(b), v.odo, 1);
  const char *du = v.imperial ? "mi" : "km";
  valUnit(b, NUM_L, du, TITLE, CX + CW / 2.0f, 118, AL_CENTER, T->text_hi, T->text_dim, 8);
  constexpr int bw = (CW - 32 - 12) / 2;
  char t[16];
  roundBtn(CX + 16, 132, bw, 48, v.pressed == TGT_ODO_MINUS);
  snprintf(t, sizeof(t), "−10 %s", du);
  text(t, BODYB, CX + 16 + bw / 2.0f, 162, T->text_hi, AL_CENTER);
  roundBtn(CX + 16 + bw + 12, 132, bw, 48, v.pressed == TGT_ODO_PLUS);
  snprintf(t, sizeof(t), "+10 %s", du);
  text(t, BODYB, CX + 16 + bw + 12 + bw / 2.0f, 162, T->text_hi, AL_CENTER);

  // Danger zone: hold 2 s to reset (progress fill, green confirmation)
  const bool done = v.odo_reset_ms && v.now_ms - v.odo_reset_ms < 1200;
  const bool flash = v.odo_reset_ms && v.now_ms - v.odo_reset_ms < 300;
  card(CX, 200, CW, 112, 14, T->surface, flash ? T->green : T->border, flash ? 2.0f : 1.0f);
  text(TR(v, "DANGER ZONE", "VESZÉLYES"), LABEL, CX + 16, 224, T->text_dim);
  constexpr int bx = CX + 16, by = 238, bW = CW - 32, bH = 60;
  gfx::fillRoundRect(bx, by, bW, bH, 14, T->red, pctA(0.10f));
  const float p = done ? 0.0f : a.holdOdo;
  if (p > 0.0f) {
    // Fill clipped to x < bx + w*p, intersected with the page clip (swipe).
    const gfx::ClipRect c = gfx::getClip();
    const int x1 = bx + jsRound(bW * p);
    gfx::setClip(gfx::ClipRect{(int16_t)max((int)c.x0, bx), (int16_t)max((int)c.y0, by),
                               (int16_t)min((int)c.x1, x1), (int16_t)min((int)c.y1, by + bH)});
    gfx::fillRoundRect(bx, by, bW, bH, 14, T->red, pctA(0.38f));
    gfx::setClip(c);
  }
  strokeRR(bx, by, bW, bH, 14, 1.5f, done ? T->green : T->red);
  const char *lbl;
  uint16_t lc;
  if (done) {
    lbl = TR(v, "Odometer reset", "Odométer nullázva");
    lc = T->green;
  } else if (v.hold_odo >= 0.0f) {
    snprintf(t, sizeof(t), "%d%%", jsRound(p * 100));
    snprintf(b, sizeof(b), "%s %s", TR(v, "Keep holding…", "Tartsd nyomva…"), t);
    lbl = b;
    lc = T->text_hi;
  } else {
    lbl = TR(v, "Hold 2 s to reset odometer", "Tartsd 2 mp-ig a nullázáshoz");
    lc = T->red;
  }
  text(lbl, BODYB, bx + bW / 2.0f, by + bH / 2 + 6, lc, AL_CENTER);
}

void setLang(const View &v) {
  static const char *const MONO[2] = {"EN", "HU"};
  static const char *const NAME[2] = {"English", "Magyar"};
  static const char *const SUB[2] = {"English interface", "Magyar nyelvű felület"};
  for (int i = 0; i < 2; i++) {
    const int y = 36 + i * 136;
    const bool on = (i == 1) == v.hu;
    card(CX, y, CW, 128, 14, v.pressed == TGT_LANG_EN + i ? T->surface_hi : T->surface, on ? T->accent : T->border,
         on ? 2.0f : 1.0f);
    gfx::fillCircle(CX + 52, y + 64, 28, on ? T->accent : T->surface_hi, on ? pctA(0.16f) : 255);
    text(MONO[i], TITLE, CX + 52, y + 73, on ? T->accent : T->text_dim, AL_CENTER);
    text(NAME[i], TITLE, CX + 96, y + 62, T->text_hi);
    text(SUB[i], SMALL, CX + 96, y + 86, T->text_dim);
    if (on) {
      gfx::fillCircle(CX + CW - 36, y + 64, 13, T->accent);
      icon(IC_CHECK20, CX + CW - 46, y + 54, T->on_accent);
    } else {
      ring(CX + CW - 36, y + 64, 12, 2, T->text_faint);
    }
  }
}

// ---- WiFi QR (cached; re-encoded only when the credentials change)
uint8_t s_qrBuf[512];
QRCode s_qr;
bool s_qrOk = false;
char s_qrPayload[240] = "";

// Escapes \ ; , : " as required by the WIFI: QR format.
void wifiEscape(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (; *in && o + 2 < n; ++in) {
    if (strchr("\\;,:\"", *in)) out[o++] = '\\';
    out[o++] = *in;
  }
  out[o] = 0;
}

// Byte-mode capacity for ECC LOW, versions 1..10. The bundled encoder does NOT
// check capacity (it overruns its buffer), so the version is chosen here.
uint8_t qrVersionFor(size_t len) {
  static const uint16_t cap[10] = {17, 32, 53, 78, 106, 134, 154, 192, 230, 271};
  for (uint8_t v = 1; v <= 10; v++) {
    if (len <= cap[v - 1] && qrcode_getBufferSize(v) <= sizeof(s_qrBuf)) return v;
  }
  return 0;
}

void ensureQr(const WebOtaStatus &st) {
  char ssid[70], pass[134], payload[sizeof(s_qrPayload)];
  wifiEscape(st.ssid, ssid, sizeof(ssid));
  wifiEscape(st.password, pass, sizeof(pass));
  if (pass[0]) snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
  else snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ssid);
  if (strcmp(payload, s_qrPayload) == 0) return;
  strlcpy(s_qrPayload, payload, sizeof(s_qrPayload));
  const uint8_t ver = qrVersionFor(strlen(payload));
  s_qrOk = ver && qrcode_initText(&s_qr, s_qrBuf, ver, ECC_LOW, payload) == 0;
}

void drawQr(int bx, int by, int box) {
  card(bx, by, box, box, 10, T->qr_bg, T->border);
  if (!s_qrOk) {
    text("QR ?", BODYB, bx + box / 2.0f, by + box / 2 + 6, T->qr_fg, AL_CENTER);
    return;
  }
  const int n = s_qr.size;
  int m = 116 / n;  // v3: 29 modules x 4 px = 116 px
  if (m < 1) m = 1;
  const int x0 = bx + (box - n * m) / 2, y0 = by + (box - n * m) / 2;
  for (int y = 0; y < n; y++) {
    int x = 0;
    while (x < n) {
      if (!qrcode_getModule(&s_qr, x, y)) {
        x++;
        continue;
      }
      int run = 1;
      while (x + run < n && qrcode_getModule(&s_qr, x + run, y)) run++;
      gfx::fillRect(x0 + x * m, y0 + y * m, run * m, m, T->qr_fg);
      x += run;
    }
  }
}

void setWifi(const View &v, const Anim &a) {
  const bool on = v.ota.state != WEBOTA_OFF;
  const bool err = v.ota.state == WEBOTA_ERROR;
  const float k = a.knob;
  const uint16_t fill = v.pressed == TGT_WIFI_TOGGLE ? T->surface_hi : T->surface;
  card(CX, 36, CW, 72, 14, fill, on ? T->accent : T->border, 1.0f, on ? pctA(0.6f) : 255);
  text(TR(v, "Update hotspot", "Frissítő hotspot"), BODYB, CX + 16, 66, T->text_hi);
  char b[64];
  if (on) {
    const uint16_t c = err ? T->red : T->green;
    gfx::fillCircle(CX + 20, 85, 4, c);
    if (err) {
      strlcpy(b, TR(v, "On · last update failed", "Be · a frissítés sikertelen"), sizeof(b));
    } else if (v.hu) {
      snprintf(b, sizeof(b), "Be · %u eszköz csatlakozva", (unsigned)v.ota.clients);
    } else {
      snprintf(b, sizeof(b), "On · %u device%s connected", (unsigned)v.ota.clients, v.ota.clients == 1 ? "" : "s");
    }
    text(b, SMALL, CX + 30, 90, c);
  } else {
    snprintf(b, sizeof(b), "%s · firmware v%s", TR(v, "Off", "Ki"), FW_VERSION);
    text(b, SMALL, CX + 16, 90, T->text_dim);
  }
  // Toggle 68x40, knob slides 160 ms
  const int tx = CX + CW - 16 - 68;
  gfx::fillRoundRect(tx, 52, 68, 40, 20, gfx::lerp(T->track, T->accent, k));
  if (k < 1.0f) strokeRR(tx, 52, 68, 40, 20, 1, T->border, (uint8_t)(255 * (1.0f - k)));
  const float kx = tx + 20 + 28 * k;
  gfx::fillCircle(kx, 73.5f, 16, T->shadow, pctA(0.25f));
  gfx::fillCircle(kx, 72, 15, gfx::lerp(T->text_dim, T->dark ? T->text_hi : T->surface, k));

  card(CX, 116, CW, 196, 14, T->surface, T->border);
  if (on) {
    ensureQr(v.ota);
    drawQr(CX + 12, 128, 132);
    text(TR(v, "Scan to join", "Olvasd be"), SMALL, CX + 12 + 66, 296, T->text_dim, AL_CENTER);
    const int ix = CX + 12 + 132 + 18;
    const float maxw = CX + CW - 16 - ix;
    char val[80];
    // SSID in BODYB fits up to 176 px; fall back to BODY, then ellipsis.
    const bool bold = textW(v.ota.ssid, BODYB) <= maxw;
    fitText(val, sizeof(val), v.ota.ssid, bold ? BODYB : BODY, maxw);
    text(TR(v, "NETWORK", "HÁLÓZAT"), LABEL, ix, 144, T->text_dim);
    text(val, bold ? BODYB : BODY, ix, 166, T->text_hi);
    text(TR(v, "PASSWORD", "JELSZÓ"), LABEL, ix, 188, T->text_dim);
    fitText(val, sizeof(val), v.ota.password[0] ? v.ota.password : "—", BODYB, maxw);
    text(val, BODYB, ix, 210, T->text_hi);
    text(TR(v, "ADDRESS", "CÍM"), LABEL, ix, 232, T->text_dim);
    snprintf(b, sizeof(b), "http://%s", v.ota.ip[0] ? v.ota.ip : "…");
    fitText(val, sizeof(val), b, BODYB, maxw);
    text(val, BODYB, ix, 254, T->accent);
    gfx::hLine(ix, 268, (int)maxw, T->border);
    snprintf(b, sizeof(b), "Firmware v%s", FW_VERSION);
    text(b, SMALL, ix, 294, T->text_dim);
  } else {
    text(TR(v, "HOW TO UPDATE", "FRISSÍTÉS LÉPÉSEI"), LABEL, CX + 16, 140, T->text_dim);
    const char *steps[4] = {
        TR(v, "Download the .bin to your phone", "Töltsd le a .bin fájlt a telefonra"),
        TR(v, "Turn on the hotspot above", "Kapcsold be fent a hotspotot"),
        TR(v, "Scan the QR code to join", "Csatlakozz a QR-kóddal"),
        TR(v, "Open 192.168.4.1 and upload", "Nyisd meg: 192.168.4.1, feltöltés"),
    };
    for (int i = 0; i < 4; i++) {
      const int cy = 166 + i * 37;
      ring(CX + 30, cy, 12, 1.5f, T->accent);
      char n[2] = {(char)('1' + i), 0};
      text(n, BODYB, CX + 30, cy + 6, T->accent, AL_CENTER);
      text(steps[i], BODY, CX + 54, cy + 6, T->text);
    }
  }
}

void drawSettingsImpl(const View &v, const Anim &a) {
  // Opaque cards of each section (background is not painted under them).
  static const Rect SYS[3] = {{CX, 36, CW, 100}, {CX, 144, CW, 80}, {CX, 232, CW, 80}};
  static const Rect SKN[4] = {{112, 36, 174, 134}, {294, 36, 174, 134}, {112, 178, 174, 134}, {294, 178, 174, 134}};
  static const Rect SPD[2] = {{CX, 36, CW, 116}, {CX, 160, CW, 152}};
  static const Rect ODO[2] = {{CX, 36, CW, 156}, {CX, 200, CW, 112}};
  static const Rect LNG[2] = {{CX, 36, CW, 128}, {CX, 172, CW, 128}};
  static const Rect WIF[2] = {{CX, 36, CW, 72}, {CX, 116, CW, 196}};
  switch (v.section) {
    case SEC_SKIN: fillBgExcept(SKN, 4, 14); break;
    case SEC_SPEED: fillBgExcept(SPD, 2, 14); break;
    case SEC_ODO: fillBgExcept(ODO, 2, 14); break;
    case SEC_LANG: fillBgExcept(LNG, 2, 14); break;
    case SEC_WIFI: fillBgExcept(WIF, 2, 14); break;
    default: fillBgExcept(SYS, 3, 14); break;
  }
  rail(v);
  switch (v.section) {
    case SEC_SKIN: setSkin(v); break;
    case SEC_SPEED: setSpeed(v); break;
    case SEC_ODO: setOdo(v, a); break;
    case SEC_LANG: setLang(v); break;
    case SEC_WIFI: setWifi(v, a); break;
    default: setSystem(v); break;
  }
}

// ============================================================================
// Firmware update overlay (§7) - replaces the whole frame
// ============================================================================
// web_ota.cpp reports errors in English; in Hungarian the known messages are
// translated (exact matches, or a known prefix followed by a library detail).
const char *otaErrorText(const View &v, const char *err, char *buf, size_t n) {
  if (!v.hu) return err;
  static const char *const EXACT[][2] = {
      {"Bike is moving. Stop the bike to update.", "A motor mozog. Állj meg a frissítéshez."},
      {"MD5 must be 32 hexadecimal characters.", "Az MD5 32 hexadecimális karakter legyen."},
      {"No OTA partition (wrong partition table).", "Nincs OTA partíció (rossz partíciós tábla)."},
      {"Firmware is too big for the update slot.", "A firmware túl nagy a frissítési helyhez."},
      {"File is too small to be firmware.", "A fájl túl kicsi egy firmware-hez."},
      {"Not an ESP32 firmware image (bad magic byte).", "Nem ESP32 firmware (hibás fejléc)."},
      {"This is the -full.bin USB image; upload the plain .bin.", "Ez a -full.bin USB-kép; a sima .bin kell."},
      {"Invalid MD5.", "Érvénytelen MD5."},
      {"Hotspot switched off during the upload.", "A hotspot kikapcsolt feltöltés közben."},
      {"Empty file.", "Üres fájl."},
      {"File is too small to be dashboard firmware.", "A fájl túl kicsi a műszerfal firmware-hez."},
      {"Upload incomplete (size mismatch).", "Hiányos feltöltés (méreteltérés)."},
      {"Upload aborted (connection lost).", "A feltöltés megszakadt (kapcsolat elveszett)."},
      {"WiFi failed to start", "A WiFi nem indult el"},
      {"Out of memory (task)", "Nincs elég memória (task)"},
      {"Invalid image — use the plain .bin, not -full.bin", "Hibás fájl — a sima .bin kell, nem a -full.bin"},
  };
  static const char *const PREFIX[][2] = {
      {"Firmware too big: ", "Túl nagy firmware: "},
      {"Update begin failed: ", "A frissítés nem indult: "},
      {"Flash write failed: ", "Flash írási hiba: "},
      {"Verify failed: ", "Ellenőrzési hiba: "},
  };
  for (const auto &e : EXACT)
    if (!strcmp(err, e[0])) return e[1];
  for (const auto &e : PREFIX) {
    const size_t l = strlen(e[0]);
    if (!strncmp(err, e[0], l)) {
      snprintf(buf, n, "%s%s", e[1], err + l);
      return buf;
    }
  }
  return err;
}

// Word-wraps into at most 2 lines of `maxw` px; returns the line count.
int wrap2(const char *s, const FontSpec &f, float maxw, char *l1, char *l2, size_t n) {
  l2[0] = 0;
  if (textW(s, f) <= maxw) {
    strlcpy(l1, s, n);
    return 1;
  }
  size_t cut = 0;
  char tmp[96];
  for (size_t i = 1; s[i] && i < sizeof(tmp) - 1; i++) {
    if (s[i] != ' ') continue;
    memcpy(tmp, s, i);
    tmp[i] = 0;
    if (textW(tmp, f) <= maxw) cut = i;
    else break;
  }
  if (!cut) {
    fitText(l1, n, s, f, maxw);
    return 1;
  }
  memcpy(tmp, s, cut);
  tmp[cut] = 0;
  strlcpy(l1, tmp, n);
  fitText(l2, n, s + cut + 1, f, maxw);
  return 2;
}

void drawOta(const View &v, const Anim &a) {
  gfx::fillRect(0, 0, gfx::W, gfx::H, T->bg);
  char b[64];
  if (v.ota_view == OTA_SUCCESS) {
    ring(240, 118, 52, 8, T->green, pctA(0.18f));
    gfx::fillCircle(240, 118, 44, T->green);
    capsule(221, 120, 234, 133, 8, T->on_accent);
    capsule(234, 133, 260, 105, 8, T->on_accent);
    text(TR(v, "Update complete", "Frissítés kész"), TITLE, 240, 206, T->text_hi, AL_CENTER);
    text(TR(v, "Restarting…", "Újraindul…"), BODY, 240, 236, T->text_dim, AL_CENTER);
    const float rot = (v.now_ms % 800) * (360.0f / 800.0f);  // 1 turn / 800 ms
    arcC(240, 272, 10, 3, 30 + rot, 290 + rot, T->accent);
    return;
  }
  if (v.ota_view == OTA_ERROR) {
    ring(240, 104, 48, 8, T->red, pctA(0.18f));
    gfx::fillCircle(240, 104, 40, T->red);
    capsule(240, 84, 240, 110, 8, T->on_accent);
    gfx::fillCircle(240, 125, 5, T->on_accent);
    text(TR(v, "Update failed", "Frissítés sikertelen"), TITLE, 240, 180, T->text_hi, AL_CENTER);
    char l1[80], l2[80], tr[96];
    const char *err = otaErrorText(v, v.ota.error[0] ? v.ota.error : "?", tr, sizeof(tr));
    const int n = wrap2(err, BODY, 400, l1, l2, sizeof(l1));
    if (n == 1) {
      text(l1, BODY, 240, 208, T->text, AL_CENTER);
      text(TR(v, "Hotspot stays on — try again", "A hotspot bekapcsolva marad — próbáld újra"), SMALL, 240, 234,
           T->text_dim, AL_CENTER);
    } else {  // two lines: the hint makes room
      text(l1, BODY, 240, 206, T->text, AL_CENTER);
      text(l2, BODY, 240, 230, T->text, AL_CENTER);
    }
    card(168, 254, 144, 52, 14, v.pressed == TGT_OTA_CLOSE ? T->border : T->surface_hi, T->border);
    text(TR(v, "Close", "Bezár"), BODYB, 240, 286, T->text_hi, AL_CENTER);
    return;
  }
  // Uploading
  text(TR(v, "Updating firmware", "Firmware frissítés"), TITLE, 240, 56, T->text_hi, AL_CENTER);
  const char *warn = TR(v, "Do not power off", "Ne kapcsold ki!");
  const int cw = jsRound(textW(warn, BODYB)) + 56;
  const int chx = 240 - cw / 2;
  gfx::fillRoundRect(chx, 72, cw, 34, 17, T->amber, pctA(0.14f));
  icon(IC_WARN20, chx + 14, 79, T->amber);
  text(warn, BODYB, chx + 42, 95, T->amber);
  hcapsule(64, 246, 352, 12, T->track);
  if (v.ota.bytes_total > 0) {
    const float p = clampf(a.otaPct / 100.0f, 0.0f, 1.0f);
    snprintf(b, sizeof(b), "%d%%", jsRound(p * 100));
    text(b, NUM_XL, 240, 222, T->text_hi, AL_CENTER);
    if (p > 0.0f) {
      hcapsule(64, 246, fmaxf(12.0f, 352 * p), 12, T->accent);
      glow(64 + 352 * p - 6, 246, 26, T->accent, 0.6f);
    }
    snprintf(b, sizeof(b), "%.2f / %.2f MB", v.ota.bytes_written / 1048576.0f, v.ota.bytes_total / 1048576.0f);
    text(b, SMALL, 64, 284, T->text_dim);
  } else {
    // Unknown length: KB counter + indeterminate sweep (1.2 s, ease-in-out)
    snprintf(b, sizeof(b), "%lu", (unsigned long)(v.ota.bytes_written / 1024));
    const float wn = textW(b, NUM_XL), wu = textW("KB", TITLE);
    const float x0 = 240 - (wn + 8 + wu) / 2;
    text(b, NUM_XL, (float)jsRound(x0), 222, T->text_hi);
    text("KB", TITLE, (float)jsRound(x0 + wn + 8), 222, T->text_dim);
    const float ph = (v.now_ms % 2400) / 1200.0f;  // there and back
    const float t = easeInOutCubic(ph < 1.0f ? ph : 2.0f - ph);
    hcapsule(64 + (352 - 100) * t, 246, 100, 12, T->accent);
  }
  snprintf(b, sizeof(b), "%s v%s", TR(v, "Current", "Jelenlegi"), FW_VERSION);
  text(b, SMALL, 416, 284, T->text_dim, AL_RIGHT);
}

// ============================================================================
// Frame composition
// ============================================================================
void drawPage(int p, const View &v) {
  switch (p) {
    case PAGE_SETTINGS: drawSettingsImpl(v, s_a); break;
    case PAGE_RACE: drawRaceImpl(v, s_a); break;
    default: drawDash(v, s_a); break;
  }
}

const char *pageTitle(int p, const View &v, char *buf, size_t n) {
  if (p == PAGE_SETTINGS) return TR(v, "SETTINGS", "BEÁLLÍTÁSOK");
  if (p == PAGE_RACE) {
    snprintf(buf, n, "%s · %s", TR(v, "RACE", "VERSENY"), v.imperial ? "0–30 MPH" : "0–50 KM/H");
    return buf;
  }
  return "";
}

}  // namespace


void begin() { s_init = false; }

void drawBoot(uint32_t t) {
  T = &THEME_DARK;  // boot is always dark
  gfx::clipReset();
  gfx::clear(T->bg);
  logo(80, 52, (uint8_t)(255 * easeOutCubic(t / 400.0f)));
  hcapsule(180, 236, 120, 3, T->track);
  const float w = 120 * easeInOutCubic((t - 200.0f) / 900.0f);
  if (t > 200 && w > 0.5f) hcapsule(240 - w / 2, 236, w, 3, T->accent);
  if (t >= 300) {
    char b[40];
    snprintf(b, sizeof(b), "%s v%s", FW_NAME, FW_VERSION);
    text(b, SMALL, 240, 276, T->text_dim, AL_CENTER, TRACK_DEFAULT,
         (uint8_t)(255 * clampf((t - 300) / 300.0f, 0.0f, 1.0f)));
  }
}

void frame(const View &v) {
  T = v.light ? &THEME_LIGHT : &THEME_DARK;
  updateAnim(v);
  gfx::clipReset();
  gfx::drawTo(nullptr);

  if (v.ota_view != OTA_NONE) {
    drawOta(v, s_a);
    return;
  }

  // Pages: a single page, or while swiping two neighbours side by side, each
  // drawn straight into the screen through a translated framebuffer base and
  // clipped to its visible columns (no off-screen copy).
  const float pos = s_pos;
  int pa = (int)floorf(pos);
  int shift = jsRound((pos - pa) * gfx::W);  // columns page `pa` has moved to the left
  if (shift >= gfx::W) { pa++; shift = 0; }
  if (shift == 0) {
    drawPage(pa < 0 ? 0 : (pa >= PAGE_COUNT ? PAGE_COUNT - 1 : pa), v);
  } else {
    uint16_t *fb = gfx::framebuffer();
    gfx::drawTo(shiftedFb(fb, -shift));  // page pa: logical [shift, W) -> screen [0, W - shift)
    gfx::clip(shift, 0, gfx::W - shift, gfx::H);
    if (pa >= 0) drawPage(pa, v);
    else gfx::fillRect(0, 0, gfx::W, gfx::H, T->bg);  // rubber band before the first page
    gfx::drawTo(shiftedFb(fb, gfx::W - shift));  // page pa+1: logical [0, shift) -> screen [W - shift, W)
    gfx::clip(0, 0, shift, gfx::H);
    if (pa + 1 < PAGE_COUNT) drawPage(pa + 1, v);
    else gfx::fillRect(0, 0, gfx::W, gfx::H, T->bg);  // rubber band after the last page
    gfx::drawTo(nullptr);
    gfx::clipReset();
  }

  char tb[40];
  const int pn = jsRound(clampf(pos, 0.0f, PAGE_COUNT - 1));
  topBar(clampf(pos, 0.0f, PAGE_COUNT - 1), pageTitle(pn, v, tb, sizeof(tb)), v.ota.state != WEBOTA_OFF,
         v.ota.state == WEBOTA_ERROR, v.demo);

  if (s_introT0 && v.now_ms - s_introT0 < INTRO_FADE_MS) {  // dashboard fades in after the splash
    const float t = (v.now_ms - s_introT0) / (float)INTRO_FADE_MS;
    gfx::fillRectAlpha(0, 0, gfx::W, gfx::H, T->bg, (uint8_t)(255 * (1.0f - t)));
  }
}

// ============================================================================
// Hit testing (TOUCH rows of the spec)
// ============================================================================
bool targetRect(const View &v, uint8_t t, Rect &r) {
  if (v.ota_view != OTA_NONE) {
    if (t == TGT_OTA_CLOSE && v.ota_view == OTA_ERROR) {
      r = {160, 250, 160, 60};
      return true;
    }
    return false;
  }
  if (v.page == PAGE_DASH) return dashTarget(v, t, r);
  if (v.page == PAGE_RACE) {
    if (t != TGT_RACE_RESET) return false;
    r = {196, 32, 104, 56};
    return true;
  }
  // Settings
  if (t >= TGT_TAB0 && t <= TGT_TAB5) {
    r = {0, (int16_t)(31 + (t - TGT_TAB0) * 48), 106, 48};
    return true;
  }
  if (t == TGT_DEMO) {
    r = {0, 0, 200, 30};
    return true;
  }
  switch (v.section) {
    case SEC_SYSTEM:
      if (t == TGT_BRIGHT) { r = {112, 74, 356, 56}; return true; }
      if (t == TGT_UNITS_KMH) { r = {288, 160, 82, 48}; return true; }
      if (t == TGT_UNITS_MPH) { r = {370, 160, 82, 48}; return true; }
      if (t == TGT_THEME_DARK) { r = {288, 248, 82, 48}; return true; }
      if (t == TGT_THEME_LIGHT) { r = {370, 248, 82, 48}; return true; }
      return false;
    case SEC_SKIN:
      if (t >= TGT_SKIN0 && t <= TGT_SKIN3) {
        const int i = t - TGT_SKIN0;
        r = {(int16_t)(112 + (i % 2) * 182), (int16_t)(36 + (i / 2) * 142), 174, 134};
        return true;
      }
      return false;
    case SEC_SPEED:
      if (t == TGT_CAL_MINUS) { r = {120, 70, 80, 72}; return true; }
      if (t == TGT_CAL_PLUS) { r = {380, 70, 80, 72}; return true; }
      if (t == TGT_FILTER_FAST) { r = {124, 196, 332, 50}; return true; }
      if (t == TGT_FILTER_OEM) { r = {124, 252, 332, 50}; return true; }
      return false;
    case SEC_ODO:
      if (t == TGT_ODO_MINUS) { r = {128, 132, 156, 48}; return true; }
      if (t == TGT_ODO_PLUS) { r = {296, 132, 156, 48}; return true; }
      if (t == TGT_ODO_RESET) { r = {128, 238, 324, 60}; return true; }
      return false;
    case SEC_LANG:
      if (t == TGT_LANG_EN) { r = {112, 36, 356, 128}; return true; }
      if (t == TGT_LANG_HU) { r = {112, 172, 356, 128}; return true; }
      return false;
    case SEC_WIFI:
      if (t == TGT_WIFI_TOGGLE) { r = {112, 36, 356, 72}; return true; }
      return false;
    default:
      return false;
  }
}

uint8_t hitTest(const View &v, int x, int y) {
  // Every target is tested; targetRect() only answers for the visible layout.
  for (uint8_t t = TGT_NONE + 1; t <= TGT_OTA_CLOSE; t++) {
    Rect r;
    if (targetRect(v, t, r) && r.contains(x, y)) return t;
  }
  return TGT_NONE;
}

}  // namespace ui
