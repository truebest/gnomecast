#ifndef GNOMECAST_WEBOS_PLATFORM_H
#define GNOMECAST_WEBOS_PLATFORM_H

#include <stdbool.h>

#define NATIVE_WEBOS_SDK_VERSION_CAP 32

typedef struct NativeWebosPlatformInfo {
    char sdk_version[NATIVE_WEBOS_SDK_VERSION_CAP];
    unsigned int sdk_major;
} NativeWebosPlatformInfo;

/* One bounded LS2 query on webOS; host builds return unknown immediately. */
bool native_webos_platform_query(NativeWebosPlatformInfo *out);

/* Pure parser for getSystemInfo replies. */
bool native_webos_platform_parse_system_info(const char *json,
                                             NativeWebosPlatformInfo *out);

/* Marketing label for logging, or "future/unqualified"/"unknown". */
const char *native_webos_tv_release(unsigned int sdk_major);

#endif
