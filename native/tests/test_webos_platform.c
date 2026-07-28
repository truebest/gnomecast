#include "webos_platform.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_version(const char *json, const char *version, unsigned int major) {
    NativeWebosPlatformInfo info;
    assert(native_webos_platform_parse_system_info(json, &info));
    assert(strcmp(info.sdk_version, version) == 0);
    assert(info.sdk_major == major);
}

int main(void) {
    expect_version("{\"returnValue\":true,\"sdkVersion\":\"05.00.00\"}", "05.00.00", 5);
    expect_version("{ \"sdkVersion\" : \"8.3.1\", \"returnValue\" : true }", "8.3.1", 8);
    for (unsigned int major = 5; major <= 12; major++) {
        char json[96];
        snprintf(json, sizeof(json),
                 "{\"returnValue\":true,\"sdkVersion\":\"%u.0.0\"}", major);
        NativeWebosPlatformInfo info;
        assert(native_webos_platform_parse_system_info(json, &info));
        assert(info.sdk_major == major);
    }

    NativeWebosPlatformInfo info;
    assert(!native_webos_platform_parse_system_info(
        "{\"returnValue\":false,\"sdkVersion\":\"8.0.0\"}", &info));
    assert(!native_webos_platform_parse_system_info("{\"returnValue\":true}", &info));
    assert(!native_webos_platform_parse_system_info(
        "{\"returnValue\":true,\"sdkVersionExtra\":\"8.0.0\"}", &info));
    assert(!native_webos_platform_parse_system_info(
        "{\"returnValue\":true,\"sdkVersion\":8}", &info));
    assert(!native_webos_platform_parse_system_info(
        "{\"returnValue\":true,\"sdkVersion\":\"not-a-version\"}", &info));
    assert(!native_webos_platform_parse_system_info(
        "{\"returnValue\":true,\"sdkVersion\":\"8x\"}", &info));
    assert(!native_webos_platform_parse_system_info(
        "{\"returnValue\":trueish,\"sdkVersion\":\"8.0.0\"}", &info));
    assert(!native_webos_platform_parse_system_info(
        "{\"returnValue\":true,\"sdkVersion\":\"99999999999999999999\"}", &info));
    assert(!native_webos_platform_parse_system_info(NULL, &info));
    assert(!native_webos_platform_parse_system_info("{}", NULL));

    assert(strcmp(native_webos_tv_release(5), "webOS TV 5.0") == 0);
    assert(strcmp(native_webos_tv_release(8), "webOS TV 23") == 0);
    assert(strcmp(native_webos_tv_release(11), "webOS TV 26") == 0);
    assert(strcmp(native_webos_tv_release(12), "future/unqualified") == 0);
    assert(strcmp(native_webos_tv_release(4), "unknown") == 0);

    memset(&info, 0xff, sizeof(info));
    assert(!native_webos_platform_query(&info));
    assert(info.sdk_version[0] == '\0' && info.sdk_major == 0);
    printf("webos-platform: OK\n");
    return 0;
}
