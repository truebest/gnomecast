#include "rdp_ffi.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

_Static_assert(RDP_STATE_IDLE == 0, "RDP_STATE_IDLE value changed");
_Static_assert(RDP_STATE_CONNECTING == 1, "RDP_STATE_CONNECTING value changed");
_Static_assert(RDP_STATE_TLS == 2, "RDP_STATE_TLS value changed");
_Static_assert(RDP_STATE_CREDSSP == 3, "RDP_STATE_CREDSSP value changed");
_Static_assert(RDP_STATE_ACTIVE == 4, "RDP_STATE_ACTIVE value changed");
_Static_assert(RDP_STATE_NO_AVC420 == 5, "RDP_STATE_NO_AVC420 value changed");
_Static_assert(RDP_STATE_DECODER_ERROR == 6, "RDP_STATE_DECODER_ERROR value changed");
_Static_assert(RDP_STATE_NETWORK_ERROR == 7, "RDP_STATE_NETWORK_ERROR value changed");
_Static_assert(RDP_STATE_PROTOCOL_ERROR == 8, "RDP_STATE_PROTOCOL_ERROR value changed");
_Static_assert(RDP_STATE_STOPPED == 9, "RDP_STATE_STOPPED value changed");

_Static_assert(RDP_AUDIO_CODEC_OPUS == 1, "RDP_AUDIO_CODEC_OPUS value changed");
_Static_assert(RDP_AUDIO_CODEC_PCM_S16LE == 2, "RDP_AUDIO_CODEC_PCM_S16LE value changed");

_Static_assert(sizeof(RdpState) == sizeof(int), "RdpState must stay C-int sized for the FFI ABI");
_Static_assert(sizeof(((RdpConfig *)0)->host) == sizeof(const char *), "RdpConfig.host must be a pointer");
_Static_assert(sizeof(((RdpConfig *)0)->port) == sizeof(uint16_t), "RdpConfig.port must stay uint16_t");
_Static_assert(sizeof(((RdpConfig *)0)->width) == sizeof(uint16_t), "RdpConfig.width must stay uint16_t");
_Static_assert(sizeof(((RdpConfig *)0)->height) == sizeof(uint16_t), "RdpConfig.height must stay uint16_t");
_Static_assert(sizeof(((RdpConfig *)0)->fps) == sizeof(uint16_t), "RdpConfig.fps must stay uint16_t");
_Static_assert(sizeof(((RdpCallbacks *)0)->ctx) == sizeof(void *), "RdpCallbacks.ctx must be a pointer");

#if UINTPTR_MAX == UINT64_MAX
_Static_assert(offsetof(RdpConfig, host) == 0, "RdpConfig.host offset changed");
_Static_assert(offsetof(RdpConfig, port) == 8, "RdpConfig.port offset changed");
_Static_assert(offsetof(RdpConfig, username) == 16, "RdpConfig.username offset changed");
_Static_assert(offsetof(RdpConfig, password) == 24, "RdpConfig.password offset changed");
_Static_assert(offsetof(RdpConfig, domain) == 32, "RdpConfig.domain offset changed");
_Static_assert(offsetof(RdpConfig, width) == 40, "RdpConfig.width offset changed");
_Static_assert(offsetof(RdpConfig, height) == 42, "RdpConfig.height offset changed");
_Static_assert(offsetof(RdpConfig, fps) == 44, "RdpConfig.fps offset changed");
_Static_assert(sizeof(RdpConfig) == 48, "RdpConfig size changed");

_Static_assert(offsetof(RdpCallbacks, ctx) == 0, "RdpCallbacks.ctx offset changed");
_Static_assert(offsetof(RdpCallbacks, on_state) == 8, "RdpCallbacks.on_state offset changed");
_Static_assert(offsetof(RdpCallbacks, on_log) == 16, "RdpCallbacks.on_log offset changed");
_Static_assert(offsetof(RdpCallbacks, on_desktop_size) == 24, "RdpCallbacks.on_desktop_size offset changed");
_Static_assert(offsetof(RdpCallbacks, on_video_au) == 32, "RdpCallbacks.on_video_au offset changed");
_Static_assert(offsetof(RdpCallbacks, on_bitmap_update) == 40, "RdpCallbacks.on_bitmap_update offset changed");
_Static_assert(offsetof(RdpCallbacks, on_audio_format) == 48, "RdpCallbacks.on_audio_format offset changed");
_Static_assert(offsetof(RdpCallbacks, on_audio_data) == 56, "RdpCallbacks.on_audio_data offset changed");
_Static_assert(sizeof(RdpCallbacks) == 64, "RdpCallbacks size changed");
#elif UINTPTR_MAX == UINT32_MAX
_Static_assert(offsetof(RdpConfig, host) == 0, "RdpConfig.host offset changed");
_Static_assert(offsetof(RdpConfig, port) == 4, "RdpConfig.port offset changed");
_Static_assert(offsetof(RdpConfig, username) == 8, "RdpConfig.username offset changed");
_Static_assert(offsetof(RdpConfig, password) == 12, "RdpConfig.password offset changed");
_Static_assert(offsetof(RdpConfig, domain) == 16, "RdpConfig.domain offset changed");
_Static_assert(offsetof(RdpConfig, width) == 20, "RdpConfig.width offset changed");
_Static_assert(offsetof(RdpConfig, height) == 22, "RdpConfig.height offset changed");
_Static_assert(offsetof(RdpConfig, fps) == 24, "RdpConfig.fps offset changed");
_Static_assert(sizeof(RdpConfig) == 28, "RdpConfig size changed");

_Static_assert(offsetof(RdpCallbacks, ctx) == 0, "RdpCallbacks.ctx offset changed");
_Static_assert(offsetof(RdpCallbacks, on_state) == 4, "RdpCallbacks.on_state offset changed");
_Static_assert(offsetof(RdpCallbacks, on_log) == 8, "RdpCallbacks.on_log offset changed");
_Static_assert(offsetof(RdpCallbacks, on_desktop_size) == 12, "RdpCallbacks.on_desktop_size offset changed");
_Static_assert(offsetof(RdpCallbacks, on_video_au) == 16, "RdpCallbacks.on_video_au offset changed");
_Static_assert(offsetof(RdpCallbacks, on_bitmap_update) == 20, "RdpCallbacks.on_bitmap_update offset changed");
_Static_assert(offsetof(RdpCallbacks, on_audio_format) == 24, "RdpCallbacks.on_audio_format offset changed");
_Static_assert(offsetof(RdpCallbacks, on_audio_data) == 28, "RdpCallbacks.on_audio_data offset changed");
_Static_assert(sizeof(RdpCallbacks) == 32, "RdpCallbacks size changed");
#else
#error "Unsupported pointer width for RDP FFI layout test"
#endif

int main(void) {
    printf("PASS rdp-ffi-layout sizeof(RdpConfig)=%zu sizeof(RdpCallbacks)=%zu\n",
        sizeof(RdpConfig), sizeof(RdpCallbacks));
    return 0;
}
