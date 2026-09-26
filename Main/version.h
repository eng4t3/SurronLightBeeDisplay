// Firmware identity. Bump FW_VERSION for every release you publish on GitHub;
// the web updater shows it so you can tell which build is running.
#ifndef WHEELIE_VERSION_H
#define WHEELIE_VERSION_H

#define FW_NAME       "WheelieAssist"
#define FW_VERSION    "4.2.0"
#define FW_BUILD_DATE __DATE__ " " __TIME__

// Where the phone downloads new firmware .bin files from.
#define FW_RELEASES_URL "https://github.com/eng4t3/SurronLightBeeDisplay/releases/latest"

#endif
