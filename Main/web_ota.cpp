// WiFi access point + web interface for manual firmware updates (OTA).
//
// Threading model
//  - webOtaBegin() / webOtaEnd() / webOtaIsOn() / webOtaGetStatus() may be
//    called from any task (normally the dashboard UI task on core 1). They
//    never touch WiFi or flash themselves and return quickly.
//  - webOtaBegin() spawns one FreeRTOS task pinned to core 0. That task owns
//    WiFi, the DNS/mDNS responders, the synchronous WebServer and the Update
//    library, and tears all of it down again before deleting itself.
//  - Shared state (the WebOtaStatus snapshot and the lifecycle flags) is
//    guarded by a spinlock. Critical sections only copy a few bytes; the lock
//    is never held while talking to WiFi or writing flash.
#include "web_ota.h"
#include "app_bridge.h"
#include "version.h"
#include "web_page.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
static const IPAddress kApIp(192, 168, 4, 1);
static const IPAddress kApMask(255, 255, 255, 0);
static const char *const kApIpStr = "192.168.4.1";
static const char *const kMdnsHost = "wheelie";  // http://wheelie.local
static const char *const kNvsNs = "webota";
static const char *const kNvsPassKey = "pass";

static constexpr uint8_t kApChannel = 6;
static constexpr uint8_t kApMaxClients = 4;
static constexpr uint32_t kIdleOffMs = 10UL * 60UL * 1000UL;  // no station + no upload
static constexpr uint32_t kRestartDelayMs = 1500;             // let the HTTP reply get out
static constexpr uint32_t kTaskStack = 10 * 1024;             // bytes
static constexpr UBaseType_t kTaskPrio = 2;
static constexpr BaseType_t kTaskCore = 0;
static constexpr uint32_t kMinImageSize = 64 * 1024;          // anything smaller is not our app

// Tag embedded in every build so the web page can read the version of a .bin
// file before it is uploaded (it scans the file for "\x01WAFW|").
extern "C" const char g_webota_fw_tag[] __attribute__((used)) =
  "\x01WAFW|" FW_NAME "|" FW_VERSION "|" FW_BUILD_DATE "\x01";

// ---------------------------------------------------------------------------
// Shared state (guarded by s_mux)
// ---------------------------------------------------------------------------
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static WebOtaStatus s_st;       // what webOtaGetStatus() returns
static bool s_alive = false;    // server task exists
static bool s_stopReq = false;  // webOtaEnd() asked the task to shut down

#define LOCK() portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)

static void stSetState(WebOtaState st) {
  LOCK();
  s_st.state = st;
  UNLOCK();
}

static void stSetError(const char *msg) {
  LOCK();
  s_st.state = WEBOTA_ERROR;
  strlcpy(s_st.error, msg ? msg : "", sizeof(s_st.error));
  UNLOCK();
}

static void stSetProgress(uint32_t written, uint32_t total) {
  uint8_t pct = 0;
  if (total > 0) {
    uint64_t p = (uint64_t)written * 100ULL / total;
    pct = p > 100 ? 100 : (uint8_t)p;
  }
  LOCK();
  s_st.bytes_written = written;
  s_st.bytes_total = total;
  s_st.progress_pct = pct;
  UNLOCK();
}

static bool stopRequested() {
  LOCK();
  bool r = s_stopReq;
  UNLOCK();
  return r;
}

// ---------------------------------------------------------------------------
// Task-private state (only touched by the server task)
// ---------------------------------------------------------------------------
static WebServer *s_server = nullptr;
static DNSServer *s_dns = nullptr;
static bool s_mdns = false;
static uint32_t s_restartAt = 0;     // millis() deadline, 0 = none
static uint32_t s_lastActivity = 0;  // for the idle auto-off
static uint32_t s_sketchSize = 0;    // cached (computing it hashes the image)

struct UploadCtx {
  bool started;      // UPLOAD_FILE_START seen for this request
  bool begun;        // Update.begin() succeeded
  bool done;         // Update.end() succeeded
  bool failed;       // request rejected / flashing failed
  int httpCode;      // reply code when failed
  char err[64];
  uint32_t expected; // size announced by the browser (0 = unknown)
  uint32_t written;
  uint32_t chunks;
  char md5[33];
};
static UploadCtx s_up;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static const char *chipName(uint16_t id) {
  switch (id) {
    case ESP_CHIP_ID_ESP32: return "ESP32";
    case ESP_CHIP_ID_ESP32S2: return "ESP32-S2";
    case ESP_CHIP_ID_ESP32C3: return "ESP32-C3";
    case ESP_CHIP_ID_ESP32S3: return "ESP32-S3";
    case ESP_CHIP_ID_ESP32C2: return "ESP32-C2";
    case ESP_CHIP_ID_ESP32C6: return "ESP32-C6";
    case ESP_CHIP_ID_ESP32H2: return "ESP32-H2";
    default: return "another chip";
  }
}

// Copies src into dst as JSON string content (no surrounding quotes).
static void jsonEsc(char *dst, size_t cap, const char *src) {
  size_t o = 0;
  for (; src && *src && o + 7 < cap; ++src) {
    unsigned char c = (unsigned char)*src;
    if (c == '"' || c == '\\') {
      dst[o++] = '\\';
      dst[o++] = (char)c;
    } else if (c < 0x20) {
      o += snprintf(dst + o, cap - o, "\\u%04x", c);
    } else {
      dst[o++] = (char)c;
    }
  }
  dst[o] = 0;
}

static float finiteOr0(float v) {
  return isfinite(v) ? v : 0.0f;
}
static double finiteOr0(double v) {
  return isfinite(v) ? v : 0.0;
}

static bool isHexMd5(const String &s) {
  if (s.length() != 32) return false;
  for (size_t i = 0; i < 32; ++i) {
    if (!isxdigit((unsigned char)s[i])) return false;
  }
  return true;
}

// SSID from the soft-AP MAC; password generated once and kept in NVS so the
// QR code on the dashboard never changes.
static void makeCredentials(char *ssid, size_t ssidCap, char *pass, size_t passCap) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(ssid, ssidCap, "WheelieAssist-%02X%02X", mac[4], mac[5]);

  Preferences prefs;
  pass[0] = 0;
  if (prefs.begin(kNvsNs, true)) {
    String p = prefs.getString(kNvsPassKey, "");
    prefs.end();
    if (p.length() >= 8 && p.length() < passCap) strlcpy(pass, p.c_str(), passCap);
  }
  if (pass[0]) return;

  // Unambiguous alphabet: no 0/o, 1/l/i. Lower case is quick to type on a phone.
  static const char kAlpha[] = "abcdefghjkmnpqrstuvwxyz23456789";
  const uint32_t n = sizeof(kAlpha) - 1;
  const uint32_t limit = 0xFFFFFFFFu - (0xFFFFFFFFu % n);  // reject to avoid modulo bias
  size_t len = 0;
  while (len < 10 && len + 1 < passCap) {
    // esp_random() is only fully random with RF on; mix in the microsecond
    // timer and cycle counter (the moment the rider taps the button).
    uint32_t r = esp_random() ^ (uint32_t)(esp_timer_get_time() * 2654435761ULL) ^ (uint32_t)ESP.getCycleCount();
    if (r >= limit) continue;
    pass[len++] = kAlpha[r % n];
  }
  pass[len] = 0;

  if (prefs.begin(kNvsNs, false)) {
    prefs.putString(kNvsPassKey, pass);
    prefs.end();
  }
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
static void sendJson(int code, const char *body) {
  s_server->sendHeader("Cache-Control", "no-store");
  s_server->send(code, "application/json", body);
}

static void sendJsonError(int code, const char *msg) {
  char esc[128];
  char body[160];
  jsonEsc(esc, sizeof(esc), msg);
  snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", esc);
  sendJson(code, body);
}

// State-changing requests must carry this header. Browsers only allow it on
// same-origin XHR/fetch (anything else needs a CORS preflight we never
// answer), so a random page can't trigger an update or reboot.
static bool requireAppHeader() {
  if (s_server->header("X-Requested-With") == "WheelieAssist") return true;
  sendJsonError(403, "Missing request header");
  return false;
}

static void redirectToPortal() {
  s_server->sendHeader("Location", String("http://") + kApIpStr + "/");
  s_server->sendHeader("Cache-Control", "no-store");
  s_server->send(302, "text/plain", "");
}

static bool hostIsUs() {
  String h = s_server->hostHeader();
  int colon = h.indexOf(':');
  if (colon >= 0) h = h.substring(0, colon);
  h.toLowerCase();
  return h.length() == 0 || h == kApIpStr || h == "wheelie.local" || h == "wheelie";
}

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------
static void handleRoot() {
  if (!hostIsUs()) {  // captive portal: some OSes request "/" on a foreign host
    redirectToPortal();
    return;
  }
  s_server->sendHeader("Cache-Control", "no-cache");
  s_server->send_P(200, "text/html; charset=utf-8", WEB_PAGE, sizeof(WEB_PAGE) - 1);
}

static void handleInfo() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  if (s_sketchSize == 0) s_sketchSize = ESP.getSketchSize();

  WebOtaStatus st;
  webOtaGetStatus(st);

  char body[768];
  snprintf(body, sizeof(body),
           "{\"name\":\"%s\",\"version\":\"%s\",\"build\":\"%s\",\"releases\":\"%s\","
           "\"chip\":\"%s\",\"rev\":\"v%u.%u\",\"cores\":%u,\"mhz\":%u,"
           "\"flash\":%lu,\"sketch\":%lu,\"heap\":%lu,\"heap_min\":%lu,"
           "\"psram\":%lu,\"psram_total\":%lu,"
           "\"running\":\"%s\",\"next\":\"%s\",\"next_size\":%lu,"
           "\"uptime\":%lu,\"ssid\":\"%s\",\"ip\":\"%s\",\"clients\":%u,\"rssi\":null,"
           "\"idf\":\"%s\",\"safe\":%s,\"state\":%u}",
           FW_NAME, FW_VERSION, FW_BUILD_DATE, FW_RELEASES_URL,
           ESP.getChipModel(), (unsigned)(ci.revision / 100), (unsigned)(ci.revision % 100),
           (unsigned)ci.cores, (unsigned)ESP.getCpuFreqMHz(),
           (unsigned long)ESP.getFlashChipSize(), (unsigned long)s_sketchSize,
           (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
           (unsigned long)ESP.getFreePsram(), (unsigned long)ESP.getPsramSize(),
           run ? run->label : "?", next ? next->label : "none",
           (unsigned long)(next ? next->size : 0),
           (unsigned long)(esp_timer_get_time() / 1000000ULL), st.ssid, st.ip,
           (unsigned)st.clients, esp_get_idf_version(),
           appIsSafeToUpdate() ? "true" : "false", (unsigned)st.state);
  sendJson(200, body);
}

static void handleTelemetry() {
  DashSnapshot d;
  memset(&d, 0, sizeof(d));
  appGetSnapshot(d);
  char body[320];
  snprintf(body, sizeof(body),
           "{\"speed_kmh\":%.1f,\"max_speed_kmh\":%.1f,\"odo_km\":%.2f,\"trip_km\":%.2f,"
           "\"ride_s\":%lu,\"best_0_50_s\":%.2f,\"imperial\":%s,\"uptime_s\":%lu,\"safe\":%s}",
           finiteOr0(d.speed_kmh), finiteOr0(d.max_speed_kmh), finiteOr0(d.odo_km),
           finiteOr0(d.trip_km), (unsigned long)d.ride_seconds, finiteOr0(d.best_0_50_s),
           d.imperial ? "true" : "false", (unsigned long)d.uptime_s,
           appIsSafeToUpdate() ? "true" : "false");
  sendJson(200, body);
}

static void handleStatus() {
  WebOtaStatus st;
  webOtaGetStatus(st);
  char esc[96];
  jsonEsc(esc, sizeof(esc), st.error);
  char body[224];
  snprintf(body, sizeof(body),
           "{\"state\":%u,\"pct\":%u,\"written\":%lu,\"total\":%lu,\"clients\":%u,\"error\":\"%s\"}",
           (unsigned)st.state, (unsigned)st.progress_pct, (unsigned long)st.bytes_written,
           (unsigned long)st.bytes_total, (unsigned)st.clients, esc);
  sendJson(200, body);
}

// GET /api/precheck?size=N - lets the page fail fast before sending megabytes.
static void handlePrecheck() {
  uint32_t size = (uint32_t)strtoul(s_server->arg("size").c_str(), nullptr, 10);
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (s_restartAt) {
    sendJsonError(409, "An update was just installed; the dashboard is restarting.");
  } else if (!appIsSafeToUpdate()) {
    sendJsonError(409, "Bike is moving. Stop the bike to install an update.");
  } else if (!next) {
    sendJsonError(500, "No OTA partition. Flash once over USB with the OTA partition table.");
  } else if (size > next->size) {
    char m[96];
    snprintf(m, sizeof(m), "Firmware too big: %lu KB, slot holds %lu KB.",
             (unsigned long)(size / 1024), (unsigned long)(next->size / 1024));
    sendJsonError(413, m);
  } else if (size && size < kMinImageSize) {
    sendJsonError(400, "File is too small to be dashboard firmware.");
  } else {
    sendJson(200, "{\"ok\":true}");
  }
}

static void handleReboot() {
  if (!requireAppHeader()) return;
  WebOtaStatus st;
  webOtaGetStatus(st);
  if (st.state == WEBOTA_UPLOADING) {
    sendJsonError(409, "An update is in progress.");
    return;
  }
  if (!appIsSafeToUpdate()) {
    sendJsonError(409, "Bike is moving. Stop the bike to reboot.");
    return;
  }
  sendJson(200, "{\"ok\":true}");
  if (!s_restartAt) s_restartAt = millis() + 800;
}

static void handleNoContent() {
  s_server->send(204);
}

static void handleNotFound() {
  if (!hostIsUs()) {  // any foreign host -> captive portal page
    redirectToPortal();
    return;
  }
  sendJsonError(404, "Not found");
}

// ---------------------------------------------------------------------------
// Firmware upload: POST /update (multipart/form-data, one file field)
// Query: size=<bytes> (or X-FW-Size header), md5=<32 hex> (optional)
// ---------------------------------------------------------------------------
static void uploadFail(int code, const char *msg) {
  if (s_up.begun) Update.abort();
  s_up.begun = false;
  s_up.failed = true;
  s_up.httpCode = code;
  strlcpy(s_up.err, msg, sizeof(s_up.err));
  stSetError(msg);
}

static void onUploadStart(WebServer &srv, HTTPUpload &) {
  if (Update.isRunning()) Update.abort();  // leftover from a broken request
  memset(&s_up, 0, sizeof(s_up));
  s_up.started = true;

  WebOtaStatus st;
  webOtaGetStatus(st);
  if (s_restartAt || st.state == WEBOTA_SUCCESS) {
    s_up.failed = true;  // don't overwrite the SUCCESS state
    s_up.httpCode = 409;
    strlcpy(s_up.err, "Update already installed; restarting.", sizeof(s_up.err));
    return;
  }
  if (srv.header("X-Requested-With") != "WheelieAssist") {
    s_up.failed = true;
    s_up.httpCode = 403;
    strlcpy(s_up.err, "Missing request header", sizeof(s_up.err));
    return;
  }
  if (!appIsSafeToUpdate()) {
    uploadFail(409, "Bike is moving. Stop the bike to update.");
    return;
  }

  String sz = srv.arg("size");
  if (sz.length() == 0) sz = srv.header("X-FW-Size");
  s_up.expected = (uint32_t)strtoul(sz.c_str(), nullptr, 10);

  String md5 = srv.arg("md5");
  md5.trim();
  if (md5.length()) {
    if (!isHexMd5(md5)) {
      uploadFail(400, "MD5 must be 32 hexadecimal characters.");
      return;
    }
    md5.toLowerCase();
    strlcpy(s_up.md5, md5.c_str(), sizeof(s_up.md5));
  }

  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (!next) {
    uploadFail(500, "No OTA partition (wrong partition table).");
    return;
  }
  // Content-Length includes a few hundred bytes of multipart framing.
  uint32_t limit = next->size;
  if (s_up.expected > limit || (uint32_t)srv.clientContentLength() > limit + 4096) {
    uploadFail(413, "Firmware is too big for the update slot.");
    return;
  }

  s_lastActivity = millis();
  LOCK();
  s_st.state = WEBOTA_UPLOADING;
  s_st.error[0] = 0;
  s_st.bytes_written = 0;
  s_st.bytes_total = s_up.expected;
  s_st.progress_pct = 0;
  UNLOCK();
}

// Validates the image header in the first chunk, then starts Update.
static bool beginFlash(const uint8_t *buf, size_t len) {
  const size_t need = 0x30;  // image header + segment header + app-desc magic (first chunk is 1436 B)
  if (len < need) {
    uploadFail(400, "File is too small to be firmware.");
    return false;
  }
  esp_image_header_t hdr;
  memcpy(&hdr, buf, sizeof(hdr));
  if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
    uploadFail(400, "Not an ESP32 firmware image (bad magic byte).");
    return false;
  }
  if (hdr.chip_id != ESP_CHIP_ID_ESP32S3) {
    char m[64];
    snprintf(m, sizeof(m), "Firmware is built for %s, not ESP32-S3.", chipName(hdr.chip_id));
    uploadFail(400, m);
    return false;
  }
  uint32_t descMagic;
  memcpy(&descMagic, buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), 4);
  if (descMagic != ESP_APP_DESC_MAGIC_WORD) {
    uploadFail(400, "This is the -full.bin USB image; upload the plain .bin.");
    return false;
  }

  size_t size = s_up.expected ? s_up.expected : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(size, U_FLASH)) {
    char m[64];
    snprintf(m, sizeof(m), "Update begin failed: %s", Update.errorString());
    uploadFail(500, m);
    return false;
  }
  if (s_up.md5[0] && !Update.setMD5(s_up.md5)) {
    uploadFail(400, "Invalid MD5.");
    return false;
  }
  s_up.begun = true;
  return true;
}

static void onUploadWrite(WebServer &srv, HTTPUpload &up) {
  if (s_up.failed || !s_up.started || up.currentSize == 0) return;

  if (stopRequested()) {
    uploadFail(503, "Hotspot switched off during the upload.");
    srv.client().stop();  // makes the parser give up instead of reading on
    return;
  }
  if (!s_up.begun && !beginFlash(up.buf, up.currentSize)) return;

  // Flash write: no locks held here.
  size_t w = Update.write(up.buf, up.currentSize);
  if (w != up.currentSize) {
    char m[64];
    snprintf(m, sizeof(m), "Flash write failed: %s", Update.errorString());
    uploadFail(500, m);
    return;
  }
  s_up.written += w;
  s_lastActivity = millis();
  stSetProgress(s_up.written, s_up.expected);

  // A multi-megabyte upload can keep this loop busy for a while: let the
  // core-0 idle task run so the task watchdog stays happy.
  if ((++s_up.chunks & 7) == 0) vTaskDelay(1);
}

static void onUploadEnd(WebServer &, HTTPUpload &up) {
  if (s_up.failed || !s_up.started) return;
  if (!s_up.begun) {
    uploadFail(400, "Empty file.");
    return;
  }
  if (s_up.written < kMinImageSize) {
    uploadFail(400, "File is too small to be dashboard firmware.");
    return;
  }
  if (s_up.expected && s_up.written != s_up.expected) {
    uploadFail(400, "Upload incomplete (size mismatch).");
    return;
  }
  (void)up;
  // Update.end() checks the MD5 (if given) and has the bootloader code verify
  // the whole image (segments, checksum, SHA-256) before switching the boot slot.
  if (!Update.end(true)) {
    char m[64];
    snprintf(m, sizeof(m), "Verify failed: %s", Update.errorString());
    s_up.begun = false;  // end() already aborted
    s_up.failed = true;
    s_up.httpCode = 400;
    strlcpy(s_up.err, m, sizeof(s_up.err));
    stSetError(m);
    return;
  }
  s_up.begun = false;
  s_up.done = true;
  stSetProgress(s_up.written, s_up.written);
  stSetState(WEBOTA_SUCCESS);
  s_restartAt = millis() + kRestartDelayMs;  // even if the reply never arrives
}

static void onUploadAborted() {
  if (s_up.done) return;  // image already committed; the restart stays scheduled
  if (s_up.begun) Update.abort();
  s_up.begun = false;
  if (s_up.started && !s_up.failed) stSetError("Upload aborted (connection lost).");
  memset(&s_up, 0, sizeof(s_up));
}

static void onUploadDone(WebServer &) {
  if (s_up.done) {
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"size\":%lu,\"version\":\"%s\"}",
             (unsigned long)s_up.written, FW_VERSION);
    sendJson(200, body);
  } else if (s_up.failed) {
    sendJsonError(s_up.httpCode ? s_up.httpCode : 500, s_up.err);
  } else {
    sendJsonError(400, "No firmware file received.");
  }
  memset(&s_up, 0, sizeof(s_up));
}

// Custom handler so raw (non-multipart) bodies are never routed to the upload
// callback (the stock handler would hand them to it with no HTTPUpload object).
class OtaUploadHandler : public RequestHandler {
public:
  bool canHandle(WebServer &, HTTPMethod m, const String &uri) override {
    return m == HTTP_POST && uri == "/update";
  }
  bool canUpload(WebServer &, const String &uri) override {
    return uri == "/update";
  }
  bool canRaw(WebServer &, const String &) override {
    return false;
  }
  bool handle(WebServer &srv, HTTPMethod m, const String &uri) override {
    if (!canHandle(srv, m, uri)) return false;
    onUploadDone(srv);
    return true;
  }
  void upload(WebServer &srv, const String &, HTTPUpload &up) override {
    switch (up.status) {
      case UPLOAD_FILE_START: onUploadStart(srv, up); break;
      case UPLOAD_FILE_WRITE: onUploadWrite(srv, up); break;
      case UPLOAD_FILE_END: onUploadEnd(srv, up); break;
      case UPLOAD_FILE_ABORTED: onUploadAborted(); break;
    }
  }
};

// ---------------------------------------------------------------------------
// Network bring-up / tear-down (server task only)
// ---------------------------------------------------------------------------
static bool startNetwork(const char *ssid, const char *pass) {
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_AP)) return false;
  if (!WiFi.softAP(ssid, pass, kApChannel, 0, kApMaxClients)) return false;
  WiFi.softAPConfig(kApIp, kApIp, kApMask, IPAddress((uint32_t)0), kApIp);
  esp_wifi_set_ps(WIFI_PS_NONE);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
  WiFi.AP.enableDhcpCaptivePortal();  // DHCP option 114 -> phones open the page
#endif

  s_dns = new DNSServer();
  s_dns->setErrorReplyCode(DNSReplyCode::NoError);
  s_dns->setTTL(60);
  s_dns->start(53, "*", kApIp);  // every name resolves to us

  s_mdns = MDNS.begin(kMdnsHost);
  if (s_mdns) MDNS.addService("http", "tcp", 80);

  s_server = new WebServer(80);
  static const char *hdrs[] = {"X-Requested-With", "X-FW-Size"};
  s_server->collectHeaders(hdrs, sizeof(hdrs) / sizeof(hdrs[0]));
  s_server->enableDelay(false);  // the task loop already sleeps

  s_server->on("/", HTTP_GET, handleRoot);
  s_server->on("/index.html", HTTP_GET, handleRoot);
  s_server->on("/api/info", HTTP_GET, handleInfo);
  s_server->on("/api/telemetry", HTTP_GET, handleTelemetry);
  s_server->on("/api/status", HTTP_GET, handleStatus);
  s_server->on("/api/precheck", HTTP_GET, handlePrecheck);
  s_server->on("/api/reboot", HTTP_POST, handleReboot);
  s_server->on("/favicon.ico", HTTP_GET, handleNoContent);
  s_server->addHandler(new OtaUploadHandler());  // owned (deleted) by the server
  // Captive-portal probes (Android, iOS/macOS, Windows, Firefox, Kindle...).
  static const char *probes[] = {
    "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
    "/connecttest.txt", "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt",
    "/fwlink", "/mobile/status.php", "/check_network_status.txt", "/kindle-wifi/wifistub.html",
  };
  for (const char *p : probes) s_server->on(p, HTTP_ANY, redirectToPortal);
  s_server->onNotFound(handleNotFound);
  s_server->begin();
  return true;
}

static void stopNetwork() {
  if (s_server) {
    s_server->stop();
    delete s_server;
    s_server = nullptr;
  }
  if (s_mdns) {
    MDNS.end();
    s_mdns = false;
  }
  if (s_dns) {
    s_dns->stop();
    delete s_dns;
    s_dns = nullptr;
  }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

static void restartNow() {
  appPrepareForRestart();
  vTaskDelay(pdMS_TO_TICKS(100));
  ESP.restart();
}

static void serverTask(void *) {
  char ssid[33], pass[65];
  LOCK();
  strlcpy(ssid, s_st.ssid, sizeof(ssid));
  strlcpy(pass, s_st.password, sizeof(pass));
  UNLOCK();

  memset(&s_up, 0, sizeof(s_up));
  s_restartAt = 0;
  bool autoOff = false;
  const char *failMsg = nullptr;

  if (!startNetwork(ssid, pass)) {
    failMsg = "WiFi failed to start";
  } else {
    stSetState(WEBOTA_READY);
    s_lastActivity = millis();
    uint32_t lastPoll = 0;

    for (;;) {
      if (stopRequested()) break;
      s_server->handleClient();

      const uint32_t now = millis();
      if (s_restartAt && (int32_t)(now - s_restartAt) >= 0 && appIsSafeToUpdate()) restartNow();

      if (now - lastPoll >= 1000) {
        lastPoll = now;
        uint8_t n = WiFi.softAPgetStationNum();
        LOCK();
        s_st.clients = n;
        bool busy = s_st.state == WEBOTA_UPLOADING;
        UNLOCK();
        if (n > 0 || busy || s_restartAt) s_lastActivity = now;
        if (now - s_lastActivity >= kIdleOffMs) {
          autoOff = true;
          break;
        }
      }
      vTaskDelay(2);
    }
  }

  stopNetwork();

  // A committed image must be booted even if the hotspot was switched off
  // meanwhile; wait until the bike is at a standstill.
  if (s_restartAt) {
    while (!appIsSafeToUpdate()) vTaskDelay(pdMS_TO_TICKS(250));
    restartNow();
  }

  LOCK();
  s_st.state = WEBOTA_OFF;
  s_st.clients = 0;
  s_st.progress_pct = 0;
  if (failMsg) strlcpy(s_st.error, failMsg, sizeof(s_st.error));
  else if (autoOff) strlcpy(s_st.error, "Hotspot off after 10 min idle", sizeof(s_st.error));
  s_alive = false;
  s_stopReq = false;
  UNLOCK();
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void webOtaBegin() {
  // Reference the version tag so --gc-sections can't drop it from the image.
  const char *volatile keepTag = g_webota_fw_tag;
  (void)keepTag;

  LOCK();
  const bool running = s_alive && !s_stopReq;
  const bool stopping = s_alive && s_stopReq;
  UNLOCK();
  if (running) return;

  if (stopping) {  // quick off->on: wait for the previous session to wind down
    for (int i = 0; i < 300; ++i) {
      vTaskDelay(pdMS_TO_TICKS(10));
      LOCK();
      bool alive = s_alive;
      UNLOCK();
      if (!alive) break;
    }
    LOCK();
    bool alive = s_alive;
    UNLOCK();
    if (alive) return;
  }

  char ssid[33], pass[65];
  makeCredentials(ssid, sizeof(ssid), pass, sizeof(pass));

  LOCK();
  memset(&s_st, 0, sizeof(s_st));
  s_st.state = WEBOTA_OFF;  // becomes READY once the AP is up
  strlcpy(s_st.ssid, ssid, sizeof(s_st.ssid));
  strlcpy(s_st.password, pass, sizeof(s_st.password));
  strlcpy(s_st.ip, kApIpStr, sizeof(s_st.ip));
  s_alive = true;
  s_stopReq = false;
  UNLOCK();

  if (xTaskCreatePinnedToCore(serverTask, "webota", kTaskStack, nullptr, kTaskPrio, nullptr, kTaskCore) != pdPASS) {
    LOCK();
    s_alive = false;
    strlcpy(s_st.error, "Out of memory (task)", sizeof(s_st.error));
    UNLOCK();
  }
}

void webOtaEnd() {
  LOCK();
  if (s_alive) {
    s_stopReq = true;
    s_st.state = WEBOTA_OFF;
    s_st.clients = 0;
  }
  UNLOCK();
}

bool webOtaIsOn() {
  LOCK();
  bool on = s_alive && !s_stopReq;
  UNLOCK();
  return on;
}

void webOtaGetStatus(WebOtaStatus &out) {
  LOCK();
  memcpy(&out, &s_st, sizeof(out));
  UNLOCK();
}
