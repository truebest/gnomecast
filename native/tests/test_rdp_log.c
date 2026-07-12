#include "rdp_log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "clog.h"

typedef struct LogCapture {
    unsigned count;
    cLogLevel level;
    char category[64];
    char message[256];
} LogCapture;

static void capture_sink(const cLogEvent *event, void *context) {
    LogCapture *capture = (LogCapture *)context;
    capture->count++;
    capture->level = event->level;
    (void)snprintf(capture->category, sizeof(capture->category), "%s",
                   event->category ? event->category : "");
    (void)snprintf(capture->message, sizeof(capture->message), "%s",
                   event->message ? event->message : "");
}

int main(void) {
    LogCapture capture = {0};
    clog_set_sink(capture_sink, &capture);
    assert(clog_configure("*=off,rdp.rust=debug") == cLogConfigOK);

    assert(!native_rdp_log_is_enabled(RDP_LOG_TRACE));
    assert(native_rdp_log_is_enabled(RDP_LOG_DEBUG));
    assert(native_rdp_log_is_enabled(RDP_LOG_FATAL));

    native_rdp_log_emit(RDP_LOG_TRACE, "green", "webrdp.transport", "disabled");
    assert(capture.count == 0);

    native_rdp_log_emit(RDP_LOG_DEBUG, "green", "webrdp.transport", "connected");
    assert(capture.count == 1);
    assert(capture.level == cLogLevelDebug);
    assert(strcmp(capture.category, "rdp.rust") == 0);
    assert(strcmp(capture.message, "green [webrdp.transport]: connected") == 0);

    static const struct {
        RdpLogLevel rdp;
        cLogLevel clog;
    } levels[] = {
        {RDP_LOG_INFO, cLogLevelInfo},       {RDP_LOG_NOTICE, cLogLevelNotice},
        {RDP_LOG_WARNING, cLogLevelWarning}, {RDP_LOG_ERROR, cLogLevelError},
        {RDP_LOG_FATAL, cLogLevelFatal},
    };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        native_rdp_log_emit(levels[i].rdp, "green", "test", "level");
        assert(capture.count == i + 2);
        assert(capture.level == levels[i].clog);
    }

    native_rdp_log_emit(RDP_LOG_WARNING, NULL, NULL, NULL);
    assert(capture.count == 7);
    assert(capture.level == cLogLevelWarning);
    assert(strcmp(capture.message, "? [webrdp]: ") == 0);

    native_rdp_log_emit((RdpLogLevel)99, "red", "test", "bad level");
    assert(capture.count == 8);
    assert(capture.level == cLogLevelError);
    assert(strstr(capture.message, "invalid Rust log level 99") != NULL);

    clog_reset_sink();
    assert(clog_configure("") == cLogConfigOK);
    puts("PASS rdp-log");
    return 0;
}
