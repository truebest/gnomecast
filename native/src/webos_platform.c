#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "webos_platform.h"

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#if defined(HELLOLG_WITH_LS2) && HELLOLG_WITH_LS2
#include <glib.h>
#include <luna-service2/lunaservice.h>
#endif

#include "clog.h"

clog_define(g_native_log_platform, cLogLevelInfo, cLogFlags_Default, "platform.webos", NULL);

static const char *native_json_value(const char *json, const char *quoted_key) {
    const char *field = strstr(json, quoted_key);
    if (!field) {
        return NULL;
    }
    field += strlen(quoted_key);
    while (isspace((unsigned char)*field)) {
        field++;
    }
    if (*field++ != ':') {
        return NULL;
    }
    while (isspace((unsigned char)*field)) {
        field++;
    }
    return field;
}

static unsigned int native_sdk_major(const char *version) {
    if (!version || *version < '0' || *version > '9') {
        return 0;
    }
    unsigned int value = 0;
    do {
        unsigned int digit = (unsigned int)(*version - '0');
        if (value > (UINT_MAX - digit) / 10u) {
            return 0;
        }
        value = value * 10u + digit;
        version++;
    } while (*version >= '0' && *version <= '9');
    if (*version != '\0' && *version != '.') {
        return 0;
    }
    return value;
}

bool native_webos_platform_parse_system_info(const char *json,
                                             NativeWebosPlatformInfo *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!json) {
        return false;
    }

    const char *success = native_json_value(json, "\"returnValue\"");
    if (!success || strncmp(success, "true", 4) != 0 ||
        (success[4] != '\0' && success[4] != ',' && success[4] != '}' &&
         !isspace((unsigned char)success[4]))) {
        return false;
    }
    const char *version = native_json_value(json, "\"sdkVersion\"");
    if (!version || *version++ != '"') {
        return false;
    }
    const char *end = version;
    while (*end && *end != '"') {
        if (*end == '\\' || (unsigned char)*end < 0x20) {
            return false;
        }
        end++;
    }
    size_t length = (size_t)(end - version);
    if (*end != '"' || length == 0 || length >= sizeof(out->sdk_version)) {
        return false;
    }
    memcpy(out->sdk_version, version, length);
    out->sdk_version[length] = '\0';
    out->sdk_major = native_sdk_major(out->sdk_version);
    if (out->sdk_major == 0) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

const char *native_webos_tv_release(unsigned int sdk_major) {
    switch (sdk_major) {
        case 5: return "webOS TV 5.0";
        case 6: return "webOS TV 6.0";
        case 7: return "webOS TV 22";
        case 8: return "webOS TV 23";
        case 9: return "webOS TV 24";
        case 10: return "webOS TV 25";
        case 11: return "webOS TV 26";
        default: return sdk_major > 11 ? "future/unqualified" : "unknown";
    }
}

#if defined(HELLOLG_WITH_LS2) && HELLOLG_WITH_LS2

#define NATIVE_WEBOS_PLATFORM_TIMEOUT_MS 2000u

typedef struct NativeWebosPlatformQuery {
    GMainLoop *loop;
    NativeWebosPlatformInfo *out;
    bool complete;
    bool success;
} NativeWebosPlatformQuery;

static bool native_webos_platform_reply(LSHandle *handle, LSMessage *message, void *userdata) {
    (void)handle;
    NativeWebosPlatformQuery *query = (NativeWebosPlatformQuery *)userdata;
    const char *payload = message ? LSMessageGetPayload(message) : NULL;
    query->success = native_webos_platform_parse_system_info(payload, query->out);
    query->complete = true;
    g_main_loop_quit(query->loop);
    return true;
}

static gboolean native_webos_platform_timeout(gpointer userdata) {
    NativeWebosPlatformQuery *query = (NativeWebosPlatformQuery *)userdata;
    query->complete = false;
    g_main_loop_quit(query->loop);
    return G_SOURCE_REMOVE;
}

bool native_webos_platform_query(NativeWebosPlatformInfo *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    bool result = false;
    LSHandle *handle = NULL;
    LSMessageToken token = LSMESSAGE_TOKEN_INVALID;
    GMainContext *context = g_main_context_new();
    if (!context) {
        return false;
    }
    g_main_context_push_thread_default(context);
    GMainLoop *loop = g_main_loop_new(context, FALSE);
    if (!loop) {
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
        return false;
    }

    LSError error;
    LSErrorInit(&error);
    if (!LSRegister(NULL, &handle, &error)) {
        clog(cLogLevelWarning, "platform LSRegister failed: %s",
             error.message ? error.message : "?");
        LSErrorFree(&error);
        goto cleanup;
    }
    if (!LSGmainAttach(handle, loop, &error)) {
        clog(cLogLevelWarning, "platform LSGmainAttach failed: %s",
             error.message ? error.message : "?");
        LSErrorFree(&error);
        goto cleanup;
    }

    NativeWebosPlatformQuery query = {
        .loop = loop,
        .out = out,
    };
    if (!LSCallOneReply(handle,
                        "luna://com.webos.service.tv.systemproperty/getSystemInfo",
                        "{\"keys\":[\"sdkVersion\"]}", native_webos_platform_reply,
                        &query, &token, &error)) {
        clog(cLogLevelWarning, "getSystemInfo call failed: %s",
             error.message ? error.message : "?");
        LSErrorFree(&error);
        goto cleanup;
    }

    GSource *timeout = g_timeout_source_new(NATIVE_WEBOS_PLATFORM_TIMEOUT_MS);
    g_source_set_callback(timeout, native_webos_platform_timeout, &query, NULL);
    g_source_attach(timeout, context);
    g_main_loop_run(loop);
    g_source_destroy(timeout);
    g_source_unref(timeout);

    if (!query.complete) {
        LSErrorInit(&error);
        if (!LSCallCancel(handle, token, &error)) {
            clog(cLogLevelDebug, "getSystemInfo cancel failed: %s",
                 error.message ? error.message : "?");
            LSErrorFree(&error);
        }
        clog(cLogLevelWarning, "getSystemInfo timed out after %u ms",
             NATIVE_WEBOS_PLATFORM_TIMEOUT_MS);
    }
    result = query.success;

cleanup:
    if (handle) {
        LSErrorInit(&error);
        (void)LSUnregister(handle, &error);
        LSErrorFree(&error);
    }
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    if (result) {
        clog(cLogLevelNotice, "sdkVersion=%s platform=%s", out->sdk_version,
             native_webos_tv_release(out->sdk_major));
    } else {
        memset(out, 0, sizeof(*out));
    }
    return result;
}

#else

bool native_webos_platform_query(NativeWebosPlatformInfo *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    clog(cLogLevelDebug, "webOS platform query unavailable on this build");
    return false;
}

#endif
