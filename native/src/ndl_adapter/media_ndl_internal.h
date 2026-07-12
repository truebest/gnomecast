#ifndef GNOMECAST_MEDIA_NDL_INTERNAL_H
#define GNOMECAST_MEDIA_NDL_INTERNAL_H

/* Private bridge shared only by the gnomecast NDL adapters. It owns the
 * application's track choreography on top of the atomic DirectMedia API. */

#include <backend_ndl/backend_ndl.h>

#include "media_backend.h"

BackendNdl *native_media_ndl_backend(NativeMedia *media);
BackendNdlResult native_media_ndl_configure_video(NativeMedia *media,
                                                   const BackendNdlVideoInfo *info);
BackendNdlResult native_media_ndl_configure_audio(NativeMedia *media,
                                                   const BackendNdlPcmInfo *info);
BackendNdlResult native_media_ndl_clear_video(NativeMedia *media);
BackendNdlResult native_media_ndl_clear_audio(NativeMedia *media);

#ifdef HELLOLG_NDL_ADAPTER_TESTING
BackendNdlLogLevel native_media_ndl_test_min_level(void);
#endif

#endif
