#ifndef GNOMECAST_MEDIA_SS4S_H
#define GNOMECAST_MEDIA_SS4S_H

#include <stdint.h>

/* Owns the SS4S library lifecycle and the single hardware player that the video
 * (H.264/NDL) and audio (Opus/PCM) tracks share. Splitting this out of NativeVideo lets
 * audio survive video track close/reopen (desktop size changes, RemoteFX switches) and
 * exist at all in the RemoteFX/RGBA path where no NativeVideo is created. */
typedef struct NativeMedia NativeMedia;
typedef struct SS4S_Player SS4S_Player;

/* Returns NULL when ss4s is not linked or no supported hardware module exists.
 * viewport dimensions of 0 skip the initial viewport set. */
NativeMedia *native_media_open(uint16_t viewport_width, uint16_t viewport_height);
void native_media_close(NativeMedia *media);
void native_media_set_viewport(NativeMedia *media, uint16_t viewport_width, uint16_t viewport_height);
/* NULL without ss4s or before native_media_open succeeds. */
SS4S_Player *native_media_player(NativeMedia *media);

#endif
