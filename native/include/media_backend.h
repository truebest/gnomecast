#ifndef GNOMECAST_MEDIA_BACKEND_H
#define GNOMECAST_MEDIA_BACKEND_H

#include <stdint.h>

/* Owns the hardware media backend lifecycle (NDL DirectMedia on webOS) and the
 * single shared pipeline that the video (H.264) and audio (PCM) tracks attach to.
 * Splitting this out of NativeVideo lets audio survive video track close/reopen
 * (desktop size changes, RemoteFX switches) and exist at all in the RemoteFX/RGBA
 * path where no NativeVideo is created. */
typedef struct NativeMedia NativeMedia;

/* Returns NULL when the backend is not linked (host builds) or the NDL library /
 * DirectMedia init is unavailable on the device. viewport dimensions of 0 skip the
 * initial viewport set. */
NativeMedia *native_media_open(uint16_t viewport_width, uint16_t viewport_height);
void native_media_close(NativeMedia *media);
/* Bookkeeping only on NDL: the hardware video plane scales to the output itself, so
 * there is no per-viewport call. Kept in the contract for logging and future
 * NDL_DirectVideoSetArea experiments. */
void native_media_set_viewport(NativeMedia *media, uint16_t viewport_width, uint16_t viewport_height);

#endif
