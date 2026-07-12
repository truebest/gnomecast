/* SPDX-License-Identifier: MIT */
#ifndef BACKEND_NDL_API_H
#define BACKEND_NDL_API_H

#include <backend_ndl/backend_ndl.h>

#if defined(BACKEND_NDL_ENABLED) && BACKEND_NDL_ENABLED
#ifndef NDL_DIRECTMEDIA_API_VERSION
#define NDL_DIRECTMEDIA_API_VERSION 2
#endif
#include <NDL_directmedia.h>

/* Private resolved entry-point table. Tests inject fakes through
 * backend_ndl_open_with_api(); neither is installed as public API. */
typedef struct BackendNdlApi {
    const char *(*DirectMediaGetError)(void);
    int (*DirectMediaInit)(const char *app_id, ResourceReleased cb);
    int (*DirectMediaQuit)(void);
    int (*DirectMediaLoad)(NDL_DIRECTMEDIA_DATA_INFO_T *data, NDLMediaLoadCallback cb);
    int (*DirectMediaUnload)(void);
    int (*DirectVideoPlay)(void *buffer, unsigned int size, long long pts);
    int (*DirectAudioPlay)(void *buffer, unsigned int size, long long pts);

    bool (*DirectMedia_DL_Initialize)(void);
    void (*DirectMedia_DL_Finalize)(void);
    bool (*DirectMedia_DL_IsInitialized)(void);
    int (*DirectMediaSetAppState)(NDL_DIRECTMEDIA_APP_STATE state);
    int (*DirectVideoSetArea)(int left, int top, int width, int height);
    int (*DirectVideoSetFrameDropThreshold)(int threshold);
    int (*DirectVideoFlushRenderBuffer)(void);
    int (*DirectVideoGetRenderBufferLength)(int *length);
    int (*DirectAudioGetAvailableBufferSize)(int *available);
    int (*DirectAudioGetTotalBufferSize)(int *total);
} BackendNdlApi;

#if defined(BACKEND_NDL_TESTING) && BACKEND_NDL_TESTING
BackendNdl *backend_ndl_open_with_api(const BackendNdlConfig *config, const BackendNdlApi *api);
#endif
#endif

#endif
