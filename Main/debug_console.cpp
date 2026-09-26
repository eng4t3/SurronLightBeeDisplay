// debug_console.cpp - see debug_console.h. Protocol:
//   * one command per line (\n or \r), max 95 chars, ASCII; longer lines and
//     unknown commands are rejected with "ERR ...".
//   * every command ends with exactly one line "OK <cmd>" or "ERR <reason>"
//     (tap/hold/swipe/shot reply when they have completed).
//   * `shot` sends "SHOT 480 320\n", 480*320*2 bytes of RGB565 little endian,
//     row-major landscape, then "\nEND\n", then "OK shot".
#include "debug_console.h"

#include <Arduino_GFX_Library.h>
#include <ctype.h>
#include <esp_timer.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_test.h"
#include "lcd_flush.h"

namespace dbg {
namespace {

AppCommandFn s_app = nullptr;
Arduino_GFX *s_canvas = nullptr;

// ---- line input
char s_line[96];
size_t s_len = 0;
bool s_overflow = false;

// ---- touch injection
enum InjKind : uint8_t { INJ_NONE = 0, INJ_PRESS, INJ_RELEASE };
struct Injection {
  uint8_t kind;
  bool started;
  int16_t x0, y0, x1, y1;
  uint32_t t0, dur;
  char reply[16];
};
Injection s_inj = {};

// ---- overrides
bool s_speedOn = false, s_accelOn = false;
float s_speed = 0.0f, s_accel = 0.0f;

// ---- gfxtest / shot
int s_testPage = -1;  // -1 = off
bool s_shotPending = false;

// ---- frame stats (ring of the last N frames)
constexpr int N = 64;
uint32_t s_draw[N], s_flush[N], s_period[N];
int s_n = 0, s_idx = 0;
uint32_t s_curDraw = 0;
int64_t s_lastFrameUs = 0;

void ok(const char *cmd) {
  Serial.print("OK ");
  Serial.println(cmd);
}
void err(const char *msg, const char *cmd = "") {
  Serial.print("ERR ");
  Serial.print(msg);
  if (cmd && *cmd) {
    Serial.print(' ');
    Serial.print(cmd);
  }
  Serial.println();
}

// Parses up to `max` integers from `s`; returns how many were read.
int parseInts(const char *s, long *v, int max) {
  int n = 0;
  while (*s && n < max) {
    while (*s == ' ') s++;
    if (!*s) break;
    char *end;
    const long x = strtol(s, &end, 10);
    if (end == s) return -1;
    v[n++] = x;
    s = end;
  }
  while (*s == ' ') s++;
  return *s ? -1 : n;
}

bool parseFloat(const char *s, float &out) {
  char *end;
  const float v = strtof(s, &end);
  if (end == s) return false;
  while (*end == ' ') end++;
  if (*end) return false;
  out = v;
  return true;
}

void startInjection(int x0, int y0, int x1, int y1, uint32_t dur, const char *reply) {
  s_inj.kind = INJ_PRESS;
  s_inj.started = false;
  s_inj.x0 = (int16_t)constrain(x0, 0, gfx::W - 1);
  s_inj.y0 = (int16_t)constrain(y0, 0, gfx::H - 1);
  s_inj.x1 = (int16_t)constrain(x1, 0, gfx::W - 1);
  s_inj.y1 = (int16_t)constrain(y1, 0, gfx::H - 1);
  s_inj.dur = dur;
  strlcpy(s_inj.reply, reply, sizeof(s_inj.reply));
  s_testPage = -1;  // real UI must be visible to receive input
}

void printFps() {
  if (!s_n) {
    Serial.println("fps: no frames yet");
    return;
  }
  uint64_t sd = 0, sf = 0, sp = 0;
  uint32_t md = 0, mf = 0, mp = 0;
  for (int i = 0; i < s_n; i++) {
    sd += s_draw[i];
    sf += s_flush[i];
    sp += s_period[i];
    md = max(md, s_draw[i]);
    mf = max(mf, s_flush[i]);
    mp = max(mp, s_period[i]);
  }
  const float ad = sd / (float)s_n / 1000.0f, af = sf / (float)s_n / 1000.0f;
  const float ap = sp / (float)s_n / 1000.0f;
  Serial.printf("fps %.1f  frame avg %.2f ms max %.2f | draw avg %.2f max %.2f | flush avg %.2f max %.2f ms (n=%d)\n",
                ap > 0 ? 1000.0f / ap : 0.0f, ap, mp / 1000.0f, ad, md / 1000.0f, af, mf / 1000.0f, s_n);
}

void printFlush() {
  const lcd::Stats st = lcd::stats();
  Serial.printf("panel: %s%s, SPI %d kHz | last transfer %lu us (cpu swap %lu, bus wait %lu), present blocked %lu us",
                lcd::fast() ? "fast" : "library flush", lcd::async() ? " async double-buffered" : "", st.spiKHz,
                (unsigned long)st.total, (unsigned long)st.copy, (unsigned long)st.wait,
                (unsigned long)st.presentWait);
  Serial.println();
}

void printHelp() {
  Serial.println(
      "commands:\n"
      "  help                      this list\n"
      "  shot                      dump the next frame (SHOT 480 320 + raw RGB565 LE + END)\n"
      "  fps [reset]               frame / draw / flush timing over the last 64 frames\n"
      "  screen <dash|race|settings>, tab <0-5>, skin <0-3>, theme <dark|light>\n"
      "  lang <en|hu>, units <metric|imperial>, wifi <on|off>, state\n"
      "  mock <on|off|wifi|demo>, mock race <ready|pulling|finished|stop|off>,\n"
      "  mock ota <progress|success|error|off>, mock hold <max|trip|odo> <0..1|off>,\n"
      "  mock boot <ms|off>        display-only sample data / states of the mockups\n"
      "  tap <x> <y> | hold <x> <y> <ms> | swipe <left|right> [y]   (injected touch)\n"
      "  speed <kmh|off>, accel <g|off>   fake display values (no odo/NVS/race effect)\n"
      "  gfxtest [0-3|off]         engine test card (held until another UI command)\n"
      "  bench                     engine micro-benchmarks\n"
      "  selftest                  check the landscape->framebuffer mapping\n"
      "  flush [fast|lib]          flush statistics / switch to the library flush\n"
      "  ping                      replies OK ping");
}

void selftest() {
  uint16_t *fb = gfx::framebuffer();
  if (!fb || !s_canvas) {
    err("no display");
    return;
  }
  struct P {
    int x, y;
    uint8_t r, g, b;
  } pts[] = {{0, 0, 255, 0, 0}, {479, 0, 0, 255, 0}, {0, 319, 0, 0, 255}, {479, 319, 255, 255, 255}, {123, 45, 18, 200, 90}};
  bool all = true;
  Serial.printf("canvas rotation %d, size %dx%d\n", s_canvas->getRotation(), s_canvas->width(), s_canvas->height());
  for (const P &p : pts) {
    const uint16_t c = s_canvas->color565(p.r, p.g, p.b);
    s_canvas->writePixel(p.x, p.y, c);
    const uint16_t got = fb[p.x * gfx::H + (gfx::H - 1 - p.y)];
    const bool okc = (c == gfx::rgb(p.r, p.g, p.b));
    const bool okp = (got == c);
    all = all && okc && okp;
    Serial.printf("  (%3d,%3d) color565=%04X gfx::rgb=%04X %s | fb[x*320+319-y]=%04X %s\n", p.x, p.y, c,
                  gfx::rgb(p.r, p.g, p.b), okc ? "ok" : "MISMATCH", got, okp ? "ok" : "MISMATCH");
  }
  Serial.println(all ? "selftest PASS" : "selftest FAIL");
}

void exec(char *line) {
  // split "cmd arg..." and lower-case the command
  while (*line == ' ') line++;
  char *arg = line;
  while (*arg && *arg != ' ') {
    *arg = (char)tolower((unsigned char)*arg);
    arg++;
  }
  if (*arg) *arg++ = 0;
  while (*arg == ' ') arg++;
  size_t al = strlen(arg);
  while (al && arg[al - 1] == ' ') arg[--al] = 0;
  const char *cmd = line;
  if (!*cmd) return;

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    printHelp();
    ok(cmd);
  } else if (!strcmp(cmd, "ping")) {
    ok(cmd);
  } else if (!strcmp(cmd, "shot")) {
    if (!gfx::framebuffer()) err("no display");
    else s_shotPending = true;  // served by frameRendered()
  } else if (!strcmp(cmd, "fps")) {
    if (!strcmp(arg, "reset")) {
      s_n = s_idx = 0;
    } else {
      printFps();
      printFlush();
    }
    ok(cmd);
  } else if (!strcmp(cmd, "tap")) {
    long v[2];
    if (parseInts(arg, v, 2) != 2) return err("usage: tap <x> <y>");
    startInjection(v[0], v[1], v[0], v[1], 80, "tap");
  } else if (!strcmp(cmd, "hold")) {
    long v[3];
    if (parseInts(arg, v, 3) != 3 || v[2] < 0 || v[2] > 20000) return err("usage: hold <x> <y> <ms>");
    startInjection(v[0], v[1], v[0], v[1], (uint32_t)v[2], "hold");
  } else if (!strcmp(cmd, "swipe")) {
    // y = 190 avoids hold-to-repeat / slider targets of the current UI
    int y = 190;
    char dir[8] = "";
    const char *sp = strchr(arg, ' ');
    if (sp) {
      long v[1];
      if (parseInts(sp, v, 1) != 1) return err("usage: swipe <left|right> [y]");
      y = (int)v[0];
      strlcpy(dir, arg, min((size_t)(sp - arg + 1), sizeof(dir)));
    } else {
      strlcpy(dir, arg, sizeof(dir));
    }
    if (!strcmp(dir, "left")) startInjection(400, y, 80, y, 300, "swipe");
    else if (!strcmp(dir, "right")) startInjection(80, y, 400, y, 300, "swipe");
    else return err("usage: swipe <left|right> [y]");
  } else if (!strcmp(cmd, "speed")) {
    float v;
    if (!strcmp(arg, "off")) s_speedOn = false;
    else if (parseFloat(arg, v) && v >= 0.0f && v < 400.0f) { s_speedOn = true; s_speed = v; }
    else return err("usage: speed <kmh|off>");
    ok(cmd);
  } else if (!strcmp(cmd, "accel")) {
    float v;
    if (!strcmp(arg, "off")) s_accelOn = false;
    else if (parseFloat(arg, v) && v > -5.0f && v < 5.0f) { s_accelOn = true; s_accel = v; }
    else return err("usage: accel <g|off>");
    ok(cmd);
  } else if (!strcmp(cmd, "gfxtest")) {
    if (!strcmp(arg, "off")) s_testPage = -1;
    else if (!*arg) s_testPage = 0;
    else {
      long v[1];
      if (parseInts(arg, v, 1) != 1 || v[0] < 0 || v[0] >= gfxtest::PAGES) return err("usage: gfxtest [0-3|off]");
      s_testPage = (int)v[0];
    }
    ok(cmd);
  } else if (!strcmp(cmd, "bench")) {
    if (!gfx::framebuffer()) return err("no display");
    gfxtest::bench(Serial);
    ok(cmd);
  } else if (!strcmp(cmd, "flush")) {
    if (!strcmp(arg, "lib")) lcd::setFast(false);
    else if (!strcmp(arg, "fast")) lcd::setFast(true);
    else if (*arg) return err("usage: flush [fast|lib]");
    printFlush();
    ok(cmd);
  } else if (!strcmp(cmd, "selftest")) {
    selftest();
    ok(cmd);
  } else if (s_app) {
    const AppResult r = s_app(cmd, arg, Serial);
    if (r == APP_OK) {
      if (strcmp(cmd, "state") != 0) s_testPage = -1;
      ok(cmd);
    } else if (r == APP_BAD_ARG) {
      err("bad argument for", cmd);
    } else {
      err("unknown command", cmd);
    }
  } else {
    err("unknown command", cmd);
  }
}

}  // namespace

void begin(AppCommandFn app, Arduino_GFX *canvas) {
  s_app = app;
  s_canvas = canvas;
}

void poll() {
  // Bounded work per frame; garbage bytes are dropped.
  for (int budget = 256; budget > 0 && Serial.available() > 0; budget--) {
    const int ch = Serial.read();
    if (ch < 0) break;
    if (ch == '\n' || ch == '\r') {
      if (s_overflow) err("line too long");
      else if (s_len) {
        s_line[s_len] = 0;
        exec(s_line);
      }
      s_len = 0;
      s_overflow = false;
    } else if (ch >= 0x20 && ch < 0x7F) {
      if (s_len < sizeof(s_line) - 1) s_line[s_len++] = (char)ch;
      else s_overflow = true;
    }
  }
}

bool touchOverride(uint16_t &x, uint16_t &y, bool &touched) {
  if (s_inj.kind == INJ_NONE) return false;
  const uint32_t now = millis();
  if (!s_inj.started) {
    s_inj.started = true;
    s_inj.t0 = now;
  }
  const uint32_t el = now - s_inj.t0;
  if (s_inj.kind == INJ_PRESS) {
    if (el < s_inj.dur || s_inj.dur == 0) {
      const float t = s_inj.dur ? (float)el / (float)s_inj.dur : 1.0f;
      x = (uint16_t)lroundf(s_inj.x0 + (s_inj.x1 - s_inj.x0) * t);
      y = (uint16_t)lroundf(s_inj.y0 + (s_inj.y1 - s_inj.y0) * t);
      touched = true;
      if (s_inj.dur == 0) s_inj.kind = INJ_RELEASE;
      return true;
    }
    s_inj.kind = INJ_RELEASE;
  }
  // one frame with the finger lifted, then hand control back to the panel
  x = (uint16_t)s_inj.x1;
  y = (uint16_t)s_inj.y1;
  touched = false;
  s_inj.kind = INJ_NONE;
  ok(s_inj.reply);
  return true;
}

bool speedOverride(float &kmh) {
  if (s_speedOn) kmh = s_speed;
  return s_speedOn;
}

bool accelOverride(float &g) {
  if (s_accelOn) g = s_accel;
  return s_accelOn;
}

bool render() {
  if (s_testPage < 0 || !gfx::framebuffer()) return false;
  gfxtest::draw(s_testPage);
  return true;
}

void frameRendered(uint32_t draw_us) {
  s_curDraw = draw_us;
  if (!s_shotPending) return;
  s_shotPending = false;
  const uint16_t *fb = gfx::framebuffer();
  if (!fb) {
    err("no display");
    return;
  }
  static uint8_t row[gfx::W * 2];
  Serial.printf("SHOT %d %d\n", gfx::W, gfx::H);
  for (int y = 0; y < gfx::H; y++) {
    for (int x = 0; x < gfx::W; x++) {
      const uint16_t v = fb[x * gfx::H + (gfx::H - 1 - y)];
      row[2 * x] = (uint8_t)v;
      row[2 * x + 1] = (uint8_t)(v >> 8);
    }
    Serial.write(row, sizeof(row));
  }
  Serial.print("\nEND\n");
  ok("shot");
  s_lastFrameUs = 0;  // the dump stalls this frame: don't count its period
}

void frameFlushed(uint32_t flush_us) {
  const int64_t now = esp_timer_get_time();
  if (s_lastFrameUs) {
    s_draw[s_idx] = s_curDraw;
    s_flush[s_idx] = flush_us;
    s_period[s_idx] = (uint32_t)(now - s_lastFrameUs);
    s_idx = (s_idx + 1) % N;
    if (s_n < N) s_n++;
  }
  s_lastFrameUs = now;
}

}  // namespace dbg
