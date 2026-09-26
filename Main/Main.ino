/*
  ================================================================================
  LIGHTBEE DISPLAY: Sur-Ron speed & acceleration telemetry cluster
  Target: Guition JC3248W535 (ESP32-S3, 16MB flash, 8MB OPI PSRAM,
          480x320 landscape QSPI IPS AXS15231B + capacitive touch)
  ================================================================================

  Task layout
    - speedPulseISR          GPIO ISR (attached from setup() -> core 1)
    - UITask   (core 1, prio 2)  speed engine, odometer, race timer, touch, 30 fps UI
    - WiFi / lwIP / web OTA  (core 0, owned by web_ota.cpp; off by default)

  Shared state
    - ISR pulse state        guarded by g_pulse_mux (spinlock, ISR-safe)
    - odo/trip (doubles)     guarded by g_tel_mux   (spinlock)
    - NVS / Preferences      guarded by g_nvs_mutex (FreeRTOS mutex)
    - other telemetry        single 32-bit values (atomic on Xtensa)

  Drawing: once per frame the UI task fills a ui::View from the telemetry and
  settings below and ui::frame() draws the whole frame with the anti-aliased
  gfx engine (ui_*.cpp, see ui.h). lcd::present() hands the finished frame to
  a flush task on core 0 (double buffered) while the next one is drawn.
*/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <JC3248W535EN_Touch_LCD.h>

#include "version.h"
#include "web_ota.h"
#include "app_bridge.h"
#include "gfx.h"            // anti-aliased engine
#include "ui.h"             // user interface (view state, drawing, hit testing)
#include "debug_console.h"  // serial console: screenshots, remote control
#include "lcd_flush.h"      // async double-buffered panel flush

// ================================================================================
// 1. PIN DEFINITIONS & CONSTANTS
// ================================================================================

#define PIN_SPEED_SENSOR 17  // Speed pulse signal (4-pin Extended IO P3/P4 Pin 3)

// Sur-Ron 2025 Light Bee X speed pulse calibration (18" rear wheel)
const float WHEEL_CIRCUMFERENCE_M  = 1.88f;
const float PULSES_PER_WHEEL_REV   = 77.65f;
const float METERS_PER_SPEED_PULSE = WHEEL_CIRCUMFERENCE_M / PULSES_PER_WHEEL_REV;  // ~0.02421 m

const float KMH_TO_MPH = 0.621371f;

// Factory default for the speed calibration multiplier: 1.10 matches GPS on a
// 2025 Light Bee X with the 18" rear wheel. Only used when NVS has no value.
const float DEFAULT_SPEED_CAL = 1.10f;

// Pulse engine timing (64-bit esp_timer microseconds: no 71-minute micros() wrap)
const uint32_t SPEED_DEBOUNCE_US = 500;       // 100+ km/h is ~870 us/pulse
const int64_t  FAST_TIMEOUT_US   = 250000;    // Fast mode: 0 km/h after 250 ms without pulses
const int64_t  OEM_TIMEOUT_US    = 1200000;   // OEM mode: 0 km/h after 1.2 s
const int64_t  OEM_DECAY_START_US = 200000;
#define OEM_SLOTS 12

// Gauge full scale
const float GAUGE_MAX_KMH = 85.0f;
const float GAUGE_MAX_MPH = 50.0f;

// Display & touch
#define GFX_BL 1  // LEDC backlight PWM

// SD card (optional)
#define SD_CS    10
#define SPI_MOSI 11
#define SPI_SCK  12
#define SPI_MISO 13

#define NVS_NS "surron_dash"

// ================================================================================
// 2. TYPES (all declared before the first function so Arduino's generated
//    prototypes can reference them)
// ================================================================================

enum ScreenState : uint8_t {
  SCREEN_BOOT = 0,
  SCREEN_SETTINGS = 1,
  SCREEN_DASHBOARD = 2,
  SCREEN_RACE = 3
};

enum AccelTimerState : uint8_t {
  ACCEL_READY = 0,     // Stopped, armed for a launch
  ACCEL_RUNNING = 1,   // Timing a run
  ACCEL_FINISHED = 2,  // Reached the last split
  ACCEL_WAIT_STOP = 3  // Reset/aborted while moving: re-arms at standstill
};

enum SettingsSubmenu : uint8_t {
  SUB_SYSTEM = 0,
  SUB_SKIN = 1,
  SUB_SPEED = 2,
  SUB_ODO = 3,
  SUB_LANGUAGE = 4,
  SUB_WIFI = 5,
  SUB_COUNT = 6
};

enum Language : uint8_t { LANG_EN = 0, LANG_HU = 1 };

struct PulseSnapshot {
  uint32_t count;         // accepted pulses since boot
  int64_t  last_edge_us;  // esp_timer time of the latest pulse
  uint32_t interval_us;   // interval before the latest pulse
};

struct SpeedEngine {
  // Fast reciprocal estimator
  uint32_t ref_count;       // pulse count at previous update
  int64_t  ref_edge_us;     // timestamp of the latest pulse at previous update
  bool     moving;
  float    fast_kmh;
  int64_t  fast_t_us;       // moment fast_kmh refers to (middle of its pulse window)
  int64_t  motion_start_us; // first pulse after standstill (launch timestamp)
  // OEM smooth estimator: 12-pulse moving average of pulse intervals
  uint32_t oem_hist[OEM_SLOTS];
  uint8_t  oem_n, oem_idx;
  float    oem_kmh;
};

struct PersistState {
  int      bright;
  float    cal;
  bool     oem;
  double   odo;
  double   trip;
  uint32_t ride;
  int      skin;
  bool     imperial;
  bool     light;
  float    best_m;   // best 0-50 km/h
  float    best_i;   // best 0-30 mph
  int      lang;
};

struct AccSample {
  int64_t t_us;
  float   v_kmh;
};

enum TouchKind : uint8_t { TK_NONE = 0, TK_TAP, TK_REPEAT, TK_DRAG, TK_LONG };

// Touch targets are ui::Target (hit testing lives with the layout in ui_*.cpp).
struct TouchState {
  bool     down;
  int16_t  x0, y0, x, y;
  uint32_t t0;
  uint32_t last_repeat;
  uint8_t  target;     // ui::Target under the finger at touch-down
  uint8_t  kind;       // TouchKind
  bool     no_swipe;   // drag / repeat targets and overlays never swipe
  bool     sliding;    // horizontal page drag in progress
  bool     cancelled;
  bool     fired;
  float    vx;         // smoothed horizontal finger velocity (px/ms) for flings
  uint32_t last_ms;
};

// ================================================================================
// 3. GLOBAL STATE
// ================================================================================

SPIClass sdSPI(HSPI);
volatile bool sd_active = false;

// --- Distance (doubles are not atomic on ESP32: only touch under g_tel_mux) ---
static portMUX_TYPE g_tel_mux = portMUX_INITIALIZER_UNLOCKED;
static double g_odo_km = 0.0;
static double g_trip_km = 0.0;
volatile uint32_t ride_seconds = 0;  // accumulated time moving > 1 km/h

// --- Settings ---
volatile ui::Skin current_skin = ui::SKIN_HALO;  // NVS "skin" 0..3: HALO, PURE, CHRONO, APEX
volatile int screen_brightness = 255;
volatile bool imperial_mode = false;
volatile bool theme_light = false;
volatile bool demo_mode = false;       // hidden: hold the SETTINGS title for 3 s
volatile float speed_cal = DEFAULT_SPEED_CAL;
volatile bool speed_filter_oem = false;
volatile Language current_lang = LANG_EN;

static bool settings_dirty = false;          // deferred save (hold-to-repeat, slider)
static uint32_t last_settings_change_ms = 0;
static bool save_requested = false;          // save at end of this frame
static double dist_unsaved_km = 0.0;         // driven distance since the last save (UI task)

// --- Speed sensor ISR state (guarded by g_pulse_mux) ---
static portMUX_TYPE g_pulse_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t isr_pulse_count = 0;
static volatile int64_t  isr_last_edge_us = 0;
static volatile uint32_t isr_last_interval_us = 0;

static SpeedEngine eng;  // UI task only

volatile float bike_speed_kmh = 0.0f;    // filtered display speed
volatile float raw_speed_kmh = 0.0f;     // unfiltered reciprocal speed
volatile float session_max_speed = 0.0f;
volatile float current_accel_g = 0.0f;
volatile int current_accel_pct = 0;

// --- Demo ---
static float demo_speed = 0.0f;
static int64_t demo_start_us = 0;

// --- Race timer (UI task only, except best times which are 32-bit) ---
volatile AccelTimerState accel_timer_state = ACCEL_READY;
static int64_t t_launch_us = 0, t_50_us = 0, t_60_us = 0, t_70_us = 0, t_80_us = 0;
static bool run_imperial = false;       // unit system latched at launch
static bool last_run_imperial = false;
volatile float current_0_50_time = 0.0f;
volatile float last_0_50_time = 0.0f;
volatile float best_0_50_time = 0.0f;      // metric 0-50 km/h   (NVS "best_0_50")
volatile float best_0_30mph_time = 0.0f;   // imperial 0-30 mph  (NVS "best_0_30i")
volatile float split_50_60 = 0.0f;
volatile float split_60_70 = 0.0f;
volatile float split_70_80 = 0.0f;
volatile float time_0_80 = 0.0f;

// 0-50 / 50-60 / 60-70 / 70-80 km/h   and   0-30 / 30-40 / 40-50 / 50-60 mph
static const float RACE_THR_METRIC[4]   = {50.0f, 60.0f, 70.0f, 80.0f};
// A 0-50 km/h (0-30 mph) run faster than this would need > ~0.95 G average:
// impossible on a Light Bee, so it comes from noise pulses and is never a record.
static const float RACE_MIN_PLAUSIBLE_S = 1.5f;
static const float RACE_THR_IMPERIAL[4] = {48.2803f, 64.3738f, 80.4672f, 96.5606f};

// --- Persistence ---
Preferences preferences;
static SemaphoreHandle_t g_nvs_mutex = NULL;
static PersistState g_nvs;        // last values written to / read from NVS
static bool g_nvs_valid = false;

// --- Display / UI ---
JC3248W535EN display;
volatile bool display_initialized = false;
ScreenState current_screen = SCREEN_BOOT;
SettingsSubmenu active_submenu = SUB_SYSTEM;
static TouchState T;
static WebOtaStatus g_ota;              // refreshed once per frame
static uint32_t g_frame_ms = 0;         // millis() at the start of the frame
static ui::View g_view;                 // what the UI shows (also used for hit testing)

// --- UI-side telemetry and feedback (UI task only) ---
static float g_hist[ui::G_HIST];        // APEX: longitudinal G, last 4 s at 10 Hz
static float g_peak_g = 0.0f;           // session peak G (resets with the session max)
static uint32_t g_max_reset_ms = 0, g_trip_reset_ms = 0, g_odo_reset_ms = 0;
static uint8_t g_tap_target = ui::TGT_NONE;  // short pressed flash after a tap
static uint32_t g_tap_ms = 0;
static bool g_ota_err_overlay = false;  // failed upload: error overlay until "Close"
static bool g_intro_pending = false;    // first dashboard frame after the splash

// Debug console mock data (display only, never saved): the mockups' sample
// ride, race and update states so screenshots can be compared 1:1.
enum MockRace : uint8_t { MR_OFF = 0, MR_READY, MR_PULLING, MR_FINISHED, MR_STOP };
static struct {
  bool on;              // sample ride data: max 74, ride 42:18, trip 18.4, odo 1 284.6
  bool wifi, demo;      // fake status icons in the top bar
  uint8_t race;         // MockRace
  uint8_t ota;          // ui::OtaView
  float hold_max, hold_trip, hold_odo;  // fake hold progress (< 0 = off)
  int16_t boot;         // >= 0: show the boot splash frozen at this time (ms)
} g_mock = {false, false, false, MR_OFF, ui::OTA_NONE, -1.0f, -1.0f, -1.0f, -1};

// ================================================================================
// 4. SPEED SENSOR ISR & SPEED ENGINE
// ================================================================================

// Speed pulse ISR with EMI debounce. Count, timestamp and interval are updated
// together inside the spinlock so readers always get a consistent triple.
void IRAM_ATTR speedPulseISR() {
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&g_pulse_mux);
  const int64_t interval = now - isr_last_edge_us;
  if (interval > (int64_t)SPEED_DEBOUNCE_US) {
    isr_last_interval_us = (interval > 0xFFFFFFFFLL) ? 0xFFFFFFFFu : (uint32_t)interval;
    isr_last_edge_us = now;
    isr_pulse_count = isr_pulse_count + 1;
  }
  portEXIT_CRITICAL_ISR(&g_pulse_mux);
}

static PulseSnapshot readPulseSnapshot() {
  PulseSnapshot s;
  portENTER_CRITICAL(&g_pulse_mux);
  s.count = isr_pulse_count;
  s.last_edge_us = isr_last_edge_us;
  s.interval_us = isr_last_interval_us;
  portEXIT_CRITICAL(&g_pulse_mux);
  return s;
}

static void oemPush(uint32_t interval_us, uint32_t times) {
  if (interval_us == 0) return;
  if (times > OEM_SLOTS) times = OEM_SLOTS;
  while (times--) {
    eng.oem_hist[eng.oem_idx] = interval_us;
    eng.oem_idx = (eng.oem_idx + 1) % OEM_SLOTS;
    if (eng.oem_n < OEM_SLOTS) eng.oem_n++;
  }
}

// One engine step per frame. `now_us` MUST be read after the snapshot so that
// now_us >= last_edge_us (no negative/underflowing age).
//
// Fast mode = true reciprocal counting: speed = (new pulses) x (m/pulse) /
// (time between the latest edge now and the latest edge at the previous
// update). Both ends of the window are pulse edges, so there is no +-1 pulse
// quantization. Between pulses the estimate only decays once the next pulse is
// overdue (bounded by 1 pulse / time since last edge), and drops to 0 after
// FAST_TIMEOUT_US.
static void speedEngineUpdate(const PulseSnapshot &s, int64_t now_us, float cal) {
  const float K = METERS_PER_SPEED_PULSE * cal * 3600000.0f;  // km/h x us for one pulse
  const bool have_edge = (s.count != 0);
  int64_t age = have_edge ? (now_us - s.last_edge_us) : INT64_MAX;
  if (age < 0) age = 0;
  const uint32_t dn = s.count - eng.ref_count;
  const bool was_moving = eng.moving;

  // An average speed over a pulse window is the true speed at the window's
  // middle (exact for constant acceleration), so fast_t_us is that midpoint.
  if (!have_edge || age > FAST_TIMEOUT_US) {
    eng.moving = false;
    eng.fast_kmh = 0.0f;
    eng.fast_t_us = now_us;
  } else if (!was_moving) {
    if (dn > 0) {
      // First pulse(s) after standstill: a new motion starts at the first edge.
      eng.moving = true;
      const bool iv_ok = dn >= 2 && s.interval_us > 0 && s.interval_us <= (uint32_t)FAST_TIMEOUT_US;
      eng.motion_start_us = s.last_edge_us - (iv_ok ? (int64_t)(dn - 1) * s.interval_us : 0);
      eng.fast_kmh = iv_ok ? K / (float)s.interval_us : 0.0f;
      eng.fast_t_us = iv_ok ? s.last_edge_us - (int64_t)(s.interval_us / 2) : s.last_edge_us;
      if (iv_ok) oemPush(s.interval_us, dn - 1);
    }
  } else if (dn > 0) {
    const int64_t dt = s.last_edge_us - eng.ref_edge_us;
    if (dt > 0) {
      eng.fast_kmh = (float)dn * K / (float)dt;
      eng.fast_t_us = eng.ref_edge_us + dt / 2;
      oemPush((uint32_t)(dt / dn), dn);  // one entry per pulse (sum is exact)
    }
  } else if (age > 0) {
    const float bound = K / (float)age;  // speed if a pulse arrived right now
    if (bound < eng.fast_kmh) {
      eng.fast_kmh = bound;
      eng.fast_t_us = now_us;
    }
  }
  eng.ref_count = s.count;
  eng.ref_edge_us = s.last_edge_us;

  // OEM smooth: average of the last 12 per-pulse intervals, pushed once per
  // real pulse, cleared at standstill so the next launch starts clean.
  if (!have_edge || age > OEM_TIMEOUT_US) {
    eng.oem_n = 0;
    eng.oem_idx = 0;
    eng.oem_kmh = 0.0f;
  } else if (eng.oem_n == 0) {
    eng.oem_kmh = 0.0f;
  } else {
    float sum = 0.0f;
    for (uint8_t i = 0; i < eng.oem_n; i++) sum += (float)eng.oem_hist[i];
    float v = K / (sum / eng.oem_n);
    if (age > OEM_DECAY_START_US) {
      float d = (float)(age - OEM_DECAY_START_US) / 1000000.0f;
      v *= (1.0f - constrain(d, 0.0f, 1.0f));
    }
    eng.oem_kmh = v;
  }
}

// ================================================================================
// 5. PERSISTENCE (NVS)
// ================================================================================

static void collectPersistState(PersistState &p) {
  portENTER_CRITICAL(&g_tel_mux);
  p.odo = g_odo_km;
  p.trip = g_trip_km;
  portEXIT_CRITICAL(&g_tel_mux);
  p.ride = ride_seconds;
  p.bright = screen_brightness;
  p.cal = speed_cal;
  p.oem = speed_filter_oem;
  p.skin = (int)current_skin;
  p.imperial = imperial_mode;
  p.light = theme_light;
  p.best_m = best_0_50_time;
  p.best_i = best_0_30mph_time;
  p.lang = (int)current_lang;
}

static void loadSettings() {
  PersistState p;
  preferences.begin(NVS_NS, true);
  p.bright = preferences.getInt("bright", 255);
  p.cal = preferences.getFloat("speed_cal", DEFAULT_SPEED_CAL);
  p.oem = preferences.getBool("filter_oem", false);
  p.odo = preferences.getDouble("odo", 0.0);
  p.trip = preferences.getDouble("trip", 0.0);
  p.ride = preferences.getULong("ride_sec", 0);
  p.skin = preferences.getInt("skin", 0);
  p.imperial = preferences.getBool("imperial", false);
  p.light = preferences.getBool("theme_light", false);
  p.best_m = preferences.getFloat("best_0_50", 0.0f);
  p.best_i = preferences.getFloat("best_0_30i", 0.0f);
  p.lang = preferences.getInt("lang", LANG_EN);
  preferences.end();

  // Validate everything (NaN-safe: constrain() lets NaN through)
  p.bright = constrain(p.bright, 20, 255);
  if (!(p.cal >= 0.50f && p.cal <= 2.00f)) p.cal = DEFAULT_SPEED_CAL;
  p.cal = roundf(p.cal * 100.0f) / 100.0f;
  if (!(p.odo >= 0.0 && p.odo < 1.0e7)) p.odo = 0.0;
  if (!(p.trip >= 0.0 && p.trip < 1.0e7)) p.trip = 0.0;
  if (p.skin < 0 || p.skin > 3) p.skin = ui::SKIN_HALO;
  // 0 = no record; anything faster than RACE_MIN_PLAUSIBLE_S is sensor noise
  if (!(p.best_m >= RACE_MIN_PLAUSIBLE_S && p.best_m < 600.0f)) p.best_m = 0.0f;
  if (!(p.best_i >= RACE_MIN_PLAUSIBLE_S && p.best_i < 600.0f)) p.best_i = 0.0f;
  if (p.lang != LANG_EN && p.lang != LANG_HU) p.lang = LANG_EN;

  screen_brightness = p.bright;
  speed_cal = p.cal;
  speed_filter_oem = p.oem;
  portENTER_CRITICAL(&g_tel_mux);
  g_odo_km = p.odo;
  g_trip_km = p.trip;
  portEXIT_CRITICAL(&g_tel_mux);
  ride_seconds = p.ride;
  current_skin = (ui::Skin)p.skin;
  imperial_mode = p.imperial;
  theme_light = p.light;
  best_0_50_time = p.best_m;
  best_0_30mph_time = p.best_i;
  current_lang = (Language)p.lang;

  g_nvs = p;
  g_nvs_valid = true;

  Serial.printf("[NVS] Loaded: Skin=%d Bright=%d Cal=%.2f OEM=%d ODO=%.2f Lang=%d\n",
                p.skin, p.bright, p.cal, (int)p.oem, p.odo, p.lang);
}

// Writes only keys whose value changed since the last save (keeps NVS wear
// low: an odometer save is ~3 entries). Safe from any task.
static bool saveAll(const char *reason) {
  PersistState cur;
  collectPersistState(cur);
  if (g_nvs_mutex == NULL || xSemaphoreTake(g_nvs_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    Serial.println("[NVS] save skipped: mutex timeout");
    return false;
  }
  int writes = 0;
  bool ok = preferences.begin(NVS_NS, false);
  if (ok) {
    const bool all = !g_nvs_valid;
    if (all || cur.bright != g_nvs.bright)     { preferences.putInt("bright", cur.bright); writes++; }
    if (all || cur.cal != g_nvs.cal)           { preferences.putFloat("speed_cal", cur.cal); writes++; }
    if (all || cur.oem != g_nvs.oem)           { preferences.putBool("filter_oem", cur.oem); writes++; }
    if (all || cur.odo != g_nvs.odo)           { preferences.putDouble("odo", cur.odo); writes++; }
    if (all || cur.trip != g_nvs.trip)         { preferences.putDouble("trip", cur.trip); writes++; }
    if (all || cur.ride != g_nvs.ride)         { preferences.putULong("ride_sec", cur.ride); writes++; }
    if (all || cur.skin != g_nvs.skin)         { preferences.putInt("skin", cur.skin); writes++; }
    if (all || cur.imperial != g_nvs.imperial) { preferences.putBool("imperial", cur.imperial); writes++; }
    if (all || cur.light != g_nvs.light)       { preferences.putBool("theme_light", cur.light); writes++; }
    if (all || cur.best_m != g_nvs.best_m)     { preferences.putFloat("best_0_50", cur.best_m); writes++; }
    if (all || cur.best_i != g_nvs.best_i)     { preferences.putFloat("best_0_30i", cur.best_i); writes++; }
    if (all || cur.lang != g_nvs.lang)         { preferences.putInt("lang", cur.lang); writes++; }
    preferences.end();
    g_nvs = cur;
    g_nvs_valid = true;
  }
  xSemaphoreGive(g_nvs_mutex);
  if (!ok) Serial.println("[NVS] save failed: cannot open namespace");
  else if (writes) Serial.printf("[NVS] %d key(s) saved (%s)\n", writes, reason);
  return ok;
}

static void markSettingsDirty() {
  settings_dirty = true;
  last_settings_change_ms = g_frame_ms;
}

static double telOdo() {
  portENTER_CRITICAL(&g_tel_mux);
  double v = g_odo_km;
  portEXIT_CRITICAL(&g_tel_mux);
  return v;
}

// Odometer persistence. The bike is switched off by cutting power, so besides
// the periodic save we also save as soon as the bike comes to a stop.
static void persistenceUpdate(uint32_t now_ms, bool ota_busy) {
  static bool init = false;
  static uint32_t last_save_ms = 0;
  static double last_saved_odo = 0.0;
  static bool was_moving = false;
  static uint32_t stopped_since = 0;

  const double odo = telOdo();
  if (!init) {
    init = true;
    last_save_ms = now_ms;
    last_saved_odo = odo;
  }

  const char *why = NULL;
  if (save_requested) why = "settings";
  if (settings_dirty && now_ms - last_settings_change_ms > 2000) why = "settings";

  if (eng.moving) {
    was_moving = true;
    stopped_since = 0;
  } else if (was_moving) {
    if (stopped_since == 0) stopped_since = now_ms | 1;
    else if (now_ms - stopped_since >= 1500) {
      was_moving = false;
      if (odo != last_saved_odo) why = "stopped";
    }
  }
  if (dist_unsaved_km >= 0.5) why = "0.5 km";  // driven distance, not manual odo edits
  if (now_ms - last_save_ms >= 60000 && odo != last_saved_odo) why = "60 s";

  // Keep the flash quiet while the web updater is writing the new image;
  // appPrepareForRestart() persists everything right before the reboot.
  if (why && !(ota_busy && !save_requested)) {
    save_requested = false;
    settings_dirty = false;
    last_save_ms = now_ms;
    last_saved_odo = odo;
    dist_unsaved_km = 0.0;
    saveAll(why);
  }
}

// ================================================================================
// 6. APP BRIDGE (called from the web server task)
// ================================================================================

void appGetSnapshot(DashSnapshot &out) {
  portENTER_CRITICAL(&g_tel_mux);
  out.odo_km = g_odo_km;
  out.trip_km = g_trip_km;
  out.speed_kmh = bike_speed_kmh;
  out.max_speed_kmh = session_max_speed;
  out.ride_seconds = ride_seconds;
  out.best_0_50_s = best_0_50_time;  // metric 0-50 km/h record
  out.imperial = imperial_mode;
  out.speed_cal = speed_cal;
  out.filter_oem = speed_filter_oem;
  out.brightness = screen_brightness;
  portEXIT_CRITICAL(&g_tel_mux);
  out.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
}

bool appIsSafeToUpdate() {
  return bike_speed_kmh < 1.0f && raw_speed_kmh < 1.0f;
}

void appPrepareForRestart() {
  saveAll("restart");
}

// Acceleration = least-squares slope of speed over the last ACC_WINDOW_US.
// A regression over ~10 samples is far less noisy than a two-point difference.
#define ACC_SAMPLES 24
static const int64_t ACC_WINDOW_US = 300000;
static const int64_t ACC_MIN_SPAN_US = 90000;
static AccSample acc_buf[ACC_SAMPLES];
static uint8_t acc_n = 0;

static void accelReset() { acc_n = 0; }

static void accelPush(int64_t t_us, float v_kmh) {
  if (acc_n && t_us <= acc_buf[acc_n - 1].t_us) return;  // no new estimate this frame
  if (acc_n == ACC_SAMPLES) {
    memmove(acc_buf, acc_buf + 1, sizeof(AccSample) * (ACC_SAMPLES - 1));
    acc_n--;
  }
  acc_buf[acc_n].t_us = t_us;
  acc_buf[acc_n].v_kmh = v_kmh;
  acc_n++;
}

// Line fit v(t) = v_mean + mps2 * (t - t_mean) over the window. Returns false
// while there is too little data for a meaningful slope.
static bool accelFit(int64_t now_us, float &mps2, int64_t &t_mean_us, float &v_mean_kmh) {
  uint8_t first = 0;
  while (first < acc_n && now_us - acc_buf[first].t_us > ACC_WINDOW_US) first++;
  const int n = acc_n - first;
  if (n < 3) return false;
  const int64_t t0 = acc_buf[first].t_us;
  if (acc_buf[acc_n - 1].t_us - t0 < ACC_MIN_SPAN_US) return false;
  float st = 0, sv = 0, stt = 0, stv = 0;
  for (uint8_t i = first; i < acc_n; i++) {
    const float t = (float)(acc_buf[i].t_us - t0) * 1e-6f;  // s
    const float v = acc_buf[i].v_kmh / 3.6f;                 // m/s
    st += t; sv += v; stt += t * t; stv += t * v;
  }
  const float den = n * stt - st * st;
  if (den <= 0.0f) return false;
  mps2 = (n * stv - st * sv) / den;
  t_mean_us = t0 + (int64_t)(st / n * 1e6f);
  v_mean_kmh = sv / n * 3.6f;
  return true;
}

static bool accelSlope(int64_t now_us, float &mps2) {
  int64_t tm;
  float vm;
  return accelFit(now_us, mps2, tm, vm);
}

// ================================================================================
// 7. RACE TIMER
// ================================================================================

static void raceClearResults() {
  t_50_us = t_60_us = t_70_us = t_80_us = 0;
  current_0_50_time = 0.0f;
  split_50_60 = 0.0f;
  split_60_70 = 0.0f;
  split_70_80 = 0.0f;
  time_0_80 = 0.0f;
}

// Linear interpolation of the moment speed crossed `thr` between two samples.
static int64_t interpCross(float v0, int64_t t0, float v1, int64_t t1, float thr) {
  if (v1 <= v0 || t1 <= t0 || v0 >= thr) return t1;
  float f = (thr - v0) / (v1 - v0);
  return t0 + (int64_t)(f * (float)(t1 - t0));
}

// Crossing time from the line fitted over the last ACC_WINDOW_US (averages out
// per-frame speed noise); falls back to two-sample interpolation. The result
// is kept after `after_us` (the previous split) and near the triggering sample.
static int64_t crossTime(float v0, int64_t t0, float v1, int64_t t1, float thr,
                         int64_t now_us, int64_t after_us) {
  float a, vm;
  int64_t tm;
  if (accelFit(now_us, a, tm, vm) && a > 0.3f) {
    const int64_t tc = tm + (int64_t)((thr - vm) / 3.6f / a * 1e6f);
    if (tc > after_us && tc >= t1 - 250000 && tc <= t1 + 100000) return tc;
  }
  const int64_t ti = interpCross(v0, t0, v1, t1, thr);
  return ti > after_us ? ti : after_us + 1;
}

// v/t come from the unfiltered reciprocal estimator (t = pulse edge time), so
// timing is not delayed by the display filter or quantized to 33 ms frames.
static void raceTimerUpdate(float v, int64_t t_us, bool moving, int64_t start_us, int64_t now_us) {
  static float prev_v = 0.0f;
  static int64_t prev_t = 0;
  static float first_v = 0.0f;       // first speed sample of the run
  static int64_t first_t = 0;
  static bool start_fixed = false;

  if (!moving) {
    if (accel_timer_state != ACCEL_READY) {
      accel_timer_state = ACCEL_READY;
      current_0_50_time = 0.0f;
    }
    prev_v = 0.0f;
    prev_t = t_us;
    return;
  }

  if (accel_timer_state == ACCEL_READY) {
    if (v <= 0.8f) return;
    accel_timer_state = ACCEL_RUNNING;
    run_imperial = imperial_mode;
    t_launch_us = (start_us > 0 && start_us <= t_us) ? start_us : t_us;
    raceClearResults();
    prev_v = 0.0f;
    prev_t = t_launch_us;
    first_t = 0;
    start_fixed = false;
  }
  if (accel_timer_state != ACCEL_RUNNING) {
    prev_v = v;
    prev_t = t_us;
    return;
  }

  // The first pulse only arrives after the wheel has rolled one pulse
  // (2.4 cm): ~0.1 s after the real launch at 4 m/s2. Extrapolate the first two
  // speed samples back to 0 km/h to find the true start (standstill launches
  // only; bounded to what one pulse of travel can take).
  if (!start_fixed && t_us > prev_t && v > 0.0f) {
    if (first_t == 0) {
      first_v = v;
      first_t = t_us;
    } else {
      start_fixed = true;
      if (first_v < 8.0f && v > first_v && t_us > first_t) {
        const int64_t back = (int64_t)((float)(t_us - first_t) * first_v / (v - first_v));
        int64_t t0 = first_t - back;
        if (t0 < t_launch_us - 350000) t0 = t_launch_us - 350000;
        if (t0 < t_launch_us) t_launch_us = t0;
      }
    }
  }

  const float *thr = run_imperial ? RACE_THR_IMPERIAL : RACE_THR_METRIC;

  if (t_50_us == 0) {
    if (v >= thr[0]) {
      t_50_us = crossTime(prev_v, prev_t, v, t_us, thr[0], now_us, t_launch_us);
      float r = (float)(t_50_us - t_launch_us) / 1000000.0f;
      last_0_50_time = r;
      current_0_50_time = r;
      last_run_imperial = run_imperial;
      if (!demo_mode) {
        volatile float &best = run_imperial ? best_0_30mph_time : best_0_50_time;
        if (r >= RACE_MIN_PLAUSIBLE_S && (best <= 0.01f || r < best)) {
          best = r;
          save_requested = true;
        }
      }
    } else {
      current_0_50_time = (float)(now_us - t_launch_us) / 1000000.0f;
      if (now_us - t_launch_us > 30000000LL) {  // not a launch: stop timing
        accel_timer_state = ACCEL_WAIT_STOP;
        current_0_50_time = 0.0f;
      }
    }
  }
  if (accel_timer_state == ACCEL_RUNNING && t_50_us && !t_60_us && v >= thr[1]) {
    t_60_us = crossTime(prev_v, prev_t, v, t_us, thr[1], now_us, t_50_us);
    split_50_60 = (float)(t_60_us - t_50_us) / 1000000.0f;
  }
  if (accel_timer_state == ACCEL_RUNNING && t_60_us && !t_70_us && v >= thr[2]) {
    t_70_us = crossTime(prev_v, prev_t, v, t_us, thr[2], now_us, t_60_us);
    split_60_70 = (float)(t_70_us - t_60_us) / 1000000.0f;
  }
  if (accel_timer_state == ACCEL_RUNNING && t_70_us && !t_80_us && v >= thr[3]) {
    t_80_us = crossTime(prev_v, prev_t, v, t_us, thr[3], now_us, t_70_us);
    split_70_80 = (float)(t_80_us - t_70_us) / 1000000.0f;
    time_0_80 = (float)(t_80_us - t_launch_us) / 1000000.0f;
    accel_timer_state = ACCEL_FINISHED;
  }
  prev_v = v;
  prev_t = t_us;
}

static void raceManualReset() {
  raceClearResults();
  accel_timer_state = (raw_speed_kmh > 0.3f || eng.moving) ? ACCEL_WAIT_STOP : ACCEL_READY;
}

// ================================================================================
// 8. TELEMETRY UPDATE (once per frame)
// ================================================================================

static void demoStep(uint32_t dt_ms, int64_t now_us) {
  static int st = 3;
  static uint32_t since = 0;
  const float dt = dt_ms / 1000.0f;
  switch (st) {
    case 0:
      demo_speed += 45.0f * dt;
      if (demo_speed >= 75.0f) { demo_speed = 75.0f; st = 1; since = g_frame_ms; }
      break;
    case 1:
      if (g_frame_ms - since > 3000) st = 2;
      break;
    case 2:
      demo_speed -= 75.0f * dt;
      if (demo_speed <= 0.0f) { demo_speed = 0.0f; st = 3; since = g_frame_ms; }
      break;
    default:
      if (g_frame_ms - since > 1500) { st = 0; demo_start_us = now_us; }
      break;
  }
}

static void updateTelemetry(uint32_t dt_ms) {
  const PulseSnapshot s = readPulseSnapshot();
  const int64_t now_us = esp_timer_get_time();  // after the snapshot (see engine)
  const float cal = speed_cal;
  speedEngineUpdate(s, now_us, cal);

  // Distance from the exact pulse count (independent of any filtering)
  static uint32_t dist_count = 0;
  const uint32_t dn = s.count - dist_count;
  dist_count = s.count;
  if (dn) {
    const double d = (double)dn * (double)METERS_PER_SPEED_PULSE * (double)cal / 1000.0;
    portENTER_CRITICAL(&g_tel_mux);
    g_odo_km += d;
    g_trip_km += d;
    portEXIT_CRITICAL(&g_tel_mux);
    dist_unsaved_km += d;
  }

  float disp_raw, race_v;
  int64_t race_t, race_start;
  bool race_moving;
  if (demo_mode) {
    demoStep(dt_ms, now_us);
    disp_raw = race_v = demo_speed;
    race_t = now_us;
    race_moving = demo_speed > 0.0f;
    race_start = demo_start_us;
  } else {
    disp_raw = speed_filter_oem ? eng.oem_kmh : eng.fast_kmh;
    race_v = eng.fast_kmh;
    race_t = eng.fast_t_us;
    race_moving = eng.moving;
    race_start = eng.motion_start_us;
  }
  raw_speed_kmh = race_v;

  // Display filter
  static float filtered = 0.0f;
  if (demo_mode) {
    filtered = disp_raw;
  } else if (speed_filter_oem) {
    filtered = 0.12f * disp_raw + 0.88f * filtered;  // heavy OEM-like damping
    if (filtered < 0.3f && disp_raw == 0.0f) filtered = 0.0f;
  } else if (disp_raw <= 0.2f) {
    filtered = 0.0f;  // only after a real FAST_TIMEOUT_US standstill
  } else {
    float diff = fabsf(disp_raw - filtered);
    float alpha = (diff > 4.0f) ? 0.75f : (diff > 1.5f) ? 0.50f : 0.35f;
    filtered = alpha * disp_raw + (1.0f - alpha) * filtered;
  }
  bike_speed_kmh = filtered;

  // Acceleration / G-force (forward pull only), 50 ms cadence.
  // Uses the unfiltered pulse speed with the time each estimate refers to (the
  // display filter would add lag, and much more in OEM mode). The launch is
  // anchored at 0 km/h at the first pulse, so the first reading after
  // standstill is a real pull, not a jump from 0.
  static bool acc_moving = false;
  static uint32_t last_accel_ms = 0;
  static float smoothed_mps2 = 0.0f;
  const float acc_v = demo_mode ? demo_speed : eng.fast_kmh;
  const int64_t acc_t = demo_mode ? now_us : eng.fast_t_us;
  if (race_moving && !acc_moving) {
    accelReset();
    accelPush((race_start > 0 && race_start <= acc_t) ? race_start : acc_t, 0.0f);
  }
  acc_moving = race_moving;
  if (race_moving) accelPush(acc_t, acc_v);
  else accelReset();

  if (g_frame_ms - last_accel_ms >= 50) {
    last_accel_ms = g_frame_ms;
    float slope = 0.0f;
    if (!race_moving || !accelSlope(now_us, slope)) slope = 0.0f;
    // Smooth the SIGNED value and clamp only for display: clamping first would
    // turn speed noise at a steady speed into a constant fake pull.
    smoothed_mps2 = 0.30f * slope + 0.70f * smoothed_mps2;
    const float pull = smoothed_mps2 > 0.0f ? smoothed_mps2 : 0.0f;
    current_accel_g = pull / 9.80665f;
    current_accel_pct = (int)lroundf(constrain(pull / 4.5f, 0.0f, 1.0f) * 100.0f);  // 4.5 m/s2 (0.46 G) = 100%
  }

  if (filtered > session_max_speed) session_max_speed = filtered;

  // Debug console fake speed / G (rendering tests): display values only.
  // Odometer, ride time, max speed and the race timer use the real sensor
  // values computed above/below, and nothing here is persisted.
  float dbg_v;
  if (dbg::speedOverride(dbg_v)) bike_speed_kmh = dbg_v;
  if (dbg::accelOverride(dbg_v)) {
    current_accel_g = dbg_v > 0.0f ? dbg_v : 0.0f;
    current_accel_pct = (int)lroundf(constrain(current_accel_g * 9.80665f / 4.5f, 0.0f, 1.0f) * 100.0f);
  }

  // Ride time: accumulate real frame time (no lost fractions)
  static uint32_t ride_ms_acc = 0;
  if (!demo_mode && filtered > 1.0f) {
    ride_ms_acc += (dt_ms > 250) ? 250 : dt_ms;
    if (ride_ms_acc >= 1000) {
      ride_seconds = ride_seconds + ride_ms_acc / 1000;
      ride_ms_acc %= 1000;
    }
  }

  raceTimerUpdate(race_v, race_t, race_moving, race_start, now_us);
}

// ================================================================================
// 9. TOUCH: GESTURES (targets and hit testing live in ui_screens.cpp)
// ================================================================================

using namespace ui;  // Target names (TGT_*), View

static uint8_t touchKind(uint8_t t) {
  switch (t) {
    case TGT_NONE: return TK_NONE;
    case TGT_BRIGHT: return TK_DRAG;
    case TGT_CAL_MINUS: case TGT_CAL_PLUS: case TGT_ODO_MINUS: case TGT_ODO_PLUS: return TK_REPEAT;
    case TGT_TRIP_RESET: case TGT_MAX_RESET: case TGT_ODO_RESET: case TGT_DEMO: return TK_LONG;
    default: return TK_TAP;
  }
}

static uint16_t longPressMs(uint8_t t) {
  switch (t) {
    case TGT_TRIP_RESET: return 1000;
    case TGT_MAX_RESET:  return 1000;
    case TGT_ODO_RESET:  return 2000;
    case TGT_DEMO:       return 3000;
    default: return 0;
  }
}

// Finger is down on `tgt` (and still inside it): pressed look.
static bool uiPressed(uint8_t tgt) {
  if (!T.down || T.target != tgt || T.cancelled || T.sliding) return false;
  Rect r;
  return targetRect(g_view, tgt, r) && r.contains(T.x, T.y);
}

// Hold-to-reset progress 0..1 while the finger holds `tgt`, -1 otherwise.
static float uiHold(uint8_t tgt) {
  if (!T.down || T.target != tgt || T.cancelled || T.fired) return -1.0f;
  const uint16_t d = longPressMs(tgt);
  if (!d) return -1.0f;
  const float p = (float)(g_frame_ms - T.t0) / (float)d;
  return p > 1.0f ? 1.0f : (p < 0.0f ? 0.0f : p);
}

static void applyParamAdjust(uint8_t tgt) {
  if (tgt == TGT_CAL_MINUS || tgt == TGT_CAL_PLUS) {
    const int dir = (tgt == TGT_CAL_PLUS) ? 1 : -1;
    float v = speed_cal + dir * 0.01f;
    v = roundf(v * 100.0f) / 100.0f;  // no float drift (0.99999x)
    speed_cal = constrain(v, 0.50f, 2.00f);
  } else if (tgt == TGT_ODO_MINUS || tgt == TGT_ODO_PLUS) {
    // 10 units of the current system (10 km or 10 mi)
    const double step = (tgt == TGT_ODO_PLUS ? 1.0 : -1.0) * (imperial_mode ? 10.0 / KMH_TO_MPH : 10.0);
    portENTER_CRITICAL(&g_tel_mux);
    g_odo_km += step;
    if (g_odo_km < 0.0) g_odo_km = 0.0;
    portEXIT_CRITICAL(&g_tel_mux);
  }
  markSettingsDirty();
}

static uint8_t pageOf(ScreenState s) {
  return s == SCREEN_SETTINGS ? PAGE_SETTINGS : (s == SCREEN_RACE ? PAGE_RACE : PAGE_DASH);
}

static void navigate(int dir) {
  static const ScreenState order[3] = {SCREEN_SETTINGS, SCREEN_DASHBOARD, SCREEN_RACE};
  int idx = pageOf(current_screen) + dir;
  if (idx < 0) idx = 0;
  if (idx > 2) idx = 2;
  current_screen = order[idx];
}

static void resetSessionMax() {
  session_max_speed = 0.0f;
  g_peak_g = 0.0f;
}

static void fireTap(uint8_t t) {
  if (t >= TGT_TAB0 && t <= TGT_TAB5) { active_submenu = (SettingsSubmenu)(t - TGT_TAB0); return; }
  if (t >= TGT_SKIN0 && t <= TGT_SKIN3) { current_skin = (ui::Skin)(t - TGT_SKIN0); save_requested = true; return; }
  switch (t) {
    case TGT_UNITS_KMH:   imperial_mode = false; save_requested = true; break;
    case TGT_UNITS_MPH:   imperial_mode = true; save_requested = true; break;
    case TGT_THEME_DARK:  theme_light = false; save_requested = true; break;
    case TGT_THEME_LIGHT: theme_light = true; save_requested = true; break;
    case TGT_FILTER_FAST: speed_filter_oem = false; save_requested = true; break;
    case TGT_FILTER_OEM:  speed_filter_oem = true; save_requested = true; break;
    case TGT_LANG_EN:     current_lang = LANG_EN; save_requested = true; break;
    case TGT_LANG_HU:     current_lang = LANG_HU; save_requested = true; break;
    case TGT_WIFI_TOGGLE:
      if (webOtaIsOn()) webOtaEnd();
      else webOtaBegin();
      break;
    case TGT_RACE_RESET:  raceManualReset(); break;
    case TGT_OTA_CLOSE:   // failed update: show the hotspot status (still on) in Settings > WiFi
      g_ota_err_overlay = false;
      g_mock.ota = OTA_NONE;
      current_screen = SCREEN_SETTINGS;
      active_submenu = SUB_WIFI;
      break;
    default: break;
  }
}

static void fireLong(uint8_t t) {
  switch (t) {
    case TGT_TRIP_RESET:
      portENTER_CRITICAL(&g_tel_mux);
      g_trip_km = 0.0;
      portEXIT_CRITICAL(&g_tel_mux);
      save_requested = true;
      g_trip_reset_ms = g_frame_ms | 1;
      break;
    case TGT_MAX_RESET:
      resetSessionMax();
      g_max_reset_ms = g_frame_ms | 1;
      break;
    case TGT_ODO_RESET:
      portENTER_CRITICAL(&g_tel_mux);
      g_odo_km = 0.0;
      portEXIT_CRITICAL(&g_tel_mux);
      save_requested = true;
      g_odo_reset_ms = g_frame_ms | 1;
      break;
    case TGT_DEMO:
      demo_mode = !demo_mode;
      demo_speed = 0.0f;
      resetSessionMax();
      raceManualReset();
      Serial.printf("[UI] Demo mode %s\n", demo_mode ? "ON" : "OFF");
      break;
    default: break;
  }
}

// Brightness slider: 8..100 % over x 158..422; stored as PWM 20..255 (NVS "bright").
static int brightPct(int pwm) { return constrain((int)lroundf(pwm / 2.55f), 8, 100); }

static void applyDrag(uint8_t t, int x) {
  if (t != TGT_BRIGHT) return;
  const int pct = constrain(8 + (int)lroundf(92.0f * (x - 158) / 264.0f), 8, 100);
  const int b = constrain((int)lroundf(pct * 2.55f), 20, 255);
  if (b != screen_brightness) {
    screen_brightness = b;
    analogWrite(GFX_BL, b);
    markSettingsDirty();
  }
}

// Gestures: tap (on release), hold-to-repeat, drag, long-press and page swipe
// (the page follows the finger; > 60 px or a fling > 0.35 px/ms changes page).
// Hit testing uses g_view, i.e. the layout that is on screen. `blocked`
// swallows all input (firmware update in progress).
static void handleTouch(bool blocked) {
  uint16_t tx = 0, ty = 0;
  bool touched;
  if (!dbg::touchOverride(tx, ty, touched)) touched = display.getTouchPoint(tx, ty);
  const uint32_t now = g_frame_ms;
  if (touched) {
    if (tx > gfx::W - 1) tx = gfx::W - 1;
    if (ty > gfx::H - 1) ty = gfx::H - 1;
  }

  if (blocked) {
    if (touched) {
      if (!T.down) { memset(&T, 0, sizeof(T)); T.down = true; }
      T.target = TGT_NONE;
      T.cancelled = true;
      T.no_swipe = true;  // x0/y0 are stale here: no swipe once unblocked
    } else {
      T.down = false;
    }
    return;
  }

  if (touched) {
    if (!T.down) {
      memset(&T, 0, sizeof(T));
      T.down = true;
      T.x0 = T.x = tx;
      T.y0 = T.y = ty;
      T.t0 = T.last_ms = now;
      T.last_repeat = now;
      T.target = hitTest(g_view, tx, ty);
      T.kind = touchKind(T.target);
      T.no_swipe = T.kind == TK_DRAG || T.kind == TK_REPEAT || g_view.ota_view != OTA_NONE;
      if (T.kind == TK_REPEAT) applyParamAdjust(T.target);
      if (T.kind == TK_DRAG) applyDrag(T.target, tx);
      return;
    }
    // finger velocity for flings (smoothed over a few frames)
    const uint32_t fdt = now - T.last_ms;
    if (fdt > 0) T.vx = 0.5f * T.vx + 0.5f * (float)((int)tx - T.x) / (float)fdt;
    T.last_ms = now;
    T.x = tx;
    T.y = ty;
    const int dx = (int)T.x - T.x0, dy = (int)T.y - T.y0;

    if (T.kind == TK_DRAG) { applyDrag(T.target, tx); return; }
    if (T.kind == TK_REPEAT) {
      Rect r;
      if (!(targetRect(g_view, T.target, r) && r.contains(tx, ty))) return;  // slid off: pause
      // First repeat after 400 ms. Calibration: 12/s, 40/s after 2 s. Odometer: 8/s.
      const uint32_t held = now - T.t0;
      if (held >= 400) {
        const bool cal = T.target == TGT_CAL_MINUS || T.target == TGT_CAL_PLUS;
        const uint32_t interval = cal ? (held >= 2000 ? 25 : 83) : 125;
        if (now - T.last_repeat >= interval) {
          T.last_repeat = now;
          applyParamAdjust(T.target);
        }
      }
      return;
    }
    if (!T.no_swipe && !T.sliding && !T.fired && abs(dx) >= 16 && abs(dx) > abs(dy)) {
      T.sliding = true;   // horizontal drag: the page follows the finger
      T.cancelled = true;
    }
    if (T.kind == TK_LONG && !T.cancelled && !T.fired) {
      if (abs(dx) > 25 || abs(dy) > 25) T.cancelled = true;
      else if (now - T.t0 >= longPressMs(T.target)) {
        T.fired = true;
        fireLong(T.target);
      }
    }
    return;
  }

  if (!T.down) return;
  // Released
  T.down = false;
  if (T.sliding) {
    T.sliding = false;
    const int dx = (int)T.x - T.x0;
    const bool fling = fabsf(T.vx) > 0.35f && (T.vx < 0) == (dx < 0);
    if (abs(dx) > 60 || fling) navigate(dx < 0 ? +1 : -1);  // swipe left -> next page
    return;
  }
  if (T.cancelled || T.fired) return;
  if (T.kind == TK_DRAG) { markSettingsDirty(); return; }
  if (T.kind == TK_REPEAT) return;
  const int dx = (int)T.x - T.x0, dy = (int)T.y - T.y0;
  if (T.kind == TK_TAP && abs(dx) < 25 && abs(dy) < 25) {
    g_tap_target = T.target;  // 120 ms pressed flash
    g_tap_ms = now;
    fireTap(T.target);
  }
}

// ================================================================================
// 10. VIEW STATE (telemetry -> ui::View, once per frame)
// ================================================================================


// Deterministic launch trace of the mockups (gTrace), newest last.
static void mockGTrace(float *out, int n, float end) {
  for (int i = 0; i < n; i++) {
    const float t = (float)i / (n - 1);
    float v = 0.02f;
    if (t >= 0.18f) {
      const float s = sinf(fminf(1.0f, (t - 0.18f) / 0.5f) * (float)M_PI / 2);
      v = 0.02f + 0.5f * powf(s, 0.7f) * expf(-fmaxf(0.0f, t - 0.55f) * 3);
    }
    v += 0.025f * sinf(i * 1.9f) * (t > 0.2f ? 1.0f : 0.3f);
    out[i] = constrain(v, 0.0f, 0.6f);
  }
  out[n - 1] = end;
  out[n - 2] = (out[n - 3] + end) / 2;
}

// APEX telemetry: G history at 10 Hz and the session peak (reads current_accel_g only).
// A G faked from the debug console shows in the history but never sets the peak.
static void updateGTelemetry() {
  static uint32_t last = 0;
  const float g = constrain((float)current_accel_g, 0.0f, 0.6f);
  float injected;
  if (!dbg::accelOverride(injected) && g > g_peak_g) g_peak_g = g;
  if (g_frame_ms - last >= 100) {
    last = g_frame_ms;
    memmove(g_hist, g_hist + 1, sizeof(float) * (ui::G_HIST - 1));
    g_hist[ui::G_HIST - 1] = g;
  }
}

static void buildView(ui::View &v) {
  const bool imp = imperial_mode;
  const float k = imp ? KMH_TO_MPH : 1.0f;
  v.now_ms = g_frame_ms;
  v.page = pageOf(current_screen);
  v.dragging = T.down && T.sliding;
  if (v.dragging) {
    int dx = (int)T.x - T.x0;
    if ((v.page == PAGE_SETTINGS && dx > 0) || (v.page == PAGE_RACE && dx < 0)) dx = dx * 3 / 10;  // rubber band
    v.drag_px = (int16_t)dx;
  } else {
    v.drag_px = 0;
  }
  v.skin = (uint8_t)current_skin;
  v.section = (uint8_t)active_submenu;
  v.light = theme_light;
  v.hu = current_lang == LANG_HU;
  v.imperial = imp;
  v.demo = demo_mode;
  v.intro = g_intro_pending;
  g_intro_pending = false;

  double odo, trip;
  portENTER_CRITICAL(&g_tel_mux);
  odo = g_odo_km;
  trip = g_trip_km;
  portEXIT_CRITICAL(&g_tel_mux);
  v.speed = bike_speed_kmh * k;
  v.full_scale = imp ? GAUGE_MAX_MPH : GAUGE_MAX_KMH;
  v.gauge_tau_ms = speed_filter_oem ? 180.0f : 90.0f;
  v.max_speed = session_max_speed * k;
  v.ride_s = ride_seconds;
  v.trip = trip * k;
  v.odo = odo * k;
  v.g = constrain((float)current_accel_g, 0.0f, 0.6f);
  v.peak_g = g_peak_g;
  memcpy(v.g_hist, g_hist, sizeof(v.g_hist));

  // Race: results are shown only for the unit system they were measured in.
  const AccelTimerState rs = accel_timer_state;
  v.race_state = (uint8_t)rs;
  v.race_target = imp ? 30.0f : 50.0f;
  v.race_t = rs == ACCEL_FINISHED ? last_0_50_time : current_0_50_time;
  v.race_t_done = rs == ACCEL_RUNNING && t_50_us != 0;
  v.best = imp ? best_0_30mph_time : best_0_50_time;
  const bool last_ok = last_run_imperial == imp;
  v.last = last_ok ? last_0_50_time : 0.0f;
  const bool run_ok = run_imperial == imp;
  v.split[0] = run_ok ? split_50_60 : 0.0f;
  v.split[1] = run_ok ? split_60_70 : 0.0f;
  v.split[2] = run_ok ? split_70_80 : 0.0f;
  v.total = run_ok ? time_0_80 : 0.0f;
  v.new_best = !demo_mode && last_ok && v.best > 0.0f && fabsf(v.best - v.last) < 1e-6f &&
               (rs == ACCEL_FINISHED || (rs == ACCEL_RUNNING && t_50_us != 0));

  v.bright_pct = (uint8_t)brightPct(screen_brightness);
  v.cal = speed_cal;
  v.oem = speed_filter_oem;

  v.ota = g_ota;
  v.ota_view = g_ota.state == WEBOTA_UPLOADING ? OTA_PROGRESS
             : g_ota.state == WEBOTA_SUCCESS   ? OTA_SUCCESS
             : g_ota_err_overlay               ? OTA_ERROR
                                               : OTA_NONE;

  v.pressed = TGT_NONE;
  if (T.down) {
    if (uiPressed(T.target)) v.pressed = T.target;
  } else if (g_tap_target != TGT_NONE && g_frame_ms - g_tap_ms < 120) {
    v.pressed = g_tap_target;
  }
  v.hold_max = uiHold(TGT_MAX_RESET);
  v.hold_trip = uiHold(TGT_TRIP_RESET);
  v.hold_odo = uiHold(TGT_ODO_RESET);
  v.max_reset_ms = g_max_reset_ms;
  v.trip_reset_ms = g_trip_reset_ms;
  v.odo_reset_ms = g_odo_reset_ms;
  v.bright_drag = T.down && T.target == TGT_BRIGHT;

  // ---- debug console mock overrides (display only)
  if (g_mock.on) {
    v.max_speed = 74.0f * k;
    v.ride_s = 42 * 60 + 18;
    v.trip = 18.4;
    v.odo = 1284.6;
    v.peak_g = 0.52f;
    mockGTrace(v.g_hist, ui::G_HIST, v.g);
    v.bright_pct = 72;
    v.cal = 1.04f;
    v.oem = false;
  }
  if (g_mock.wifi && v.ota.state == WEBOTA_OFF) {
    v.ota.state = WEBOTA_READY;
    v.ota.clients = 1;
    strlcpy(v.ota.ssid, "LightBeeDisplay-1A2B", sizeof(v.ota.ssid));
    strlcpy(v.ota.password, "k7m2qx9vtp", sizeof(v.ota.password));
    strlcpy(v.ota.ip, "192.168.4.1", sizeof(v.ota.ip));
  }
  if (g_mock.demo) v.demo = true;
  if (g_mock.hold_max >= 0.0f) v.hold_max = g_mock.hold_max;
  if (g_mock.hold_trip >= 0.0f) v.hold_trip = g_mock.hold_trip;
  if (g_mock.hold_odo >= 0.0f) v.hold_odo = g_mock.hold_odo;
  if (g_mock.race != MR_OFF) {
    static const float SPLITS_A[4] = {0.88f, 1.05f, 1.41f, 6.92f};
    static const float SPLITS_B[4] = {0.84f, 1.02f, 1.37f, 6.65f};
    const float *sp = g_mock.race == MR_FINISHED ? SPLITS_B : SPLITS_A;
    v.race_state = g_mock.race == MR_PULLING ? RACE_RUNNING
                 : g_mock.race == MR_FINISHED ? RACE_FINISHED
                 : g_mock.race == MR_STOP     ? RACE_WAIT_STOP
                                              : RACE_READY;
    v.race_t = g_mock.race == MR_PULLING ? 2.87f : (g_mock.race == MR_FINISHED ? 3.42f : 0.0f);
    v.race_t_done = false;
    v.best = g_mock.race == MR_FINISHED ? 3.42f : 3.58f;
    v.last = g_mock.race == MR_FINISHED ? 3.42f : 3.71f;
    v.new_best = g_mock.race == MR_FINISHED;
    for (int i = 0; i < 3; i++) v.split[i] = g_mock.race == MR_PULLING ? 0.0f : sp[i];
    v.total = g_mock.race == MR_PULLING ? 0.0f : sp[3];
  }
  if (g_mock.ota != OTA_NONE) {
    v.ota_view = g_mock.ota;
    v.ota.progress_pct = 64;
    v.ota.bytes_written = 1268777;  // 1.21 MB
    v.ota.bytes_total = 1981809;    // 1.89 MB
    if (g_mock.ota == OTA_ERROR)
      strlcpy(v.ota.error, "Invalid image — use the plain .bin, not -full.bin", sizeof(v.ota.error));
  }
}

// ================================================================================
// 11. DEBUG CONSOLE (app commands, see debug_console.h)
// ================================================================================

// UI state commands. Changes are not saved here (they persist only if
// something else triggers a save). `mock ...` only changes what is displayed.
static dbg::AppResult appDebugCommand(const char *cmd, const char *arg, Print &out) {
  auto num = [&](long lo, long hi, long &v) {
    char *e;
    v = strtol(arg, &e, 10);
    return e != arg && *e == 0 && v >= lo && v <= hi;
  };
  long v;
  if (!strcmp(cmd, "screen")) {
    if (!strcmp(arg, "dash")) current_screen = SCREEN_DASHBOARD;
    else if (!strcmp(arg, "race")) current_screen = SCREEN_RACE;
    else if (!strcmp(arg, "settings")) current_screen = SCREEN_SETTINGS;
    else return dbg::APP_BAD_ARG;
  } else if (!strcmp(cmd, "tab")) {
    if (!num(0, SUB_COUNT - 1, v)) return dbg::APP_BAD_ARG;
    active_submenu = (SettingsSubmenu)v;
    current_screen = SCREEN_SETTINGS;
  } else if (!strcmp(cmd, "skin")) {
    if (!num(0, 3, v)) return dbg::APP_BAD_ARG;
    current_skin = (ui::Skin)v;
  } else if (!strcmp(cmd, "theme")) {
    if (!strcmp(arg, "dark")) theme_light = false;
    else if (!strcmp(arg, "light")) theme_light = true;
    else return dbg::APP_BAD_ARG;
  } else if (!strcmp(cmd, "lang")) {
    if (!strcmp(arg, "en")) current_lang = LANG_EN;
    else if (!strcmp(arg, "hu")) current_lang = LANG_HU;
    else return dbg::APP_BAD_ARG;
  } else if (!strcmp(cmd, "units")) {
    if (!strcmp(arg, "metric")) imperial_mode = false;
    else if (!strcmp(arg, "imperial")) imperial_mode = true;
    else return dbg::APP_BAD_ARG;
  } else if (!strcmp(cmd, "wifi")) {
    if (!strcmp(arg, "on")) { if (!webOtaIsOn()) webOtaBegin(); }
    else if (!strcmp(arg, "off")) { if (webOtaIsOn()) webOtaEnd(); }
    else return dbg::APP_BAD_ARG;
  } else if (!strcmp(cmd, "mock")) {
    // mock off | on | wifi | demo | race <ready|pulling|finished|stop|off> |
    //      ota <progress|success|error|off> | hold <max|trip|odo> <0..1|off> |
    //      boot <ms|off>
    char a0[12] = "", a1[12] = "", a2[12] = "";
    sscanf(arg, "%11s %11s %11s", a0, a1, a2);
    if (!strcmp(a0, "off")) {
      g_mock = {false, false, false, MR_OFF, OTA_NONE, -1.0f, -1.0f, -1.0f, -1};
    } else if (!strcmp(a0, "on")) {
      g_mock.on = true;
    } else if (!strcmp(a0, "wifi")) {
      g_mock.on = g_mock.wifi = true;
      g_mock.demo = false;
    } else if (!strcmp(a0, "demo")) {
      g_mock.on = g_mock.demo = true;
      g_mock.wifi = false;
    } else if (!strcmp(a0, "race")) {
      static const char *const n[] = {"off", "ready", "pulling", "finished", "stop"};
      int i = 0;
      while (i < 5 && strcmp(a1, n[i])) i++;
      if (i == 5) return dbg::APP_BAD_ARG;
      g_mock.race = (uint8_t)i;
    } else if (!strcmp(a0, "ota")) {
      static const char *const n[] = {"off", "progress", "success", "error"};
      int i = 0;
      while (i < 4 && strcmp(a1, n[i])) i++;
      if (i == 4) return dbg::APP_BAD_ARG;
      g_mock.ota = (uint8_t)i;
    } else if (!strcmp(a0, "boot")) {
      long ms = -1;
      if (strcmp(a1, "off") && (sscanf(a1, "%ld", &ms) != 1 || ms < 0 || ms > (long)ui::BOOT_MS))
        return dbg::APP_BAD_ARG;
      g_mock.boot = (int16_t)ms;
    } else if (!strcmp(a0, "hold")) {
      float p = -1.0f;
      if (strcmp(a2, "off") && sscanf(a2, "%f", &p) != 1) return dbg::APP_BAD_ARG;
      p = p < 0.0f ? -1.0f : constrain(p, 0.0f, 1.0f);
      if (!strcmp(a1, "max")) g_mock.hold_max = p;
      else if (!strcmp(a1, "trip")) g_mock.hold_trip = p;
      else if (!strcmp(a1, "odo")) g_mock.hold_odo = p;
      else return dbg::APP_BAD_ARG;
    } else {
      return dbg::APP_BAD_ARG;
    }
  } else if (!strcmp(cmd, "state")) {
    static const char *const scr[] = {"boot", "settings", "dash", "race"};
    out.printf("STATE screen=%s tab=%d skin=%d theme=%s lang=%s units=%s wifi=%s up=%lu\n",
               scr[current_screen & 3], (int)active_submenu, (int)current_skin,
               theme_light ? "light" : "dark", current_lang == LANG_HU ? "hu" : "en",
               imperial_mode ? "imperial" : "metric", webOtaIsOn() ? "on" : "off",
               (unsigned long)(millis() / 1000));
  } else {
    return dbg::APP_UNKNOWN;
  }
  return dbg::APP_OK;
}

// ================================================================================
// 12. UI & TELEMETRY TASK (core 1)
// ================================================================================

void UITask(void *pvParameters) {
  (void)pvParameters;
  Serial.println("[UI] Initializing touch LCD...");
  display_initialized = display.begin();
  if (display_initialized) {
    display.gfx->setRotation(1);  // landscape (gfx draws in landscape coordinates)
    gfx::begin(display.gfx->getFramebuffer());
    if (!lcd::begin(display)) Serial.println("[UI] fast flush unavailable, using library flush");
    ui::begin();
    Serial.println("[UI] Touch LCD initialized.");
  } else {
    Serial.println("[UI] Touch LCD init FAILED - running headless (telemetry only).");
  }

  pinMode(GFX_BL, OUTPUT);
  analogWrite(GFX_BL, screen_brightness);

  // Boot splash (~1.2 s, always dark), then the dashboard fades in with an
  // ignition sweep of the gauge.
  if (display_initialized) {
    const uint32_t t0 = millis();
    TickType_t wake = xTaskGetTickCount();
    for (uint32_t t = 0; t <= ui::BOOT_MS; t = millis() - t0) {
      ui::drawBoot(t);
      lcd::present();
      xTaskDelayUntil(&wake, pdMS_TO_TICKS(33));
    }
    g_intro_pending = true;
  }
  current_screen = SCREEN_DASHBOARD;
  dbg::begin(appDebugCommand, display_initialized ? display.gfx : nullptr);

  TickType_t last_wake = xTaskGetTickCount();
  uint32_t last_ms = millis();
  uint32_t last_diag_ms = last_ms;

  for (;;) {
    const uint32_t now_ms = millis();
    const uint32_t dt_ms = now_ms - last_ms;
    last_ms = now_ms;
    g_frame_ms = now_ms;

    dbg::poll();  // debug console commands (before telemetry: speed override)
    updateTelemetry(dt_ms);
    updateGTelemetry();
    webOtaGetStatus(g_ota);
    const bool ota_busy = (g_ota.state == WEBOTA_UPLOADING || g_ota.state == WEBOTA_SUCCESS);
    static WebOtaState prev_ota_state = WEBOTA_OFF;
    if (g_ota.state == WEBOTA_ERROR && prev_ota_state != WEBOTA_ERROR) g_ota_err_overlay = true;  // failed upload
    if (g_ota.state == WEBOTA_OFF) g_ota_err_overlay = false;
    prev_ota_state = g_ota.state;

    if (display_initialized) {
      handleTouch(ota_busy || (g_mock.ota != OTA_NONE && g_mock.ota != OTA_ERROR));  // hit tests the shown layout
      buildView(g_view);
      const uint32_t t_draw = micros();
      if (!dbg::render()) {  // `gfxtest` card replaces the UI
        if (g_mock.boot >= 0) ui::drawBoot((uint32_t)g_mock.boot);
        else ui::frame(g_view);
      }
      const uint32_t t_flush = micros();
      dbg::frameRendered(t_flush - t_draw);  // serves `shot` before the flush
      lcd::present();  // async: core 0 sends this frame, we draw the next one
      dbg::frameFlushed(micros() - t_flush);
    }
    persistenceUpdate(now_ms, ota_busy);

    if (now_ms - last_diag_ms >= 30000) {
      last_diag_ms = now_ms;
      Serial.printf("[UI] stack free min %u B, heap %u B, psram %u B\n",
                    (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)ESP.getFreeHeap(),
                    (unsigned)ESP.getFreePsram());
    }

    // 30 fps normally; 10 fps while flashing to leave CPU for the updater.
    const TickType_t period = pdMS_TO_TICKS(ota_busy ? 100 : 33);
    if (xTaskDelayUntil(&last_wake, period) == pdFALSE) {
      vTaskDelay(1);  // frame overran: still yield, then resync
      last_wake = xTaskGetTickCount();
    }
  }
}

// ================================================================================
// 13. SETUP / LOOP
// ================================================================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("==================================================");
  Serial.printf("   %s  v%s  (%s)\n", FW_NAME, FW_VERSION, FW_BUILD_DATE);
  Serial.println("==================================================");

  g_nvs_mutex = xSemaphoreCreateMutex();
  memset(&eng, 0, sizeof(eng));
  memset(&T, 0, sizeof(T));
  memset(&g_ota, 0, sizeof(g_ota));
  memset(&g_view, 0, sizeof(g_view));
  g_view.page = PAGE_DASH;
  loadSettings();

  // Speed sensor ISR (attached from this task -> serviced on core 1)
  pinMode(PIN_SPEED_SENSOR, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_SPEED_SENSOR), speedPulseISR, RISING);
  Serial.println("[Speed] Speed sensor ISR attached on GPIO 17.");

  // Optional SD card
  sdSPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  if (SD.begin(SD_CS, sdSPI, 20000000)) {
    Serial.println("[+] SD Card initialized.");
    sd_active = true;
  } else {
    Serial.println("[-] SD Card not detected (optional).");
    sd_active = false;
  }

  // WiFi / update hotspot stays OFF until enabled from Settings > WIFI.

  // UI + telemetry on the APP core (core 1); WiFi/lwIP live on core 0.
  xTaskCreatePinnedToCore(UITask, "UITask", 16384, NULL, 2, NULL, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
