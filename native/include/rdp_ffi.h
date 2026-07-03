#ifndef GNOMECAST_RDP_FFI_H
#define GNOMECAST_RDP_FFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RdpState {
    RDP_STATE_IDLE = 0,
    RDP_STATE_CONNECTING = 1,
    RDP_STATE_TLS = 2,
    RDP_STATE_CREDSSP = 3,
    RDP_STATE_ACTIVE = 4,
    RDP_STATE_NO_AVC420 = 5,
    RDP_STATE_DECODER_ERROR = 6,
    RDP_STATE_NETWORK_ERROR = 7,
    RDP_STATE_PROTOCOL_ERROR = 8,
    RDP_STATE_STOPPED = 9
} RdpState;

/* Values shared with RDP_AUDIO_CODEC_* constants in webrdp-min/src/native.rs. */
typedef enum RdpAudioCodec {
    RDP_AUDIO_CODEC_OPUS = 1,
    RDP_AUDIO_CODEC_PCM_S16LE = 2
} RdpAudioCodec;

typedef struct RdpConfig {
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;
    const char *domain;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
} RdpConfig;

typedef struct RdpCallbacks {
    void *ctx;
    void (*on_state)(void *ctx, RdpState state, const char *detail);
    void (*on_log)(void *ctx, const char *line);
    void (*on_desktop_size)(void *ctx, uint16_t width, uint16_t height);
    void (*on_video_au)(void *ctx, const uint8_t *data, size_t len, bool is_keyframe, uint64_t pts90k);
    void (*on_bitmap_update)(void *ctx, uint16_t surface_id, uint32_t left, uint32_t top, uint32_t width,
                             uint32_t height, uint32_t stride, const uint8_t *rgba, size_t len);
    /* codec takes RdpAudioCodec values. Fired before the first on_audio_data of a stream
     * and again whenever the negotiated format changes. */
    void (*on_audio_format)(void *ctx, uint32_t codec, uint32_t sample_rate, uint16_t channels);
    /* One encoded packet (Opus) or PCM chunk per call; bytes are valid only for the call. */
    void (*on_audio_data)(void *ctx, const uint8_t *data, size_t len, uint32_t ts_ms);
} RdpCallbacks;

typedef struct RdpSession RdpSession;

RdpSession *rdp_session_start(const RdpConfig *config, const RdpCallbacks *callbacks);
void rdp_session_stop(RdpSession *session);

void rdp_send_pointer_move(RdpSession *session, uint16_t x, uint16_t y);
void rdp_send_pointer_button(RdpSession *session, uint16_t x, uint16_t y, uint8_t button, bool down);
void rdp_send_pointer_wheel(RdpSession *session, uint16_t x, uint16_t y, int16_t delta);
void rdp_send_key(RdpSession *session, uint8_t scancode, bool down, bool extended);
void rdp_send_unicode(RdpSession *session, uint16_t codepoint, bool down);

#ifdef __cplusplus
}
#endif

#endif
