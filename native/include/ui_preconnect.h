#ifndef GNOMECAST_UI_PRECONNECT_H
#define GNOMECAST_UI_PRECONNECT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <SDL.h>

typedef struct NativePreconnectUi NativePreconnectUi;

NativePreconnectUi *native_preconnect_ui_create(SDL_Window *window, SDL_Renderer *renderer, const char *host,
                                                uint16_t port, const char *username, const char *password,
                                                const char *domain, uint16_t fps, uint16_t audio_prebuffer_ms,
                                                bool relative_mouse, bool jump_filter);
void native_preconnect_ui_destroy(NativePreconnectUi *ui);
void native_preconnect_ui_resize(NativePreconnectUi *ui, int width, int height);
void native_preconnect_ui_tick(NativePreconnectUi *ui);
void native_preconnect_ui_set_visible(NativePreconnectUi *ui, bool visible);
void native_preconnect_ui_set_connecting(NativePreconnectUi *ui, bool connecting, const char *status);
void native_preconnect_ui_set_status(NativePreconnectUi *ui, const char *status, bool error);
bool native_preconnect_ui_read_current(NativePreconnectUi *ui, char *host, size_t host_cap, uint16_t *port,
                                       char *username, size_t username_cap, char *password, size_t password_cap,
                                       char *domain, size_t domain_cap, uint16_t *fps, uint16_t *audio_prebuffer_ms,
                                       bool *relative_mouse, bool *jump_filter);
bool native_preconnect_ui_take_connect(NativePreconnectUi *ui, char *host, size_t host_cap, uint16_t *port,
                                       char *username, size_t username_cap, char *password, size_t password_cap,
                                       char *domain, size_t domain_cap, uint16_t *fps, uint16_t *audio_prebuffer_ms,
                                       bool *relative_mouse, bool *jump_filter);

#endif
