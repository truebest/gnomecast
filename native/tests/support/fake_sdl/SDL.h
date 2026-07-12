#ifndef GNOMECAST_TEST_FAKE_SDL_H
#define GNOMECAST_TEST_FAKE_SDL_H

#include <stdint.h>

typedef struct SDL_Renderer {
    int unused;
} SDL_Renderer;

typedef struct SDL_Texture {
    int unused;
} SDL_Texture;

typedef struct SDL_Rect {
    int x;
    int y;
    int w;
    int h;
} SDL_Rect;

#define SDL_PIXELFORMAT_RGBA32 1u
#define SDL_TEXTUREACCESS_STREAMING 1

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, uint32_t format, int access,
                               int width, int height);
void SDL_DestroyTexture(SDL_Texture *texture);
int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch);
int SDL_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture);
int SDL_SetRenderDrawColor(SDL_Renderer *renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *src,
                   const SDL_Rect *dst);
void SDL_RenderPresent(SDL_Renderer *renderer);
const char *SDL_GetError(void);

#endif
