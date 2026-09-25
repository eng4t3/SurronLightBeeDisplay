/*
  ================================================================================
  WHEELIE ASSIST / EMOTO-DASH-PRO: Sur-Ron speed & acceleration telemetry cluster
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

  Drawing: everything goes straight to the Arduino_Canvas framebuffer in
  rotation 1 (landscape) via ui_gfx.h, then display.flush() once per frame.
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

#include "ui_gfx.h"      // palette, primitives, fonts (incl. big_font.h)
#include "logo.h"
#include "version.h"
#include "web_ota.h"
#include "app_bridge.h"
#include <qrcode_helper.h>  // ricmoo QRCode (ships with the JC3248W535EN library)

// ================================================================================
// 1. PIN DEFINITIONS & CONSTANTS
// ================================================================================

#define PIN_SPEED_SENSOR 17  // Speed pulse signal (4-pin Extended IO P3/P4 Pin 3)

// Sur-Ron 2025 Light Bee X speed pulse calibration
const float WHEEL_CIRCUMFERENCE_M  = 1.88f;
const float PULSES_PER_WHEEL_REV   = 77.65f;
const float METERS_PER_SPEED_PULSE = WHEEL_CIRCUMFERENCE_M / PULSES_PER_WHEEL_REV;  // ~0.02421 m

const float KMH_TO_MPH = 0.621371f;

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

#define TR(en, hu) ((current_lang == LANG_HU) ? (hu) : (en))

// ================================================================================
// 2. TYPES (all declared before the first function so Arduino's generated
//    prototypes can reference them)
// ================================================================================

enum GaugeSkin : uint8_t {
  SKIN_PRO_ARC = 0,        // Segmented 26-LED arc
  SKIN_CYBER_HORIZON = 1,  // Continuous gradient halo
  SKIN_ANALOG_SPORT = 2,   // Sport needle dial
  SKIN_F1_RACE = 3         // F1 steering-wheel cockpit
};

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

enum TouchTarget : uint8_t {
  TGT_NONE = 0,
  TGT_TAB0, TGT_TAB1, TGT_TAB2, TGT_TAB3, TGT_TAB4, TGT_TAB5,
  TGT_BRIGHT, TGT_UNITS, TGT_THEME,
  TGT_SKIN0, TGT_SKIN1, TGT_SKIN2, TGT_SKIN3,
  TGT_CAL_MINUS, TGT_CAL_PLUS, TGT_FILTER_FAST, TGT_FILTER_OEM,
  TGT_ODO_MINUS, TGT_ODO_PLUS, TGT_ODO_RESET,
  TGT_LANG_EN, TGT_LANG_HU,
  TGT_WIFI_TOGGLE,
  TGT_TRIP_RESET, TGT_MAX_RESET,
  TGT_RACE_RESET,
  TGT_DEMO
};

struct TouchState {
  bool     down;
  int16_t  x0, y0, x, y;
  uint32_t t0;
  uint32_t last_repeat;
  uint8_t  target;
  uint8_t  kind;
  bool     no_swipe;
  bool     swiped;
  bool     cancelled;
  bool     fired;
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
volatile GaugeSkin current_skin = SKIN_PRO_ARC;
volatile int screen_brightness = 255;
volatile bool imperial_mode = false;
volatile bool theme_light = false;
volatile bool demo_mode = false;       // hidden: hold the SETTINGS title for 3 s
volatile float speed_cal = 1.00f;
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
static uint32_t trip_reset_flash_until = 0;
static uint32_t odo_reset_flash_until = 0;

// --- WiFi QR cache ---
static uint8_t qr_modules[512];
static QRCode qr;
static bool qr_ok = false;
static char qr_payload[240] = "";

// --- Layout (landscape px). Shared by drawing AND hit testing. ---
static const Rect R_TOPBAR_MAX  = {300, 0, 180, 36};
static const Rect R_TOPBAR_DEMO = {0, 0, 200, 36};
static const Rect R_TRIP        = {12, 270, 224, 44};
static const Rect R_ODOBAR      = {244, 270, 224, 44};

static const Rect R_RUN         = {12, 146, 224, 166};
static const Rect R_SPLITS      = {244, 146, 224, 166};

static const Rect R_TABBAR      = {8, 38, 464, 34};

static const Rect R_BRIGHT      = {12, 80, 456, 70};
static const Rect R_UNITS       = {12, 158, 456, 70};
static const Rect R_THEME       = {12, 236, 456, 70};

static const Rect R_CALCARD     = {12, 80, 456, 118};
static const Rect R_CAL_MINUS   = {24, 110, 92, 76};
static const Rect R_CAL_PLUS    = {364, 110, 92, 76};
static const Rect R_FILTERCARD  = {12, 206, 456, 104};
static const Rect R_FILTER_FAST = {24, 238, 208, 62};
static const Rect R_FILTER_OEM  = {248, 238, 208, 62};

static const Rect R_ODOCARD     = {12, 80, 456, 230};
static const Rect R_ODO_MINUS   = {24, 196, 150, 50};
static const Rect R_ODO_PLUS    = {306, 196, 150, 50};
static const Rect R_ODO_RESET   = {24, 256, 432, 44};

static const Rect R_LANG_EN     = {12, 80, 456, 110};
static const Rect R_LANG_HU     = {12, 198, 456, 110};

static const Rect R_WIFI_TOGGLE = {12, 80, 456, 62};
static const Rect R_WIFI_INFO   = {12, 150, 264, 160};
static const Rect R_WIFI_QR     = {284, 150, 184, 160};
static const Rect R_WIFI_HELP   = {12, 150, 456, 160};

// Gauge centre (dashboard)
static const int GCX = 240;
static const int GCY = 152;

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
  p.cal = preferences.getFloat("speed_cal", 1.00f);
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
  if (!(p.cal >= 0.50f && p.cal <= 2.00f)) p.cal = 1.00f;
  p.cal = roundf(p.cal * 100.0f) / 100.0f;
  if (!(p.odo >= 0.0 && p.odo < 1.0e7)) p.odo = 0.0;
  if (!(p.trip >= 0.0 && p.trip < 1.0e7)) p.trip = 0.0;
  if (p.skin < 0 || p.skin > 3) p.skin = SKIN_PRO_ARC;
  if (!(p.best_m >= 0.0f && p.best_m < 600.0f)) p.best_m = 0.0f;
  if (!(p.best_i >= 0.0f && p.best_i < 600.0f)) p.best_i = 0.0f;
  if (p.lang != LANG_EN && p.lang != LANG_HU) p.lang = LANG_EN;

  screen_brightness = p.bright;
  speed_cal = p.cal;
  speed_filter_oem = p.oem;
  portENTER_CRITICAL(&g_tel_mux);
  g_odo_km = p.odo;
  g_trip_km = p.trip;
  portEXIT_CRITICAL(&g_tel_mux);
  ride_seconds = p.ride;
  current_skin = (GaugeSkin)p.skin;
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
        if (best <= 0.01f || r < best) {
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
// 9. TOUCH: TARGETS, HIT TESTING, GESTURES
// ================================================================================

static const char *tabLabel(int i) {
  static const char *const en[SUB_COUNT] = {"SYSTEM", "SKIN", "SPEED", "ODO", "LANG", "WIFI"};
  static const char *const hu[SUB_COUNT] = {"RENDSZER", "SKIN", "SEBESS.", "ODO", "NYELV", "WIFI"};
  return (current_lang == LANG_HU) ? hu[i] : en[i];
}

// Tab widths follow their labels, so 6 tabs fit in both languages. Drawing and
// hit testing both use this, so they can never disagree.
static void tabLayout(int16_t *xs, int16_t *ws) {
  const int PAD = 3;
  int lw[SUB_COUNT];
  int sum = 0;
  for (int i = 0; i < SUB_COUNT; i++) {
    lw[i] = uiTextW(tabLabel(i), &FreeSansBold9pt7b);
    sum += lw[i];
  }
  int extra = (R_TABBAR.w - 2 * PAD - sum) / SUB_COUNT;
  if (extra < 4) extra = 4;
  int x = R_TABBAR.x + PAD;
  for (int i = 0; i < SUB_COUNT; i++) {
    xs[i] = x;
    ws[i] = lw[i] + extra;
    x += ws[i];
  }
  ws[SUB_COUNT - 1] = R_TABBAR.right() - PAD - xs[SUB_COUNT - 1];
}

static Rect tabHitRect(int i) {
  int16_t xs[SUB_COUNT], ws[SUB_COUNT];
  tabLayout(xs, ws);
  int x0 = (i == 0) ? 0 : xs[i];
  int x1 = (i == SUB_COUNT - 1) ? UI_W : xs[i] + ws[i];
  Rect r = {(int16_t)x0, 34, (int16_t)(x1 - x0), 42};
  return r;
}

static Rect skinRect(int i) {
  Rect r = {12, (int16_t)(80 + i * 58), 456, 52};
  return r;
}

static bool targetRect(uint8_t t, Rect &r) {
  if (t >= TGT_TAB0 && t <= TGT_TAB5) { r = tabHitRect(t - TGT_TAB0); return true; }
  if (t >= TGT_SKIN0 && t <= TGT_SKIN3) { r = skinRect(t - TGT_SKIN0); return true; }
  switch (t) {
    case TGT_BRIGHT:      r = R_BRIGHT; return true;
    case TGT_UNITS:       r = R_UNITS; return true;
    case TGT_THEME:       r = R_THEME; return true;
    case TGT_CAL_MINUS:   r = R_CAL_MINUS; return true;
    case TGT_CAL_PLUS:    r = R_CAL_PLUS; return true;
    case TGT_FILTER_FAST: r = R_FILTER_FAST; return true;
    case TGT_FILTER_OEM:  r = R_FILTER_OEM; return true;
    case TGT_ODO_MINUS:   r = R_ODO_MINUS; return true;
    case TGT_ODO_PLUS:    r = R_ODO_PLUS; return true;
    case TGT_ODO_RESET:   r = R_ODO_RESET; return true;
    case TGT_LANG_EN:     r = R_LANG_EN; return true;
    case TGT_LANG_HU:     r = R_LANG_HU; return true;
    case TGT_WIFI_TOGGLE: r = R_WIFI_TOGGLE; return true;
    case TGT_TRIP_RESET:  r = R_TRIP; return true;
    case TGT_MAX_RESET:   r = R_TOPBAR_MAX; return true;
    case TGT_RACE_RESET:  r = R_RUN; return true;
    case TGT_DEMO:        r = R_TOPBAR_DEMO; return true;
    default: return false;
  }
}

static uint8_t touchKind(uint8_t t) {
  switch (t) {
    case TGT_NONE: return TK_NONE;
    case TGT_BRIGHT: return TK_DRAG;
    case TGT_CAL_MINUS: case TGT_CAL_PLUS: case TGT_ODO_MINUS: case TGT_ODO_PLUS: return TK_REPEAT;
    case TGT_TRIP_RESET: case TGT_ODO_RESET: case TGT_DEMO: return TK_LONG;
    default: return TK_TAP;
  }
}

static uint16_t longPressMs(uint8_t t) {
  switch (t) {
    case TGT_TRIP_RESET: return 1000;
    case TGT_ODO_RESET:  return 2000;
    case TGT_DEMO:       return 3000;
    default: return 0;
  }
}

static uint8_t hitTest(int x, int y) {
  uint8_t list[12];
  int n = 0;
  if (current_screen == SCREEN_DASHBOARD) {
    list[n++] = TGT_TRIP_RESET;
    list[n++] = TGT_MAX_RESET;
  } else if (current_screen == SCREEN_RACE) {
    list[n++] = TGT_RACE_RESET;
  } else if (current_screen == SCREEN_SETTINGS) {
    list[n++] = TGT_DEMO;
    for (int i = 0; i < SUB_COUNT; i++) list[n++] = TGT_TAB0 + i;
    for (int i = 0; i < n; i++) {
      Rect r;
      if (targetRect(list[i], r) && r.contains(x, y)) return list[i];
    }
    n = 0;
    switch (active_submenu) {
      case SUB_SYSTEM:   list[n++] = TGT_BRIGHT; list[n++] = TGT_UNITS; list[n++] = TGT_THEME; break;
      case SUB_SKIN:     for (int i = 0; i < 4; i++) list[n++] = TGT_SKIN0 + i; break;
      case SUB_SPEED:    list[n++] = TGT_CAL_MINUS; list[n++] = TGT_CAL_PLUS;
                         list[n++] = TGT_FILTER_FAST; list[n++] = TGT_FILTER_OEM; break;
      case SUB_ODO:      list[n++] = TGT_ODO_MINUS; list[n++] = TGT_ODO_PLUS; list[n++] = TGT_ODO_RESET; break;
      case SUB_LANGUAGE: list[n++] = TGT_LANG_EN; list[n++] = TGT_LANG_HU; break;
      case SUB_WIFI:     list[n++] = TGT_WIFI_TOGGLE; break;
      default: break;
    }
  }
  for (int i = 0; i < n; i++) {
    Rect r;
    if (targetRect(list[i], r) && r.contains(x, y)) return list[i];
  }
  return TGT_NONE;
}

static bool uiPressed(uint8_t tgt) {
  if (!T.down || T.target != tgt || T.cancelled || T.swiped) return false;
  Rect r;
  return targetRect(tgt, r) && r.contains(T.x, T.y);
}

// 0..1 progress of a running long-press on `tgt` (for the fill animation).
static float uiHold(uint8_t tgt) {
  if (!T.down || T.target != tgt || T.cancelled || T.fired) return 0.0f;
  uint16_t d = longPressMs(tgt);
  if (!d) return 0.0f;
  float p = (float)(g_frame_ms - T.t0) / (float)d;
  return p > 1.0f ? 1.0f : (p < 0.0f ? 0.0f : p);
}

static void applyParamAdjust(uint8_t tgt, int level) {
  if (tgt == TGT_CAL_MINUS || tgt == TGT_CAL_PLUS) {
    int dir = (tgt == TGT_CAL_PLUS) ? 1 : -1;
    float step = (level >= 2) ? 0.02f : 0.01f;
    float v = speed_cal + dir * step;
    v = roundf(v * 100.0f) / 100.0f;  // no float drift (0.99999x)
    speed_cal = constrain(v, 0.50f, 2.00f);
  } else if (tgt == TGT_ODO_MINUS || tgt == TGT_ODO_PLUS) {
    int dir = (tgt == TGT_ODO_PLUS) ? 1 : -1;
    double step = (level >= 2) ? 50.0 : 10.0;
    portENTER_CRITICAL(&g_tel_mux);
    g_odo_km += dir * step;
    if (g_odo_km < 0.0) g_odo_km = 0.0;
    portEXIT_CRITICAL(&g_tel_mux);
  }
  markSettingsDirty();
}

static void navigate(int dir) {
  static const ScreenState order[3] = {SCREEN_SETTINGS, SCREEN_DASHBOARD, SCREEN_RACE};
  int idx = 1;
  for (int i = 0; i < 3; i++) if (order[i] == current_screen) idx = i;
  idx += dir;
  if (idx < 0) idx = 0;
  if (idx > 2) idx = 2;
  current_screen = order[idx];
}

static void fireTap(uint8_t t) {
  if (t >= TGT_TAB0 && t <= TGT_TAB5) { active_submenu = (SettingsSubmenu)(t - TGT_TAB0); return; }
  if (t >= TGT_SKIN0 && t <= TGT_SKIN3) { current_skin = (GaugeSkin)(t - TGT_SKIN0); save_requested = true; return; }
  switch (t) {
    case TGT_UNITS:       imperial_mode = !imperial_mode; save_requested = true; break;
    case TGT_THEME:       theme_light = !theme_light; save_requested = true; break;
    case TGT_FILTER_FAST: speed_filter_oem = false; save_requested = true; break;
    case TGT_FILTER_OEM:  speed_filter_oem = true; save_requested = true; break;
    case TGT_LANG_EN:     current_lang = LANG_EN; save_requested = true; break;
    case TGT_LANG_HU:     current_lang = LANG_HU; save_requested = true; break;
    case TGT_WIFI_TOGGLE:
      if (webOtaIsOn()) webOtaEnd();
      else webOtaBegin();
      break;
    case TGT_MAX_RESET:   session_max_speed = 0.0f; break;
    case TGT_RACE_RESET:  raceManualReset(); break;
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
      trip_reset_flash_until = (g_frame_ms + 1500) | 1;
      break;
    case TGT_ODO_RESET:
      portENTER_CRITICAL(&g_tel_mux);
      g_odo_km = 0.0;
      portEXIT_CRITICAL(&g_tel_mux);
      save_requested = true;
      odo_reset_flash_until = (g_frame_ms + 1500) | 1;
      break;
    case TGT_DEMO:
      demo_mode = !demo_mode;
      demo_speed = 0.0f;
      session_max_speed = 0.0f;
      raceManualReset();
      Serial.printf("[UI] Demo mode %s\n", demo_mode ? "ON" : "OFF");
      break;
    default: break;
  }
}

static void applyDrag(uint8_t t, int x) {
  if (t != TGT_BRIGHT) return;
  const int x0 = R_BRIGHT.x + 18, x1 = R_BRIGHT.right() - 18;
  x = constrain(x, x0, x1);
  int b = 20 + (int)lroundf((float)(x - x0) * (255 - 20) / (float)(x1 - x0));
  b = constrain(b, 20, 255);
  if (b != screen_brightness) {
    screen_brightness = b;
    analogWrite(GFX_BL, b);
    markSettingsDirty();
  }
}

// Gestures: tap (on release), hold-to-repeat, drag, long-press and swipe.
// `blocked` swallows all input (firmware update overlay).
static void handleTouch(bool blocked) {
  uint16_t tx = 0, ty = 0;
  const bool touched = display.getTouchPoint(tx, ty);
  const uint32_t now = g_frame_ms;
  if (touched) {
    if (tx > UI_W - 1) tx = UI_W - 1;
    if (ty > UI_H - 1) ty = UI_H - 1;
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
      T.t0 = now;
      T.last_repeat = now;
      T.target = hitTest(tx, ty);
      T.kind = touchKind(T.target);
      T.no_swipe = (current_screen == SCREEN_SETTINGS && ty < 76) || T.kind == TK_DRAG || T.kind == TK_REPEAT;
      if (T.kind == TK_REPEAT) applyParamAdjust(T.target, 0);
      if (T.kind == TK_DRAG) applyDrag(T.target, tx);
      return;
    }
    T.x = tx;
    T.y = ty;
    const int dx = (int)T.x - T.x0, dy = (int)T.y - T.y0;

    if (T.kind == TK_DRAG) { applyDrag(T.target, tx); return; }
    if (T.kind == TK_REPEAT) {
      Rect r;
      if (!(targetRect(T.target, r) && r.contains(tx, ty))) return;  // slid off: pause
      const uint32_t held = now - T.t0;
      if (held >= 350) {
        uint32_t interval = 140;
        int lvl = 0;
        if (held >= 2000) { interval = 40; lvl = 2; }
        else if (held >= 1000) { interval = 80; lvl = 1; }
        if (now - T.last_repeat >= interval) {
          T.last_repeat = now;
          applyParamAdjust(T.target, lvl);
        }
      }
      return;
    }
    if (T.kind == TK_LONG && !T.cancelled && !T.fired) {
      if (abs(dx) > 25 || abs(dy) > 25) T.cancelled = true;
      else if (now - T.t0 >= longPressMs(T.target)) {
        T.fired = true;
        fireLong(T.target);
      }
    }
    if (!T.no_swipe && !T.swiped && !T.fired && abs(dx) >= 60 && abs(dx) * 2 > abs(dy) * 3) {
      navigate(dx < 0 ? +1 : -1);  // swipe left -> next screen
      T.swiped = true;
      T.cancelled = true;
    }
    return;
  }

  if (!T.down) return;
  // Released
  T.down = false;
  if (T.swiped || T.cancelled || T.fired) return;
  if (T.kind == TK_DRAG) { markSettingsDirty(); return; }
  if (T.kind == TK_REPEAT) return;
  const int dx = (int)T.x - T.x0, dy = (int)T.y - T.y0;
  const uint32_t dur = now - T.t0;
  if (!T.no_swipe && abs(dx) >= 45 && abs(dx) * 5 > abs(dy) * 7 && dur < 600) {
    navigate(dx < 0 ? +1 : -1);  // quick flick
    return;
  }
  if (T.kind == TK_TAP && abs(dx) < 25 && abs(dy) < 25) fireTap(T.target);
}

// ================================================================================
// 10. RENDERING: SHARED PIECES
// ================================================================================

static void drawTopBar(const char *title, const GFXfont *tf, uint16_t tcol,
                       const char *right, uint16_t rcol, int page) {
  const int MID = 17;
  uiTextMid(title, 14, MID, tf, tcol, AL_LEFT);
  uiPageDots(240, MID, 3, page);

  const int rw = uiTextW(right, &FreeSansBold9pt7b);
  uiTextMid(right, 466, MID, &FreeSansBold9pt7b, rcol, AL_RIGHT);
  int x = 466 - rw - 12;  // right edge for status icons
  if (g_ota.state != WEBOTA_OFF) {
    uiWifiIcon(x - 11, MID + 7, g_ota.state == WEBOTA_ERROR ? P->red : P->accent);
    x -= 30;
  }
  if (demo_mode) {
    const int bw = uiTextW("DEMO", &FreeSansBold9pt7b) + 14;
    int bx = 14 + uiTextW(title, tf) + 10;       // prefer next to the title
    if (bx + bw > 222) bx = x - bw;               // ...unless it would hit the dots
    uiRRect(bx, MID - 10, bw, 20, 10, P->amber);
    uiTextMid("DEMO", bx + bw / 2, MID, &FreeSansBold9pt7b, P->on_accent, AL_CENTER);
  }
}

static uint16_t speedZoneColor(float pct) {
  if (pct < 0.60f) return P->accent;
  if (pct < 0.85f) return P->amber;
  return P->red;
}

static void drawBootScreen() {
  P = theme_light ? &PAL_LIGHT : &PAL_DARK;
  ui_g->fillScreen(C565(8, 12, 22));
  const int lx = (UI_W - LOGO_WIDTH) / 2;
  const int ly = (UI_H - LOGO_HEIGHT) / 2 - 10;
  for (int y = 0; y < LOGO_HEIGHT; y++) {
    for (int x = 0; x < LOGO_WIDTH; x++) {
      uint16_t c = pgm_read_word(&logo_bitmap[y * LOGO_WIDTH + x]);
      if (c != 0x0000) ui_g->drawPixel(lx + x, ly + y, c);
    }
  }
  char buf[40];
  snprintf(buf, sizeof(buf), "%s  v%s", FW_NAME, FW_VERSION);
  uiText(buf, UI_W / 2, ly + LOGO_HEIGHT + 18, &FreeSans9pt7b, C565(120, 134, 158), AL_CENTER);
  display.flush();
  delay(1200);
}

// ================================================================================
// 11. DASHBOARD
// ================================================================================

static void gaugeProArc(float pct, const char *spd, const char *unit) {
  const int N = 26;
  const float span = 270.0f / N;
  const int active = (int)lroundf(pct * N);
  for (int i = 0; i < N; i++) {
    const float a0 = 135.0f + i * span + 1.3f;
    const float a1 = a0 + span - 2.6f;
    uint16_t c = P->track;
    if (i < active) c = (i < N * 60 / 100) ? P->accent : (i < N * 85 / 100) ? P->amber : P->red;
    uiArc(GCX, GCY, 98, 116, a0, a1, c, 12.0f);
  }
  uiBigText(spd, GCX, 100, P->text, AL_CENTER);
  uiText(unit, GCX, 184, &FreeSansBold12pt7b, P->accent, AL_CENTER);
}

static void gaugeCyber(float pct, const char *spd, const char *unit) {
  const float A0 = 135.0f, SW = 270.0f;
  uiArcRound(GCX, GCY, 104, 114, A0, A0 + SW, P->track);

  const float lo[4] = {0.0f, 0.45f, 0.75f, 0.90f};
  const float hi[4] = {0.45f, 0.75f, 0.90f, 1.0f};
  const uint16_t col[4] = {P->accent, P->green, P->amber, P->red};
  uint16_t tip_col = P->accent;
  for (int b = 0; b < 4; b++) {
    const float s = A0 + lo[b] * SW;
    const float e = A0 + (pct < hi[b] ? pct : hi[b]) * SW;
    if (e > s) { uiArc(GCX, GCY, 104, 114, s, e, col[b]); tip_col = col[b]; }
  }
  if (pct > 0.004f) {
    int x, y;
    uiPolar(GCX, GCY, 109.0f, A0, x, y);
    uiCircle(x, y, 5, P->accent);                       // round start cap
    uiPolar(GCX, GCY, 109.0f, A0 + pct * SW, x, y);
    uiCircle(x, y, 9, tip_col);                         // glowing tip
    uiCircle(x, y, 4, C565(255, 255, 255));
  }
  // End labels just outside the arc ends
  uiTextMid("0", 150, 244, &FreeSans9pt7b, P->text_dim, AL_CENTER);
  uiTextMid(imperial_mode ? "50" : "85", 330, 244, &FreeSans9pt7b, P->text_dim, AL_CENTER);

  uiBigText(spd, GCX, 98, P->text, AL_CENTER);
  uiText(unit, GCX, 180, &FreeSansBold12pt7b, P->accent, AL_CENTER);

  const Rect bar = {180, 214, 120, 6};
  uiProgress(bar, pct, speedZoneColor(pct));
}

static void gaugeAnalog(float pct, float gmax, const char *spd, const char *unit) {
  const float A0 = 135.0f, SW = 270.0f;
  const float red_from = imperial_mode ? 45.0f : 70.0f;
  const int minor = imperial_mode ? 5 : 10;
  const int major = imperial_mode ? 10 : 20;

  uiArc(GCX, GCY, 110, 114, A0, A0 + SW, P->track);
  uiArc(GCX, GCY, 106, 114, A0 + red_from / gmax * SW, A0 + SW, P->red);

  // Ticks and labels at their TRUE angles for the gauge's full scale
  char b[8];
  for (int v = 0; v <= (int)gmax; v += minor) {
    const float a = A0 + (float)v / gmax * SW;
    const bool mj = (v % major) == 0;
    const uint16_t c = (v >= red_from) ? P->red : (mj ? P->text : P->text_dim);
    const float hw = mj ? 1.2f : 0.7f;
    uiArc(GCX, GCY, mj ? 94 : 102, 114, a - hw, a + hw, c, 5.0f);
    if (mj) {
      int lx, ly;
      uiPolar(GCX, GCY, 80.0f, a, lx, ly);
      snprintf(b, sizeof(b), "%d", v);
      uiTextMid(b, lx, ly, &FreeSans9pt7b, P->text_dim, AL_CENTER);
    }
  }

  // Tapered needle
  const float na = (A0 + pct * SW) * UI_DEG2RAD;
  const float ux = cosf(na), uy = sinf(na);
  const float px = -uy, py = ux;
  ui_g->fillTriangle(lroundf(GCX + 100 * ux), lroundf(GCY + 100 * uy),
                     lroundf(GCX - 16 * ux + 6 * px), lroundf(GCY - 16 * uy + 6 * py),
                     lroundf(GCX - 16 * ux - 6 * px), lroundf(GCY - 16 * uy - 6 * py), P->red);

  // Hub with digital readout
  uiCircle(GCX, GCY, 42, P->border);
  uiCircle(GCX, GCY, 40, P->surface);
  uiText(spd, GCX, 130, &FreeSansBold18pt7b, P->text, AL_CENTER);
  uiText(unit, GCX, 162, &FreeSans9pt7b, P->accent, AL_CENTER);
}

static void gaugeF1(float pct, const char *spd, const char *unit) {
  // Shift lights
  const int N = 15;
  const int act = (int)lroundf(pct * N);
  const bool flash = pct >= 0.92f && ((g_frame_ms / 80) % 2 == 0);
  for (int i = 0; i < N; i++) {
    const int x = 75 + i * 22, y = 40;
    if (flash) uiRRect(x, y, 18, 14, 3, P->text);
    else if (i < act) uiRRect(x, y, 18, 14, 3, i < 5 ? P->green : (i < 10 ? P->red : P->purple));
    else { uiRRect(x, y, 18, 14, 3, P->track); uiRRectLine(x, y, 18, 14, 3, P->border); }
  }

  const Rect L = {14, 62, 90, 200}, C = {112, 62, 256, 200}, R = {376, 62, 90, 200};
  uiCard(L, false, false, 10);
  uiCard(R, false, false, 10);
  uiCard(C, false, false, 10);

  // Left: G-force
  char b[16];
  uiText("G-FORCE", L.cx(), 74, &FreeSansBold9pt7b, P->text_dim, AL_CENTER);
  snprintf(b, sizeof(b), "%.2fG", current_accel_g);
  uiText(b, L.cx(), 96, &FreeSansBold12pt7b, current_accel_pct > 60 ? P->amber : P->accent, AL_CENTER);
  for (int i = 0; i < 10; i++) {
    const int by = 212 - i * 10;
    uint16_t c = P->track;
    if (i < current_accel_pct / 10) c = (i < 5) ? P->green : (i < 8 ? P->amber : P->red);
    uiRRect(L.x + 15, by, 60, 7, 2, c);
  }
  const bool boost = current_accel_pct > 50;
  uiText(boost ? "BOOST" : "PULL", L.cx(), 232, &FreeSansBold9pt7b, boost ? P->red : P->green, AL_CENTER);

  // Right: peak, mode, DRS
  uiText("PEAK", R.cx(), 74, &FreeSansBold9pt7b, P->text_dim, AL_CENTER);
  snprintf(b, sizeof(b), "%d", (int)lroundf(session_max_speed * (imperial_mode ? KMH_TO_MPH : 1.0f)));
  uiText(b, R.cx(), 96, &FreeSansBold12pt7b, P->amber, AL_CENTER);
  const Rect mode = {386, 128, 70, 50};
  uiRRect(mode, 8, P->surface_hi);
  uiText("MODE", mode.cx(), 135, &FreeSans9pt7b, P->text_dim, AL_CENTER);
  uiText("HOT", mode.cx(), 154, &FreeSansBold12pt7b, P->red, AL_CENTER);
  const bool drs = pct >= 0.6f;
  const Rect drsr = {386, 196, 70, 34};
  uiRRect(drsr, 8, drs ? P->green : P->surface_hi);
  uiTextMid("DRS", drsr.cx(), drsr.cy(), &FreeSansBold9pt7b, drs ? P->on_accent : P->text_dim, AL_CENTER);

  // Centre HUD with red corner brackets
  const uint16_t red = P->red;
  uiFill(C.x, C.y, 18, 3, red);                 uiFill(C.x, C.y, 3, 18, red);
  uiFill(C.right() - 18, C.y, 18, 3, red);      uiFill(C.right() - 3, C.y, 3, 18, red);
  uiFill(C.x, C.bottom() - 3, 18, 3, red);      uiFill(C.x, C.bottom() - 18, 3, 18, red);
  uiFill(C.right() - 18, C.bottom() - 3, 18, 3, red); uiFill(C.right() - 3, C.bottom() - 18, 3, 18, red);

  uiBigText(spd, 240, 86, P->text, AL_CENTER);
  const Rect cap = {195, 172, 90, 26};
  uiRRect(cap, 8, P->surface_hi);
  uiRRectLine(cap, 8, P->accent);
  uiTextMid(unit, cap.cx(), cap.cy(), &FreeSansBold9pt7b, P->accent, AL_CENTER);
  const Rect bar = {134, 222, 212, 6};
  uiProgress(bar, pct, speedZoneColor(pct));
}

static void drawDashBottom(double odo, double trip) {
  const float k = imperial_mode ? KMH_TO_MPH : 1.0f;
  const char *du = imperial_mode ? "mi" : "km";
  char v[24];

  // TRIP (hold 1 s to reset, with a filling progress bar)
  const float hold = uiHold(TGT_TRIP_RESET);
  uiCard(R_TRIP, false, false);
  if (hold > 0.0f) {
    uiRRect(R_TRIP.x, R_TRIP.y, (int)(R_TRIP.w * hold), R_TRIP.h, UI_RADIUS, P->danger_bg);
    uiRRectLine(R_TRIP, UI_RADIUS, P->red);
  }
  const int cy = R_TRIP.cy();
  uiTextMid("TRIP", R_TRIP.x + 12, cy, &FreeSans9pt7b, P->text_dim);
  snprintf(v, sizeof(v), "%.1f %s", trip * k, du);
  const int vx = R_TRIP.x + 12 + uiTextW("TRIP", &FreeSans9pt7b) + 10;
  uiTextMid(v, vx, cy, &FreeSansBold12pt7b, P->text);
  const int vend = vx + uiTextW(v, &FreeSansBold12pt7b);
  const char *hint;
  uint16_t hcol;
  if (trip_reset_flash_until && (int32_t)(trip_reset_flash_until - g_frame_ms) > 0) { hint = "RESET!"; hcol = P->green; }
  else if (hold > 0.0f) { hint = TR("HOLD..", "TART.."); hcol = P->red; }
  else { hint = TR("HOLD", "TART"); hcol = P->text_faint; }
  if (vend + 8 + uiTextW(hint, &FreeSans9pt7b) < R_TRIP.right() - 10)
    uiTextMid(hint, R_TRIP.right() - 10, cy, &FreeSans9pt7b, hcol, AL_RIGHT);

  // ODO
  uiCard(R_ODOBAR, false, false);
  uiTextMid("ODO", R_ODOBAR.x + 12, cy, &FreeSans9pt7b, P->text_dim);
  snprintf(v, sizeof(v), "%.1f %s", odo * k, du);
  uiTextMid(v, R_ODOBAR.right() - 12, cy, &FreeSansBold12pt7b, P->accent, AL_RIGHT);
}

static void drawDashboard(double odo, double trip) {
  float v = bike_speed_kmh;
  if (imperial_mode) v *= KMH_TO_MPH;
  const char *unit = imperial_mode ? "MPH" : "KM/H";

  // Hysteresis on the displayed integer (no digit flicker)
  static int stable_spd = 0;
  const int rounded = (int)lroundf(v);
  if (fabsf(v - (float)stable_spd) >= 0.65f || rounded == 0) stable_spd = rounded;
  char spd[8];
  snprintf(spd, sizeof(spd), "%d", stable_spd);

  const float gmax = imperial_mode ? GAUGE_MAX_MPH : GAUGE_MAX_KMH;
  const float pct = constrain(v / gmax, 0.0f, 1.0f);

  // Top bar
  char left[24], right[24];
  const unsigned rs = ride_seconds;
  const unsigned hrs = rs / 3600, mins = (rs / 60) % 60;
  if (hrs > 0) snprintf(left, sizeof(left), "RIDE %uh %02um", hrs, mins);
  else snprintf(left, sizeof(left), "RIDE %02u:%02u", mins, rs % 60);
  snprintf(right, sizeof(right), "MAX %d %s", (int)lroundf(session_max_speed * (imperial_mode ? KMH_TO_MPH : 1.0f)), unit);
  drawTopBar(left, &FreeSansBold9pt7b, P->text_dim, right, P->amber, 1);
  if (uiPressed(TGT_MAX_RESET)) uiRRectLine(R_TOPBAR_MAX.x + 20, 4, R_TOPBAR_MAX.w - 24, 28, 8, P->amber);

  switch (current_skin) {
    case SKIN_CYBER_HORIZON: gaugeCyber(pct, spd, unit); break;
    case SKIN_ANALOG_SPORT:  gaugeAnalog(pct, gmax, spd, unit); break;
    case SKIN_F1_RACE:       gaugeF1(pct, spd, unit); break;
    default:                 gaugeProArc(pct, spd, unit); break;
  }

  drawDashBottom(odo, trip);
}

// ================================================================================
// 12. RACE SCREEN
// ================================================================================

static void fmtTime(char *buf, size_t n, float t) {
  if (t > 0.001f) snprintf(buf, n, "%.2f s", t);
  else strlcpy(buf, "--.-- s", n);
}

static void drawRace() {
  const bool imp = imperial_mode;
  float v = bike_speed_kmh;
  if (imp) v *= KMH_TO_MPH;
  const int spd = (int)lroundf(v);
  const float pct = constrain(v / (imp ? GAUGE_MAX_MPH : GAUGE_MAX_KMH), 0.0f, 1.0f);

  drawTopBar(TR("RACE MODE", "VERSENY MOD"), &FreeSansBold12pt7b, P->accent,
             imp ? "0-30 MPH" : "0-50 KM/H", P->amber, 2);

  // Shift / speed LED bar
  const int LEDS = 28;
  const int lit = (int)lroundf(pct * LEDS);
  for (int i = 0; i < LEDS; i++) {
    const int x = 18 + i * 16;
    uint16_t c = P->track;
    if (i < lit) {
      if (i < LEDS * 50 / 100) c = P->green;
      else if (i < LEDS * 75 / 100) c = P->amber;
      else if (i < LEDS * 90 / 100) c = P->red;
      else c = ((g_frame_ms / 150) % 2 == 0) ? P->text : P->accent;
    }
    uiRRect(x, 40, 12, 10, 2, c);
  }

  // Speed with unit beside it
  char b[24];
  snprintf(b, sizeof(b), "%d", spd);
  uiBigText(b, 240, 56, P->text, AL_CENTER);
  const int nw = uiBigW(b, &FreeSansBold24pt7b);
  uiText(imp ? "mph" : "km/h", 240 + nw / 2 + 8, 56 + 69 - 17, &FreeSansBold12pt7b, P->text_dim);

  // ── Run card (tap to reset) ──
  uiCard(R_RUN, false, uiPressed(TGT_RACE_RESET));
  uiText(imp ? TR("0-30 MPH RUN", "0-30 MPH FUTAM") : TR("0-50 KM/H RUN", "0-50 KM/H FUTAM"),
         R_RUN.x + 12, 158, &FreeSansBold9pt7b, P->text_dim);

  const char *status;
  uint16_t scol, tcol;
  float tval;
  switch (accel_timer_state) {
    case ACCEL_RUNNING:
      status = TR("PULLING...", "GYORSITAS...");
      scol = P->amber;
      tval = current_0_50_time;
      tcol = t_50_us ? P->green : P->accent;
      break;
    case ACCEL_FINISHED:
      status = TR("RUN FINISHED", "FUTAM KESZ");
      scol = P->green;
      tval = last_0_50_time;
      tcol = P->green;
      break;
    case ACCEL_WAIT_STOP:
      status = TR("STOP TO ARM", "ALLJ MEG");
      scol = P->text_dim;
      tval = 0.0f;
      tcol = P->text_faint;
      break;
    default:
      status = TR("LAUNCH READY", "RAJT KESZ");
      scol = ((g_frame_ms / 500) % 2 == 0) ? P->green : P->text_dim;
      tval = 0.0f;
      tcol = P->text;
      break;
  }
  uiText(status, R_RUN.cx(), 180, &FreeSansBold12pt7b, scol, AL_CENTER);
  if (accel_timer_state == ACCEL_WAIT_STOP) strlcpy(b, "--.--s", sizeof(b));
  else snprintf(b, sizeof(b), "%.2fs", tval);
  uiText(b, R_RUN.cx(), 206, &FreeSansBold24pt7b, tcol, AL_CENTER);

  uiHLine(R_RUN.x + 12, 252, R_RUN.w - 24, P->sep);
  char t[16];
  uiText(TR("LAST", "UTOLSO"), R_RUN.x + 12, 260, &FreeSans9pt7b, P->text_dim);
  fmtTime(t, sizeof(t), (last_run_imperial == imp) ? last_0_50_time : 0.0f);
  uiText(t, R_RUN.right() - 12, 260, &FreeSansBold9pt7b, P->text, AL_RIGHT);
  uiText(TR("BEST", "LEGJOBB"), R_RUN.x + 12, 284, &FreeSans9pt7b, P->amber);
  fmtTime(t, sizeof(t), imp ? best_0_30mph_time : best_0_50_time);
  uiText(t, R_RUN.right() - 12, 284, &FreeSansBold9pt7b, P->amber, AL_RIGHT);

  // ── Splits card ──
  uiCard(R_SPLITS, false, false);
  const int lx = R_SPLITS.x + 12, rx = R_SPLITS.right() - 12;
  uiText(TR("INTERVAL SPLITS", "RESZIDOK"), lx, 158, &FreeSansBold9pt7b, P->accent);
  uiHLine(lx, 178, R_SPLITS.w - 24, P->sep);

  const char *labels_m[3] = {"50 - 60 KM/H", "60 - 70 KM/H", "70 - 80 KM/H"};
  const char *labels_i[3] = {"30 - 40 MPH", "40 - 50 MPH", "50 - 60 MPH"};
  const float splits[3] = {split_50_60, split_60_70, split_70_80};
  for (int i = 0; i < 3; i++) {
    const int y = 188 + i * 26;
    uiText(imp ? labels_i[i] : labels_m[i], lx, y, &FreeSans9pt7b, P->text_dim);
    fmtTime(t, sizeof(t), splits[i]);
    uiText(t, rx, y, &FreeSansBold9pt7b, splits[i] > 0.001f ? P->text : P->text_faint, AL_RIGHT);
  }
  uiHLine(lx, 266, R_SPLITS.w - 24, P->sep);
  uiText(imp ? TR("0-60 MPH TOTAL", "0-60 MPH OSSZ") : TR("0-80 KM/H TOTAL", "0-80 KM/H OSSZ"),
         lx, 278, &FreeSans9pt7b, P->amber);
  fmtTime(t, sizeof(t), time_0_80);
  uiText(t, rx, 278, &FreeSansBold9pt7b, time_0_80 > 0.001f ? P->amber : P->text_faint, AL_RIGHT);
}

// ================================================================================
// 13. SETTINGS
// ================================================================================

static void drawTabs() {
  int16_t xs[SUB_COUNT], ws[SUB_COUNT];
  tabLayout(xs, ws);
  uiRRect(R_TABBAR, 12, P->surface);
  uiRRectLine(R_TABBAR, 12, P->border);
  for (int i = 0; i < SUB_COUNT; i++) {
    const Rect r = {xs[i], (int16_t)(R_TABBAR.y + 3), ws[i], (int16_t)(R_TABBAR.h - 6)};
    const bool active = (active_submenu == i);
    if (active) uiRRect(r, 9, P->accent);
    else if (uiPressed(TGT_TAB0 + i)) uiRRect(r, 9, P->pressed);
    uiTextMid(tabLabel(i), r.cx(), r.cy(), &FreeSansBold9pt7b, active ? P->on_accent : P->text_dim, AL_CENTER);
  }
}

static void drawSettingsSystem() {
  // Brightness slider
  uiCard(R_BRIGHT, false, false);
  char b[8];
  uiText(TR("BRIGHTNESS", "FENYERO"), R_BRIGHT.x + 16, 92, &FreeSansBold9pt7b, P->text_dim);
  snprintf(b, sizeof(b), "%d%%", (int)lroundf(screen_brightness * 100.0f / 255.0f));
  uiText(b, R_BRIGHT.right() - 16, 92, &FreeSansBold9pt7b, P->accent, AL_RIGHT);
  const int x0 = R_BRIGHT.x + 18, x1 = R_BRIGHT.right() - 18;
  const int fill = (int)lroundf((float)(screen_brightness - 20) * (x1 - x0) / (255 - 20));
  uiRRect(x0, 122, x1 - x0, 10, 5, P->track);
  uiRRect(x0, 122, fill < 10 ? 10 : fill, 10, 5, P->accent);
  const bool drag = T.down && T.target == TGT_BRIGHT;
  uiCircle(x0 + fill, 127, drag ? 14 : 12, P->accent);
  uiCircle(x0 + fill, 127, drag ? 6 : 5, P->surface);

  // Units
  uiCard(R_UNITS, false, uiPressed(TGT_UNITS));
  uiText(TR("UNITS", "MERTEKEGYSEG"), R_UNITS.x + 16, R_UNITS.y + 14, &FreeSansBold12pt7b, P->text);
  uiText(TR("Speed & distance", "Sebesseg es tavolsag"), R_UNITS.x + 16, R_UNITS.y + 44, &FreeSans9pt7b, P->text_dim);
  const Rect su = {272, (int16_t)(R_UNITS.y + 18), 184, 34};
  uiSegmented(su, "KM/H", "MPH", imperial_mode);

  // Theme
  uiCard(R_THEME, false, uiPressed(TGT_THEME));
  uiText(TR("THEME", "TEMA"), R_THEME.x + 16, R_THEME.y + 14, &FreeSansBold12pt7b, P->text);
  uiText(TR("Night / day display", "Ejszakai / nappali"), R_THEME.x + 16, R_THEME.y + 44, &FreeSans9pt7b, P->text_dim);
  const Rect st = {272, (int16_t)(R_THEME.y + 18), 184, 34};
  uiSegmented(st, TR("DARK", "SOTET"), TR("LIGHT", "VILAGOS"), theme_light);
}

static void drawSettingsSkin() {
  static const char *const names[4] = {"PRO ARC", "CYBER HORIZON", "ANALOG SPORT", "FORMULA 1"};
  static const char *const desc_en[4] = {
    "26-LED segmented race arc & digital speed",
    "Continuous neon halo & dynamic power bar",
    "Sports dial & sweeping red needle",
    "F1 shift lights, telemetry sidebars & HUD"};
  static const char *const desc_hu[4] = {
    "26 LED-es szegmenses iv & digitalis kijelzo",
    "Folytonos neon iv & dinamikus power bar",
    "Sport szamlap & piros mutato",
    "F1 shift lampak, telemetria & verseny HUD"};
  for (int i = 0; i < 4; i++) {
    const Rect r = skinRect(i);
    const bool sel = ((int)current_skin == i);
    uiCard(r, sel, uiPressed(TGT_SKIN0 + i));
    const int cy = r.cy();
    uiCircle(r.x + 28, cy, 15, sel ? P->accent : P->surface_hi);
    char n[2] = {(char)('1' + i), 0};
    uiTextMid(n, r.x + 28, cy, &FreeSansBold9pt7b, sel ? P->on_accent : P->text_dim, AL_CENTER);
    uiText(names[i], r.x + 54, cy - 16, &FreeSansBold9pt7b, sel ? P->accent : P->text);
    uiText(current_lang == LANG_HU ? desc_hu[i] : desc_en[i], r.x + 54, cy + 4, &FreeSans9pt7b, P->text_dim);
    uiRadio(r.right() - 22, cy, sel);
  }
}

static void drawSettingsSpeed() {
  // Calibration
  uiCard(R_CALCARD, false, false);
  uiText(TR("SPEED CALIBRATION", "SEBESSEG KALIBRACIO"), 240, 90, &FreeSansBold9pt7b, P->text_dim, AL_CENTER);
  uiButton(R_CAL_MINUS, "-0.01", &FreeSansBold12pt7b, uiPressed(TGT_CAL_MINUS), P->surface_hi, P->text);
  uiButton(R_CAL_PLUS, "+0.01", &FreeSansBold12pt7b, uiPressed(TGT_CAL_PLUS), P->surface_hi, P->text);
  char b[12];
  snprintf(b, sizeof(b), "%.2fx", (float)speed_cal);
  uiBigText(b, 240, 114, P->accent, AL_CENTER);

  // Filter mode
  uiCard(R_FILTERCARD, false, false);
  uiText(TR("SPEED RESPONSE & FILTERING", "SEBESSEG VALASZIDO ES SZURES"), 240, 216, &FreeSansBold9pt7b, P->text, AL_CENTER);

  const bool fast = !speed_filter_oem;
  uiRRect(R_FILTER_FAST, 10, uiPressed(TGT_FILTER_FAST) ? P->pressed : (fast ? P->accent : P->surface_hi));
  uiText(TR("FAST / ADAPTIVE", "GYORS / VALOS IDEJU"), R_FILTER_FAST.cx(), R_FILTER_FAST.y + 12, &FreeSansBold9pt7b,
         fast ? P->on_accent : P->text_dim, AL_CENTER);
  uiText(TR("Zero lag, instant stop", "Azonnali reakcio"), R_FILTER_FAST.cx(), R_FILTER_FAST.y + 36, &FreeSans9pt7b,
         fast ? P->on_accent : P->text_faint, AL_CENTER);

  const bool oem = speed_filter_oem;
  uiRRect(R_FILTER_OEM, 10, uiPressed(TGT_FILTER_OEM) ? P->pressed : (oem ? P->amber : P->surface_hi));
  uiText(TR("OEM SMOOTH", "LASSU / GYARI OEM"), R_FILTER_OEM.cx(), R_FILTER_OEM.y + 12, &FreeSansBold9pt7b,
         oem ? P->on_accent : P->text_dim, AL_CENTER);
  uiText(TR("12-pulse average", "Simitott kijelzes"), R_FILTER_OEM.cx(), R_FILTER_OEM.y + 36, &FreeSans9pt7b,
         oem ? P->on_accent : P->text_faint, AL_CENTER);
}

static void drawSettingsOdo(double odo) {
  uiCard(R_ODOCARD, false, false);
  uiText(TR("TOTAL ODOMETER", "OSSZES MEGTETT TAV"), 240, 92, &FreeSansBold9pt7b, P->text_dim, AL_CENTER);
  char b[24];
  snprintf(b, sizeof(b), "%ld KM", (long)llround(odo));
  uiBigText(b, 240, 112, P->accent, AL_CENTER);

  uiButton(R_ODO_MINUS, "-10 KM", &FreeSansBold12pt7b, uiPressed(TGT_ODO_MINUS), P->surface_hi, P->text);
  uiButton(R_ODO_PLUS, "+10 KM", &FreeSansBold12pt7b, uiPressed(TGT_ODO_PLUS), P->surface_hi, P->text);
  uiTextMid(TR("HOLD = FAST", "TART = GYORS"), 240, R_ODO_MINUS.cy(), &FreeSans9pt7b, P->text_faint, AL_CENTER);

  // Reset: hold 2 s (progress fill), no single-tap wipe
  const float hold = uiHold(TGT_ODO_RESET);
  uiRRect(R_ODO_RESET, 10, P->danger_bg);
  if (hold > 0.0f) uiRRect(R_ODO_RESET.x, R_ODO_RESET.y, (int)(R_ODO_RESET.w * hold), R_ODO_RESET.h, 10, P->red);
  uiRRectLine(R_ODO_RESET, 10, P->red);
  const char *lbl;
  if (odo_reset_flash_until && (int32_t)(odo_reset_flash_until - g_frame_ms) > 0) lbl = TR("ODOMETER RESET", "ODO NULLAZVA");
  else lbl = TR("HOLD 2 s TO RESET ODO", "TARTSD 2 MP: ODO NULLAZAS");
  uiTextMid(lbl, 240, R_ODO_RESET.cy(), &FreeSansBold9pt7b, hold > 0.5f ? C565(255, 255, 255) : (theme_light ? P->red : P->text), AL_CENTER);
}

static void drawSettingsLang() {
  const bool hu = (current_lang == LANG_HU);
  uiCard(R_LANG_EN, !hu, uiPressed(TGT_LANG_EN));
  uiText("ENGLISH", R_LANG_EN.x + 24, R_LANG_EN.cy() - 22, &FreeSansBold12pt7b, P->text);
  uiText("English user interface", R_LANG_EN.x + 24, R_LANG_EN.cy() + 8, &FreeSans9pt7b, P->text_dim);
  uiRadio(R_LANG_EN.right() - 26, R_LANG_EN.cy(), !hu);

  uiCard(R_LANG_HU, hu, uiPressed(TGT_LANG_HU));
  uiText("MAGYAR", R_LANG_HU.x + 24, R_LANG_HU.cy() - 22, &FreeSansBold12pt7b, P->text);
  uiText("Magyar nyelvu felulet", R_LANG_HU.x + 24, R_LANG_HU.cy() + 8, &FreeSans9pt7b, P->text_dim);
  uiRadio(R_LANG_HU.right() - 26, R_LANG_HU.cy(), hu);
}

// ---- WiFi QR ----------------------------------------------------------------

// Escapes \ ; , : " as required by the WIFI: QR format.
static void wifiQrEscape(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (; *in && o + 2 < n; ++in) {
    if (strchr("\\;,:\"", *in)) out[o++] = '\\';
    out[o++] = *in;
  }
  out[o] = 0;
}

// Byte-mode capacity for ECC LOW, versions 1..10. The bundled QR encoder does
// NOT check capacity (it overruns its stack buffer), so pick the version here.
static uint8_t qrVersionFor(size_t len) {
  static const uint16_t cap[10] = {17, 32, 53, 78, 106, 134, 154, 192, 230, 271};
  for (uint8_t v = 1; v <= 10; v++) {
    if (len <= cap[v - 1] && qrcode_getBufferSize(v) <= sizeof(qr_modules)) return v;
  }
  return 0;
}

static void ensureWifiQr(const WebOtaStatus &st) {
  char ssid[70], pass[134], payload[sizeof(qr_payload)];
  wifiQrEscape(st.ssid, ssid, sizeof(ssid));
  wifiQrEscape(st.password, pass, sizeof(pass));
  if (pass[0]) snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
  else snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ssid);
  if (strcmp(payload, qr_payload) == 0) return;
  strlcpy(qr_payload, payload, sizeof(qr_payload));
  const uint8_t v = qrVersionFor(strlen(payload));
  qr_ok = v && qrcode_initText(&qr, qr_modules, v, ECC_LOW, payload) == 0;
}

static void drawWifiQr(const Rect &panel) {
  uiRRect(panel, 10, C565(255, 255, 255));  // white in both themes (quiet zone)
  if (!qr_ok) {
    uiTextMid("QR ?", panel.cx(), panel.cy(), &FreeSansBold12pt7b, C565(0, 0, 0), AL_CENTER);
    return;
  }
  const int size = qr.size;
  const int avail = (panel.w < panel.h ? panel.w : panel.h);
  int m = avail / (size + 8);          // >= 4 module quiet zone
  if (m < 2) m = avail / (size + 2);
  if (m < 1) m = 1;
  const int total = size * m;
  const int x0 = panel.x + (panel.w - total) / 2;
  const int y0 = panel.y + (panel.h - total) / 2;
  const uint16_t black = C565(0, 0, 0);
  for (int y = 0; y < size; y++) {
    int x = 0;
    while (x < size) {
      if (!qrcode_getModule(&qr, x, y)) { x++; continue; }
      int run = 1;
      while (x + run < size && qrcode_getModule(&qr, x + run, y)) run++;
      ui_g->fillRect(x0 + x * m, y0 + y * m, run * m, m, black);
      x += run;
    }
  }
}

// Word-wraps `s` into at most 2 lines of `maxw` px.
static void drawWrapped2(const char *s, int x, int y, int maxw, const GFXfont *f, uint16_t c) {
  char line[80];
  size_t len = strlen(s);
  size_t cut = len;
  strlcpy(line, s, sizeof(line));
  if (uiTextW(line, f) > maxw) {
    // find the last space that fits
    cut = 0;
    for (size_t i = 1; i < len && i < sizeof(line) - 1; i++) {
      if (s[i] != ' ') continue;
      memcpy(line, s, i);
      line[i] = 0;
      if (uiTextW(line, f) <= maxw) cut = i; else break;
    }
    if (cut == 0) cut = len;  // no space: will be cut with ".."
  }
  memcpy(line, s, cut < sizeof(line) - 1 ? cut : sizeof(line) - 1);
  line[cut < sizeof(line) - 1 ? cut : sizeof(line) - 1] = 0;
  char fit[80];
  uiFit(line, maxw, f, fit, sizeof(fit));
  uiText(fit, x, y, f, c);
  if (cut < len) {
    uiFit(s + cut + 1, maxw, f, fit, sizeof(fit));
    uiText(fit, x, y + 20, f, c);
  }
}

static void drawSettingsWifi() {
  const bool on = g_ota.state != WEBOTA_OFF;
  const bool err = g_ota.state == WEBOTA_ERROR;

  uiCard(R_WIFI_TOGGLE, on, uiPressed(TGT_WIFI_TOGGLE));
  uiText(TR("UPDATE HOTSPOT", "FRISSITO HOTSPOT"), R_WIFI_TOGGLE.x + 16, 91, &FreeSansBold12pt7b, P->text);
  char sub[64];
  if (!on) strlcpy(sub, TR("Off - tap to start WiFi update", "Ki - erintsd a WiFi frissiteshez"), sizeof(sub));
  else if (err) strlcpy(sub, TR("On - last update failed", "Be - a frissites sikertelen"), sizeof(sub));
  else snprintf(sub, sizeof(sub), TR("On - %u device(s) connected", "Be - %u eszkoz csatlakozva"), (unsigned)g_ota.clients);
  uiText(sub, R_WIFI_TOGGLE.x + 16, 117, &FreeSans9pt7b, !on ? P->text_dim : (err ? P->red : P->green));
  uiToggle(R_WIFI_TOGGLE.right() - 72, R_WIFI_TOGGLE.cy() - 15, on, false);

  char fw[48];
  if (!on) {
    uiCard(R_WIFI_HELP, false, false);
    const int x = R_WIFI_HELP.x + 16;
    uiText(TR("1. Turn on the hotspot above", "1. Kapcsold be a hotspotot fent"), x, 162, &FreeSans9pt7b, P->text);
    uiText(TR("2. Scan the QR code with your phone", "2. Olvasd be a QR kodot a telefonnal"), x, 188, &FreeSans9pt7b, P->text);
    uiText(TR("3. Open the address shown", "3. Nyisd meg a kiirt cimet"), x, 214, &FreeSans9pt7b, P->text);
    uiText(TR("4. Upload the new .bin firmware", "4. Toltsd fel az uj .bin fajlt"), x, 240, &FreeSans9pt7b, P->text);
    uiHLine(x, 264, R_WIFI_HELP.w - 32, P->sep);
    snprintf(fw, sizeof(fw), "FIRMWARE %s", FW_VERSION);
    uiText(fw, x, 276, &FreeSansBold9pt7b, P->text_dim);
    uiText(FW_BUILD_DATE, R_WIFI_HELP.right() - 16, 276, &FreeSans9pt7b, P->text_faint, AL_RIGHT);
    uiWifiIcon(404, 238, P->surface_hi, 2.6f);
    return;
  }

  // Credentials + status
  uiCard(R_WIFI_INFO, false, false);
  const int lx = R_WIFI_INFO.x + 12, vx = R_WIFI_INFO.x + 82, vw = R_WIFI_INFO.right() - 10 - vx;
  char v[80];
  uiText("WIFI", lx, 162, &FreeSans9pt7b, P->text_dim);
  // "WheelieAssist-XXXX" is 168..180 px bold (> vw = 172) but <= 171 px regular
  const GFXfont *sf = uiTextW(g_ota.ssid, &FreeSansBold9pt7b) <= vw ? &FreeSansBold9pt7b : &FreeSans9pt7b;
  uiFit(g_ota.ssid, vw, sf, v, sizeof(v));
  uiText(v, vx, 162, sf, P->text);
  uiText(TR("PASS", "JELSZO"), lx, 188, &FreeSans9pt7b, P->text_dim);
  uiFit(g_ota.password[0] ? g_ota.password : "-", vw, &FreeSansBold9pt7b, v, sizeof(v));
  uiText(v, vx, 188, &FreeSansBold9pt7b, P->text);
  uiText(TR("OPEN", "CIM"), lx, 214, &FreeSans9pt7b, P->text_dim);
  char url[40];
  snprintf(url, sizeof(url), "http://%s", g_ota.ip[0] ? g_ota.ip : "...");
  uiFit(url, vw, &FreeSansBold9pt7b, v, sizeof(v));
  uiText(v, vx, 214, &FreeSansBold9pt7b, P->accent);
  uiHLine(lx, 238, R_WIFI_INFO.w - 24, P->sep);
  snprintf(fw, sizeof(fw), "FW %s", FW_VERSION);
  uiText(fw, lx, 248, &FreeSansBold9pt7b, P->text_dim);
  if (err) {
    char e[80];
    snprintf(e, sizeof(e), "%s %s", TR("ERROR:", "HIBA:"), g_ota.error[0] ? g_ota.error : "?");
    drawWrapped2(e, lx, 272, R_WIFI_INFO.w - 24, &FreeSans9pt7b, P->red);
  } else {
    uiText(TR("Waiting for upload...", "Varakozas a feltoltesre..."), lx, 272, &FreeSans9pt7b, P->text_faint);
  }

  ensureWifiQr(g_ota);
  drawWifiQr(R_WIFI_QR);
}

static void drawSettings(double odo) {
  char right[24];
  snprintf(right, sizeof(right), "FW %s", FW_VERSION);
  drawTopBar(TR("SETTINGS", "BEALLITASOK"), &FreeSansBold12pt7b, P->accent, right, P->text_faint, 0);
  drawTabs();
  switch (active_submenu) {
    case SUB_SYSTEM:   drawSettingsSystem(); break;
    case SUB_SKIN:     drawSettingsSkin(); break;
    case SUB_SPEED:    drawSettingsSpeed(); break;
    case SUB_ODO:      drawSettingsOdo(odo); break;
    case SUB_LANGUAGE: drawSettingsLang(); break;
    case SUB_WIFI:     drawSettingsWifi(); break;
    default: break;
  }
}

// ================================================================================
// 14. FIRMWARE UPDATE OVERLAY (any screen, input blocked)
// ================================================================================

static void drawOtaOverlay() {
  if (g_ota.state == WEBOTA_SUCCESS) {
    const int cx = 240, cy = 112;
    uiCircle(cx, cy, 40, P->green);
    uiThickLine(cx - 18, cy + 1, cx - 5, cy + 14, 7, P->on_accent);
    uiThickLine(cx - 5, cy + 14, cx + 20, cy - 12, 7, P->on_accent);
    uiText(TR("UPDATE OK", "FRISSITES KESZ"), 240, 176, &FreeSansBold18pt7b, P->green, AL_CENTER);
    uiText(TR("Restarting...", "Ujraindul..."), 240, 220, &FreeSansBold12pt7b, P->text, AL_CENTER);
    return;
  }

  uiText(TR("UPDATING FIRMWARE", "FIRMWARE FRISSITES"), 240, 36, &FreeSansBold18pt7b, P->text, AL_CENTER);
  uiText(TR("Do not switch off the bike", "Ne kapcsold ki a motort!"), 240, 78, &FreeSansBold12pt7b, P->amber, AL_CENTER);

  char b[32];
  const Rect bar = {40, 206, 400, 24};
  if (g_ota.bytes_total > 0) {
    unsigned pct = g_ota.progress_pct > 100 ? 100 : g_ota.progress_pct;
    snprintf(b, sizeof(b), "%u%%", pct);
    uiBigText(b, 240, 118, P->accent, AL_CENTER);
    uiProgress(bar, pct / 100.0f, P->accent);
    snprintf(b, sizeof(b), "%.2f / %.2f MB", g_ota.bytes_written / 1048576.0f, g_ota.bytes_total / 1048576.0f);
  } else {
    // Unknown length: show bytes and an indeterminate sweep
    snprintf(b, sizeof(b), "%lu", (unsigned long)(g_ota.bytes_written / 1024));
    strlcat(b, " KB", sizeof(b));
    uiBigText(b, 240, 118, P->accent, AL_CENTER);
    uiRRect(bar, bar.h / 2, P->track);
    const int seg = 100;
    const int pos = (int)((g_frame_ms / 8) % (bar.w - seg));
    uiRRect(bar.x + pos, bar.y, seg, bar.h, bar.h / 2, P->accent);
    b[0] = 0;
  }
  if (b[0]) uiText(b, 240, 246, &FreeSans9pt7b, P->text_dim, AL_CENTER);
  char fw[40];
  snprintf(fw, sizeof(fw), "%s %s", TR("Current firmware", "Jelenlegi verzio"), FW_VERSION);
  uiText(fw, 240, 280, &FreeSans9pt7b, P->text_faint, AL_CENTER);
}

// ================================================================================
// 15. FRAME RENDER
// ================================================================================

static void renderUI() {
  P = theme_light ? &PAL_LIGHT : &PAL_DARK;
  ui_g->fillScreen(P->bg);

  if (g_ota.state == WEBOTA_UPLOADING || g_ota.state == WEBOTA_SUCCESS) {
    drawOtaOverlay();
    display.flush();
    return;
  }

  double odo, trip;
  portENTER_CRITICAL(&g_tel_mux);
  odo = g_odo_km;
  trip = g_trip_km;
  portEXIT_CRITICAL(&g_tel_mux);

  switch (current_screen) {
    case SCREEN_DASHBOARD: drawDashboard(odo, trip); break;
    case SCREEN_RACE:      drawRace(); break;
    case SCREEN_SETTINGS:  drawSettings(odo); break;
    default: break;
  }
  display.flush();
}

// ================================================================================
// 16. UI & TELEMETRY TASK (core 1)
// ================================================================================

void UITask(void *pvParameters) {
  (void)pvParameters;
  Serial.println("[UI] Initializing touch LCD...");
  display_initialized = display.begin();
  if (display_initialized) {
    ui_g = display.gfx;
    ui_g->setRotation(1);     // landscape for ALL drawing (see ui_gfx.h)
    ui_g->setTextWrap(false);
    Serial.println("[UI] Touch LCD initialized.");
  } else {
    Serial.println("[UI] Touch LCD init FAILED - running headless (telemetry only).");
  }

  pinMode(GFX_BL, OUTPUT);
  analogWrite(GFX_BL, screen_brightness);

  if (display_initialized) drawBootScreen();
  current_screen = SCREEN_DASHBOARD;

  TickType_t last_wake = xTaskGetTickCount();
  uint32_t last_ms = millis();
  uint32_t last_diag_ms = last_ms;

  for (;;) {
    const uint32_t now_ms = millis();
    const uint32_t dt_ms = now_ms - last_ms;
    last_ms = now_ms;
    g_frame_ms = now_ms;

    updateTelemetry(dt_ms);
    webOtaGetStatus(g_ota);
    const bool ota_busy = (g_ota.state == WEBOTA_UPLOADING || g_ota.state == WEBOTA_SUCCESS);
    static WebOtaState prev_ota_state = WEBOTA_OFF;
    if (prev_ota_state == WEBOTA_UPLOADING && g_ota.state == WEBOTA_ERROR) {
      current_screen = SCREEN_SETTINGS;  // failed upload: show the error on the WIFI tab
      active_submenu = SUB_WIFI;
    }
    prev_ota_state = g_ota.state;

    if (display_initialized) {
      handleTouch(ota_busy);
      renderUI();
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
// 17. SETUP / LOOP
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
