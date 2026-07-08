#ifndef GNOMECAST_AUDIO_OPUS_H
#define GNOMECAST_AUDIO_OPUS_H

#include <stddef.h>
#include <stdint.h>

/* Thin per-session libopus decoder for the audio mixer: rdpsnd delivers encoded Opus
 * packets (grd prefers Opus over PCM when the client offers both — ~96kbps instead of
 * ~1.4Mbps raw), and mixing needs raw samples, so each session decodes on its own
 * rdp-worker thread before pushing into the mixer ring. libopus decode is cheap (~1-2%
 * of one ARM core per 48kHz stereo stream, NEON-optimized).
 *
 * Builds without HELLOLG_WITH_OPUS stub out to "unavailable": open returns NULL and the
 * caller degrades that session to silence with a log. */

typedef struct NativeOpusDecoder NativeOpusDecoder;

/* Returns NULL when libopus is not compiled in or the decoder cannot be created. */
NativeOpusDecoder *native_opus_decoder_open(uint32_t sample_rate, uint16_t channels);

/* Decodes one Opus packet. Returns the number of frames decoded (>0) and points *pcm at
 * an internal interleaved S16 buffer valid until the next call; returns 0 for a packet
 * that should simply be skipped (corrupt/oversized — logged once per decoder). */
int native_opus_decoder_decode(NativeOpusDecoder *decoder, const uint8_t *data, size_t len, const int16_t **pcm);

void native_opus_decoder_close(NativeOpusDecoder *decoder);

#endif
