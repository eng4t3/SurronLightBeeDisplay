// Hooks the dashboard (Main.ino) provides to other modules such as the web
// interface. Implemented in Main.ino. All functions are safe to call from any
// task.
#ifndef WHEELIE_APP_BRIDGE_H
#define WHEELIE_APP_BRIDGE_H

#include <Arduino.h>

struct DashSnapshot {
  float speed_kmh;
  float max_speed_kmh;
  double odo_km;
  double trip_km;
  uint32_t ride_seconds;
  float best_0_50_s;
  bool imperial;
  float speed_cal;
  bool filter_oem;
  int brightness;          // 20..255
  uint32_t uptime_s;
};

void appGetSnapshot(DashSnapshot &out);

// False while the bike is moving; the updater refuses to flash then.
bool appIsSafeToUpdate();

// Persist odometer/settings before the updater reboots the device.
void appPrepareForRestart();

#endif
