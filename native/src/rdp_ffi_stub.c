#include "rdp_ffi.h"

#include <stdlib.h>

#include "clog.h"

clog_define(g_native_log_rdp, cLogLevelInfo, cLogFlags_Default, "rdp.stub", NULL);

struct RdpSession {
    RdpCallbacks callbacks;
};

static void emit_state(const RdpCallbacks *callbacks, RdpState state, const char *detail) {
    if (callbacks && callbacks->on_state) {
        callbacks->on_state(callbacks->ctx, state, detail);
    }
}

RdpSession *rdp_session_start(const RdpConfig *config, const RdpCallbacks *callbacks) {
    if (!config || !config->host || !config->host[0]) {
        clog(cLogLevelError, "refusing to start RDP stub without a host");
        emit_state(callbacks, RDP_STATE_PROTOCOL_ERROR, "missing host");
        return NULL;
    }
    RdpSession *session = (RdpSession *)calloc(1, sizeof(RdpSession));
    if (!session) {
        clog(cLogLevelError, "failed to allocate RDP stub session");
        emit_state(callbacks, RDP_STATE_NETWORK_ERROR, "alloc failed");
        return NULL;
    }
    if (callbacks) {
        session->callbacks = *callbacks;
    }
    emit_state(&session->callbacks, RDP_STATE_CONNECTING, "native RDP FFI stub");
    clog(cLogLevelNotice, "started native RDP FFI stub session");
    return session;
}

void rdp_session_stop(RdpSession *session) {
    if (!session) {
        return;
    }
    emit_state(&session->callbacks, RDP_STATE_STOPPED, "stopped");
    clog(cLogLevelDebug, "stopped native RDP FFI stub session");
    free(session);
}

void rdp_send_pointer_move(RdpSession *session, uint16_t x, uint16_t y) {
    (void)session;
    (void)x;
    (void)y;
}

void rdp_send_pointer_button(RdpSession *session, uint16_t x, uint16_t y, uint8_t button, bool down) {
    (void)session;
    (void)x;
    (void)y;
    (void)button;
    (void)down;
}

void rdp_send_pointer_wheel(RdpSession *session, uint16_t x, uint16_t y, int16_t delta) {
    (void)session;
    (void)x;
    (void)y;
    (void)delta;
}

void rdp_send_key(RdpSession *session, uint8_t scancode, bool down, bool extended) {
    (void)session;
    (void)scancode;
    (void)down;
    (void)extended;
}

void rdp_send_unicode(RdpSession *session, uint16_t codepoint, bool down) {
    (void)session;
    (void)codepoint;
    (void)down;
}

void rdp_send_sync(RdpSession *session, bool scroll_lock, bool num_lock, bool caps_lock) {
    (void)session;
    (void)scroll_lock;
    (void)num_lock;
    (void)caps_lock;
}

void rdp_set_suppress_output(RdpSession *session, bool allow_display) {
    (void)session;
    (void)allow_display;
}

void rdp_request_refresh(RdpSession *session) {
    (void)session;
}
