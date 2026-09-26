// ui.h - the WheelieAssist user interface (design/DESIGN_SPEC.md).
//
// Main.ino owns all application state. Once per frame it fills a ui::View
// from its telemetry/settings and calls ui::frame(view); the UI draws the
// complete frame with the gfx engine and never reads application globals.
// Layout-dependent input (hit testing, touch rects) lives here too, so
// drawing and touch can never disagree; the gesture engine stays in Main.ino.
//
//   ui_theme.h     colour tokens
//   ui_draw.h/.cpp shared drawing helpers (text layout, cards, icons, top bar)
//   ui_dash.cpp    dashboard skins HALO / PURE / CHRONO / APEX
//   ui_screens.cpp race, settings, firmware update overlay, boot, frame composition
#pragma once

#include <Arduino.h>

#include "web_ota.h"

namespace ui {

// ---------------------------------------------------------------- enums
enum Page : uint8_t { PAGE_SETTINGS = 0, PAGE_DASH = 1, PAGE_RACE = 2, PAGE_COUNT = 3 };

// Skin index == NVS "skin" value.
enum Skin : uint8_t { SKIN_HALO = 0, SKIN_PURE = 1, SKIN_CHRONO = 2, SKIN_APEX = 3, SKIN_COUNT = 4 };

enum Section : uint8_t {
  SEC_SYSTEM = 0,
  SEC_SKIN = 1,
  SEC_SPEED = 2,
  SEC_ODO = 3,
  SEC_LANG = 4,
  SEC_WIFI = 5,
  SEC_COUNT = 6
};

// Same values as Main.ino's AccelTimerState.
enum RaceState : uint8_t { RACE_READY = 0, RACE_RUNNING = 1, RACE_FINISHED = 2, RACE_WAIT_STOP = 3 };

enum OtaView : uint8_t { OTA_NONE = 0, OTA_PROGRESS, OTA_SUCCESS, OTA_ERROR };

// Touch targets (hit-tested by ui::hitTest, acted upon by Main.ino).
enum Target : uint8_t {
  TGT_NONE = 0,
  TGT_TAB0, TGT_TAB1, TGT_TAB2, TGT_TAB3, TGT_TAB4, TGT_TAB5,  // settings rail
  TGT_BRIGHT,
  TGT_UNITS_KMH, TGT_UNITS_MPH,
  TGT_THEME_DARK, TGT_THEME_LIGHT,
  TGT_SKIN0, TGT_SKIN1, TGT_SKIN2, TGT_SKIN3,
  TGT_CAL_MINUS, TGT_CAL_PLUS, TGT_FILTER_FAST, TGT_FILTER_OEM,
  TGT_ODO_MINUS, TGT_ODO_PLUS, TGT_ODO_RESET,
  TGT_LANG_EN, TGT_LANG_HU,
  TGT_WIFI_TOGGLE,
  TGT_TRIP_RESET, TGT_MAX_RESET,
  TGT_RACE_RESET,
  TGT_DEMO,       // hidden: hold the SETTINGS title 3 s
  TGT_OTA_CLOSE,  // error overlay
};

struct Rect {
  int16_t x, y, w, h;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// ---------------------------------------------------------------- view state
constexpr int G_HIST = 40;  // APEX sparkline: 40 samples at 10 Hz = 4 s

struct View {
  uint32_t now_ms;

  // Navigation / settings
  uint8_t page;        // Page
  int16_t drag_px;     // page swipe: finger offset while dragging (0 otherwise)
  bool dragging;
  uint8_t skin;        // Skin
  uint8_t section;     // Section
  bool light, hu, imperial, demo;
  bool intro;          // true on the first frame after the boot splash

  // Telemetry, already converted to the user's units (km/h|mph, km|mi)
  float speed;         // filtered display speed
  float full_scale;    // gauge full scale (85 km/h / 50 mph)
  float gauge_tau_ms;  // needle/arc follow time constant (90 FAST / 180 OEM)
  float max_speed;     // session max
  uint32_t ride_s;
  double trip, odo;
  float g, peak_g;     // longitudinal G (forward only), session peak
  float g_hist[G_HIST];  // oldest first

  // Race
  uint8_t race_state;  // RaceState
  float race_t;        // timer value (live while running)
  bool race_t_done;    // 0-50 reached (value frozen, shown green)
  float race_target;   // 50 km/h or 30 mph, in user units
  float best, last;    // <= 0: never set
  bool new_best;       // the finished run set a new best
  float split[3];      // <= 0: unknown
  float total;         // 0-80 total, <= 0: unknown

  // System settings
  uint8_t bright_pct;  // 8..100
  float cal;
  bool oem;

  // Hotspot / firmware update
  WebOtaStatus ota;
  uint8_t ota_view;    // OtaView (overlay)

  // Interaction feedback
  uint8_t pressed;     // target under a held finger (TGT_NONE if none)
  float hold_max, hold_trip, hold_odo;  // 0..1 hold-to-reset progress (0 = not holding)
  uint32_t max_reset_ms, trip_reset_ms, odo_reset_ms;  // now_ms of the last reset (0 = never)
  bool bright_drag;    // brightness knob is being dragged
};

// ---------------------------------------------------------------- API
void begin();

// Boot splash at `t_ms` since power-up (0..BOOT_MS), always dark.
constexpr uint32_t BOOT_MS = 1200;
void drawBoot(uint32_t t_ms);

// Draws the complete frame (every pixel) into the current gfx framebuffer.
void frame(const View &v);

// Hit testing for the current layout. `page` is the page under the finger.
uint8_t hitTest(const View &v, int x, int y);
bool targetRect(const View &v, uint8_t target, Rect &r);

}  // namespace ui
