#include "rdp_log.h"

#include "clog.h"

clog_define(g_native_log_rdp, cLogLevelInfo, cLogFlags_Default, "rdp.rust", NULL);

static cLogLevel native_rdp_log_level(RdpLogLevel level) {
    switch (level) {
        case RDP_LOG_TRACE:
            return cLogLevelTrace;
        case RDP_LOG_DEBUG:
            return cLogLevelDebug;
        case RDP_LOG_INFO:
            return cLogLevelInfo;
        case RDP_LOG_NOTICE:
            return cLogLevelNotice;
        case RDP_LOG_WARNING:
            return cLogLevelWarning;
        case RDP_LOG_ERROR:
            return cLogLevelError;
        case RDP_LOG_FATAL:
            return cLogLevelFatal;
        default:
            return cLogLevelError;
    }
}

bool native_rdp_log_is_enabled(RdpLogLevel level) {
    return clog_is_enabled(native_rdp_log_level(level));
}

void native_rdp_log_emit(RdpLogLevel level, const char *session, const char *target,
                         const char *message) {
    cLogLevel mapped = native_rdp_log_level(level);
    const char *safe_session = session && session[0] ? session : "?";
    const char *safe_target = target && target[0] ? target : "webrdp";
    const char *safe_message = message ? message : "";

    if (level < RDP_LOG_TRACE || level > RDP_LOG_FATAL) {
        clog(cLogLevelError, "%s [%s]: invalid Rust log level %d: %s", safe_session,
             safe_target, (int)level, safe_message);
        return;
    }
    clog(mapped, "%s [%s]: %s", safe_session, safe_target, safe_message);
}
