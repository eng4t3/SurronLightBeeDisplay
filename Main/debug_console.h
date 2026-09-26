// debug_console.h - line based debug / remote-control console on the USB CDC
// Serial port (115200). Used by tools/devshot.py for screenshots.
//
// Integration (UI task, once per frame):
//
//   dbg::poll();                         // read + execute pending commands
//   handleTouch();                       // uses dbg::touchOverride()
//   uint32_t t0 = micros();
//   if (!dbg::render()) renderUI();      // gfxtest card replaces the UI
//   uint32_t t1 = micros();
//   dbg::frameRendered(t1 - t0);         // serves `shot` (before flush)
//   lcd::present();
//   dbg::frameFlushed(micros() - t1);
//
// State commands (screen/tab/skin/theme/lang/units/wifi/mock/state) are
// forwarded to the app's handler, so the console does not depend on the UI code.
#pragma once

#include <Arduino.h>

class Arduino_GFX;

namespace dbg {

// App command handler. `cmd` is lower case, `arg` the rest of the line
// (trimmed, may be empty). It may print informational lines to `out` and
// returns APP_OK, APP_BAD_ARG or APP_UNKNOWN; the console then prints the
// final "OK <cmd>" / "ERR ..." line.
enum AppResult : int8_t { APP_UNKNOWN = -1, APP_BAD_ARG = 0, APP_OK = 1 };
typedef AppResult (*AppCommandFn)(const char *cmd, const char *arg, Print &out);

// canvas: the Arduino_GFX canvas (optional, only used by `selftest`).
void begin(AppCommandFn app, Arduino_GFX *canvas = nullptr);

// Reads Serial and executes complete lines. Call once per frame.
void poll();

// Injected touch (tap/hold/swipe). Returns true while an injection is
// active; then (x, y, touched) replace the panel reading for this frame.
bool touchOverride(uint16_t &x, uint16_t &y, bool &touched);

// Fake speed / acceleration for rendering tests (display values only).
bool speedOverride(float &kmh);
bool accelOverride(float &g);

// True while `gfxtest` owns the screen: the test card has been drawn and the
// app must skip its own rendering this frame.
bool render();

// Frame hooks: call after drawing (before flush) and after flush.
void frameRendered(uint32_t draw_us);
void frameFlushed(uint32_t flush_us);

}  // namespace dbg
