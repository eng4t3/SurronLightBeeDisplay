// ui_theme.h - design tokens of the WheelieAssist UI (design/DESIGN_SPEC.md §2).
//
// Every colour is an exact RGB565 value, so the mockup hex and the device
// colour are identical (8-bit channels are bit-replicated in the mockups).
#pragma once

#include <Arduino.h>

#include "gfx.h"

namespace ui {

struct Theme {
  uint16_t bg, surface, surface_hi, border, track;
  uint16_t text_hi, text, text_dim, text_faint;
  uint16_t accent, on_accent, green, amber, red, on_amber;
  uint16_t face, metal_hi, metal_lo, qr_bg, qr_fg, shadow;
  float glowK;  // glow alpha multiplier (light theme glows are softer)
  bool dark;
};

constexpr Theme THEME_DARK = {
    gfx::hex(0x080C10), gfx::hex(0x101418), gfx::hex(0x181C21), gfx::hex(0x212831), gfx::hex(0x182029),
    gfx::hex(0xF7FBFF), gfx::hex(0xD6DBDE), gfx::hex(0x8C9AA5), gfx::hex(0x525D6B),
    gfx::hex(0x21C7FF), gfx::hex(0x001018), gfx::hex(0x31D784), gfx::hex(0xFFB221), gfx::hex(0xFF5152),
    gfx::hex(0x181000),
    gfx::hex(0x101418), gfx::hex(0xC6CBD6), gfx::hex(0x39414A), gfx::hex(0xFFFFFF), gfx::hex(0x000000),
    gfx::hex(0x000000),
    1.0f, true,
};

constexpr Theme THEME_LIGHT = {
    gfx::hex(0xEFF3F7), gfx::hex(0xFFFFFF), gfx::hex(0xE7EBEF), gfx::hex(0xCED3DE), gfx::hex(0xDEE3E7),
    gfx::hex(0x000408), gfx::hex(0x182431), gfx::hex(0x4A5563), gfx::hex(0x8C96A5),
    gfx::hex(0x0079C6), gfx::hex(0xFFFFFF), gfx::hex(0x089A4A), gfx::hex(0xC67500), gfx::hex(0xD62439),
    gfx::hex(0xFFFFFF),
    gfx::hex(0xFFFFFF), gfx::hex(0xEFF3F7), gfx::hex(0x94A2AD), gfx::hex(0xFFFFFF), gfx::hex(0x000000),
    gfx::hex(0x4A5563),
    0.55f, false,
};

// Spec checks: the generated token table (RGB565 column).
static_assert(THEME_DARK.bg == 0x0862 && THEME_DARK.accent == 0x263F && THEME_DARK.text_dim == 0x8CD4, "dark tokens");
static_assert(THEME_LIGHT.bg == 0xEF9E && THEME_LIGHT.accent == 0x03D8 && THEME_LIGHT.red == 0xD127, "light tokens");

// Speed zones (fraction of the gauge full scale): flat colour thresholds.
constexpr float ZONE_AMBER = 0.60f;
constexpr float ZONE_RED = 0.85f;

inline uint16_t zoneColor(const Theme &t, float pct) {
  return pct < ZONE_AMBER ? t.accent : (pct < ZONE_RED ? t.amber : t.red);
}

// Zone gradient used along the HALO arc and the PURE bar:
// 0%:accent 50%:accent 72%:amber 86%:amber 97%:red 100%:red
inline void zoneStops(const Theme &t, gfx::Stop *s) {
  s[0] = {0.00f, t.accent};
  s[1] = {0.50f, t.accent};
  s[2] = {0.72f, t.amber};
  s[3] = {0.86f, t.amber};
  s[4] = {0.97f, t.red};
  s[5] = {1.00f, t.red};
}
constexpr int ZONE_STOPS = 6;

// Alpha helpers for the "token @NN%" notation of the spec.
constexpr uint8_t pctA(float p) { return (uint8_t)(p * 255.0f + 0.5f); }

}  // namespace ui
