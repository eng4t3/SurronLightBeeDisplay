// WiFi access point + web interface for manual firmware updates (OTA).
// Implemented in web_ota.cpp. Everything here is safe to call from any task.
#ifndef WHEELIE_WEB_OTA_H
#define WHEELIE_WEB_OTA_H

#include <Arduino.h>

enum WebOtaState : uint8_t {
  WEBOTA_OFF = 0,     // WiFi and server stopped
  WEBOTA_READY,       // Access point up, waiting for an upload
  WEBOTA_UPLOADING,   // Receiving and flashing a firmware image
  WEBOTA_SUCCESS,     // Image verified; device reboots in a moment
  WEBOTA_ERROR        // Last upload failed (see error); still accepting uploads
};

struct WebOtaStatus {
  WebOtaState state;
  uint8_t progress_pct;    // 0..100 while uploading
  uint32_t bytes_written;
  uint32_t bytes_total;    // 0 when the browser did not send a length
  uint8_t clients;         // stations connected to the access point
  char ssid[33];
  char password[65];
  char ip[16];
  char error[64];
};

void webOtaBegin();                       // start AP + HTTP server (non-blocking)
void webOtaEnd();                         // stop server and turn WiFi off
bool webOtaIsOn();
void webOtaGetStatus(WebOtaStatus &out);  // consistent snapshot

#endif
