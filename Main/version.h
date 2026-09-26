// Firmware identity. Bump FW_VERSION for every release you publish on GitHub;
// the web updater shows it so you can tell which build is running.
#ifndef LBD_VERSION_H
#define LBD_VERSION_H

#define FW_NAME       "LightBee Display"
#define FW_VERSION    "4.2.1"
#define FW_BUILD_DATE __DATE__ " " __TIME__

// Where the phone downloads new firmware .bin files from.
#define FW_RELEASES_URL "https://github.com/eng4t3/SurronLightBeeDisplay/releases/latest"

#endif
