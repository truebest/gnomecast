#ifndef GNOMECAST_AUDIO_SS4S_H
#define GNOMECAST_AUDIO_SS4S_H

#include <stddef.h>
#include <stdint.h>

#include "media_ss4s.h"

typedef struct NativeAudio NativeAudio;

typedef enum NativeAudioResult {
    NATIVE_AUDIO_OK = 0,
    NATIVE_AUDIO_ERROR = 1
} NativeAudioResult;

/* Attaches an audio track to the shared media player. codec takes RdpAudioCodec values
 * from rdp_ffi.h (1=Opus, 2=PCM S16LE). Returns NULL when ss4s is not linked, the module
 * lacks the codec, or the hardware open fails — callers degrade to silent video. Jitter
 * buffering and resampling live upstream in NativeAudioPipeline; this boundary feeds
 * already-paced PCM directly. */
NativeAudio *native_audio_open(NativeMedia *media, uint32_t codec, uint32_t sample_rate, uint16_t channels);
/* Feed one encoded Opus packet or one interleaved S16LE PCM chunk. Transient conditions
 * (decoder not ready, buffer overflow) drop the chunk and still return OK. */
NativeAudioResult native_audio_feed(NativeAudio *audio, const uint8_t *data, size_t len);
/* Stops feeding without touching the hardware: on the webOS ss4s backends closing a track
 * unloads the WHOLE shared media pipeline (video included), so a mid-session audio
 * failure must degrade by muting, not by closing the track. Subsequent feeds are dropped. */
void native_audio_disable(NativeAudio *audio);
/* Closes the audio track. WARNING: on the webOS ss4s backends this unloads the shared
 * media pipeline; if a video track is still open, the caller must recover it (the video
 * decoder additionally needs a fresh IDR from the server after any reload). */
void native_audio_close(NativeAudio *audio);

#endif
