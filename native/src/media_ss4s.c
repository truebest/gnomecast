#include "media_ss4s.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HELLOLG_WITH_SS4S
#include <stdarg.h>

#include <ss4s.h>
#endif

#ifdef HELLOLG_WITH_SS4S
typedef struct NativeSs4sModuleCandidate {
    const char *id;
    const char *kind;
} NativeSs4sModuleCandidate;

static const NativeSs4sModuleCandidate k_native_media_modules[] = {
    {.id = "ndl-webos5", .kind = "NDL/directmedia"},
    {.id = "ndl-webos4", .kind = "NDL/directmedia"},
    {.id = "ndl-esplayer", .kind = "NDL/esplayer"},
    {.id = "smp-webos", .kind = "SMP/StarfishMediaAPIs"},
    {.id = "smp-webos4", .kind = "SMP/StarfishMediaAPIs"},
    {.id = "smp-webos3", .kind = "SMP/StarfishMediaAPIs"},
};
#endif

struct NativeMedia {
    uint16_t viewport_width;
    uint16_t viewport_height;
#ifdef HELLOLG_WITH_SS4S
    SS4S_Player *player;
    const char *module_id;
    bool ss4s_started;
#endif
};

#ifdef HELLOLG_WITH_SS4S
static void native_ss4s_log(SS4S_LogLevel level, const char *tag, const char *fmt, ...) {
    static const char *const names[] = {"fatal", "error", "warn", "info", "debug", "verbose"};
    const char *level_name = "log";
    if ((size_t)level < sizeof(names) / sizeof(names[0])) {
        level_name = names[level];
    }

    fprintf(stderr, "[native-media.ss4s.%s.%s] ", level_name, tag ? tag : "ss4s");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt ? fmt : "", args);
    va_end(args);
    fputc('\n', stderr);
}

static bool native_media_module_allowed(const char *id) {
    if (!id) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_native_media_modules) / sizeof(k_native_media_modules[0]); i++) {
        if (strcmp(id, k_native_media_modules[i].id) == 0) {
            return true;
        }
    }
    return false;
}

/* Selection stays video-driven: video is the app's primary function and must never be
 * broken by audio concerns. The selected module is also passed as the audio driver; if
 * it happens to lack audio support, native_audio_open degrades gracefully later. */
static const NativeSs4sModuleCandidate *native_media_select_module(void) {
    for (size_t i = 0; i < sizeof(k_native_media_modules) / sizeof(k_native_media_modules[0]); i++) {
        const NativeSs4sModuleCandidate *candidate = &k_native_media_modules[i];
        SS4S_ModuleCheckFlag flags = SS4S_ModuleCheck(candidate->id, SS4S_MODULE_CHECK_VIDEO);
        if ((flags & SS4S_MODULE_CHECK_VIDEO) != 0) {
            return candidate;
        }
    }
    return NULL;
}

static bool native_media_init_ss4s(NativeMedia *media) {
    SS4S_SetLoggingFunction(native_ss4s_log);
    SS4S_SetLogLevel(SS4S_LogLevelInfo);

    const NativeSs4sModuleCandidate *module = native_media_select_module();
    if (!module) {
        fprintf(stderr, "[native-media] no supported ss4s hardware video module found (tried NDL/directmedia, then SMP)\n");
        return false;
    }

    char app_name[] = "gnomecast-native";
    char *argv[] = {app_name, NULL};
    int argc = 1;
    SS4S_Config config = {
        /* One module drives both tracks so they share a single player context; a missing
         * audio driver in the module never fails SS4S_Init, only audio open later. */
        .audioDriver = module->id,
        .videoDriver = module->id,
        .loggingFunction = native_ss4s_log,
    };
    if (SS4S_Init(argc, argv, &config) != 0) {
        fprintf(stderr, "[native-media] SS4S_Init failed for module %s\n", module->id);
        return false;
    }
    media->ss4s_started = true;
    media->module_id = module->id;

    const char *opened_module = SS4S_GetVideoModuleName();
    if (!native_media_module_allowed(opened_module)) {
        fprintf(stderr, "[native-media] ss4s opened unsupported video module `%s`; refusing dummy/software fallback\n",
                opened_module ? opened_module : "(none)");
        return false;
    }

    if (SS4S_PostInit(argc, argv) != 0) {
        fprintf(stderr, "[native-media] SS4S_PostInit failed for module %s\n", opened_module);
        return false;
    }

    const char *audio_module = SS4S_GetAudioModuleName();
    fprintf(stderr, "[native-media] selected ss4s %s module %s (audio module %s)\n", module->kind, opened_module,
            audio_module ? audio_module : "(none)");

    media->player = SS4S_PlayerOpen();
    if (!media->player) {
        fprintf(stderr, "[native-media] SS4S_PlayerOpen failed\n");
        return false;
    }
    /* wait=true would defer the NDL load until BOTH tracks open, which never happens in
     * video-only or audio-only sessions; keep false and accept one pipeline reload when
     * the second track opens (in practice audio negotiates before the first IDR). */
    SS4S_PlayerSetWaitAudioVideoReady(media->player, false);
    if (media->viewport_width != 0 && media->viewport_height != 0) {
        SS4S_PlayerSetViewportSize(media->player, (int)media->viewport_width, (int)media->viewport_height);
    }
    return true;
}
#endif

NativeMedia *native_media_open(uint16_t viewport_width, uint16_t viewport_height) {
#ifndef HELLOLG_WITH_SS4S
    (void)viewport_width;
    (void)viewport_height;
    fprintf(stderr, "[native-media] ss4s is not linked; hardware media unavailable\n");
    return NULL;
#else
    NativeMedia *media = (NativeMedia *)calloc(1, sizeof(NativeMedia));
    if (!media) {
        return NULL;
    }
    media->viewport_width = viewport_width;
    media->viewport_height = viewport_height;

    if (!native_media_init_ss4s(media)) {
        native_media_close(media);
        return NULL;
    }
    return media;
#endif
}

void native_media_close(NativeMedia *media) {
    if (!media) {
        return;
    }
#ifdef HELLOLG_WITH_SS4S
    if (media->player) {
        SS4S_PlayerClose(media->player);
    }
    if (media->ss4s_started) {
        SS4S_Quit();
    }
#endif
    free(media);
}

void native_media_set_viewport(NativeMedia *media, uint16_t viewport_width, uint16_t viewport_height) {
    if (!media || viewport_width == 0 || viewport_height == 0) {
        return;
    }
    if (media->viewport_width == viewport_width && media->viewport_height == viewport_height) {
        return;
    }
    media->viewport_width = viewport_width;
    media->viewport_height = viewport_height;
#ifdef HELLOLG_WITH_SS4S
    if (media->player) {
        SS4S_PlayerSetViewportSize(media->player, (int)viewport_width, (int)viewport_height);
        fprintf(stderr, "[native-media] viewport=%ux%u\n", (unsigned)viewport_width, (unsigned)viewport_height);
    }
#endif
}

SS4S_Player *native_media_player(NativeMedia *media) {
#ifndef HELLOLG_WITH_SS4S
    (void)media;
    return NULL;
#else
    return media ? media->player : NULL;
#endif
}
