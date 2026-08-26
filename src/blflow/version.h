// version.h - single source of truth for the firmware version string.
//
// STRVERSION is injected by platformio.ini from the `custom_version` option, so
// the version lives in exactly one place (the .ini) and flows into the binary,
// the /api/info response and firmware/manifest.json (via merge_firmware.py).

#ifndef BLSF_VERSION_H
#define BLSF_VERSION_H

#ifndef STRVERSION
#define STRVERSION "0.0.0-dev"
#endif

#define FW_VERSION   STRVERSION
#define FW_BUILD     __DATE__ " " __TIME__
#define FW_NAME      "BLSmartFlow"

#endif  // BLSF_VERSION_H
