#ifndef GNOMECAST_AUDIO_BACKEND_H
#define GNOMECAST_AUDIO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "media_backend.h"

typedef struct NativeAudio NativeAudio;

typedef enum NativeAudioResult {
    NATIVE_AUDIO_OK = 0,
    NATIVE_AUDIO_ERROR = 1
} NativeAudioResult;

/* Attaches an audio track to the shared media pipeline. codec takes RdpAudioCodec
 * values from rdp_ffi.h; the NDL backend accepts only PCM S16LE (2=PCM) — Opus is
 * decoded and mixed upstream, so no passthrough path exists. Returns NULL when the
 * backend is not linked, the format is unsupported, or the hardware open fails —
 * callers degrade to silent video. Jitter buffering and resampling live upstream in
 * NativeAudioPipeline; this boundary feeds already-paced PCM directly. */
NativeAudio *native_audio_open(NativeMedia *media, uint32_t codec, uint32_t sample_rate, uint16_t channels);
/* Feed one interleaved S16LE PCM chunk. Transient conditions (pipeline reloading,
 * buffer overflow) drop the chunk and still return OK. */
NativeAudioResult native_audio_feed(NativeAudio *audio, const uint8_t *data, size_t len);
/* Stops feeding without touching the hardware. This avoids a pipeline reload and
 * video keyframe recovery for a best-effort mid-session audio failure. Subsequent
 * feeds are dropped. */
void native_audio_disable(NativeAudio *audio);
/* Closes the audio track. On webOS the media adapter reloads any surviving video
 * track; the decoder then needs a fresh IDR because the pipeline was reloaded. */
void native_audio_close(NativeAudio *audio);

#endif
