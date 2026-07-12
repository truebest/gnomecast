#include "ui_preconnect.h"

#include "rdp_ffi.h"
#include "ui_fonts.h"
#include "ui_host.h"
#include "ui_mixer.h"
#include "ui_profile_name.h"
#include "ui_slot_palette.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw/sdl/lv_draw_sdl.h"
#include "lvgl.h"

#include "clog.h"

clog_define(g_native_log_ui, cLogLevelInfo, cLogFlags_Default, "ui.preconnect", NULL);

/* host/username/domain are capped at 255 usable chars: more than enough for any real
 * endpoint (a DNS name maxes at 253), Windows username, or domain, and comfortably below
 * NativeConfig's 512-byte storage so a form value can never overflow the config on save. */
#define UI_HOST_MAX 256u
#define UI_NAME_MAX NATIVE_SETTINGS_STRING_MAX
#define UI_USERNAME_MAX 256u
#define UI_DOMAIN_MAX 256u
/* Keep the UI password limit aligned with NativeConfig's 512-byte string storage. */
#define UI_PASSWORD_TEXT_MAX 511u
#define UI_PASSWORD_MAX (UI_PASSWORD_TEXT_MAX + 1u)
#define UI_PORT_MAX 6u
#define UI_DETAIL_MAX 192u
#define UI_SETUP_PANEL_WIDTH 680
#define UI_CANVAS_WIDTH 1920
#define UI_CANVAS_HEIGHT 1080
#define UI_CARD_HEIGHT 218
#define UI_CARD_GAP 28
#define UI_FORM_KEY_EVENT (LV_EVENT_KEY | LV_EVENT_PREPROCESS)
#define UI_BRAND_CUBE_SIZE 34
#define UI_ONBOARDING_CUBE_SIZE 38
#define UI_CARD_AUDIO_METER_COUNT 2
#define UI_CARD_AUDIO_METER_WIDTH 6
#define UI_CARD_AUDIO_METER_HEIGHT 22
#define UI_CARD_AUDIO_METER_TOP 8
#define UI_CARD_AUDIO_METER_BASELINE (UI_CARD_AUDIO_METER_TOP + UI_CARD_AUDIO_METER_HEIGHT)
#define UI_HUB_MASK_COLOR 0x040508
#define UI_HUB_MASK_SEGMENT_COUNT 4
#define UI_HUB_X_AT_PERCENT(pct) ((UI_CANVAS_WIDTH * (pct) + 50) / 100)
#define UI_HUB_Y_FROM_BOTTOM_PERCENT(pct) (UI_CANVAS_HEIGHT - (UI_CANVAS_HEIGHT * (pct) + 50) / 100)
/* LVGL consumes one key state per read; text input is represented as press+release per char.
 * The ring is sized for the largest single field paste (the 511-char password) with one slot
 * kept empty, so capacity is UI_KEY_QUEUE_CAP - 1. */
#define UI_KEY_QUEUE_TEXT_MAX UI_PASSWORD_TEXT_MAX
#define UI_KEY_QUEUE_CAP (2u * UI_KEY_QUEUE_TEXT_MAX + 1u)

_Static_assert(UI_KEY_QUEUE_CAP - 1u >= 2u * (UI_USERNAME_MAX - 1u), "key queue must hold username paste");
_Static_assert(UI_KEY_QUEUE_CAP - 1u >= 2u * (UI_DOMAIN_MAX - 1u), "key queue must hold domain paste");
_Static_assert(UI_KEY_QUEUE_CAP - 1u >= 2u * (UI_NAME_MAX - 1u), "key queue must hold profile-name paste");
_Static_assert(UI_KEY_QUEUE_CAP - 1u >= 2u * UI_PASSWORD_TEXT_MAX, "key queue must hold password paste");

static const uint16_t UI_FPS_OPTIONS[] = {30, 60};

typedef struct NativePreconnectQueuedKey {
    lv_indev_state_t state;
    uint32_t key;
} NativePreconnectQueuedKey;

typedef struct NativePreconnectKeyDriver {
    lv_indev_drv_t base;
    lv_indev_state_t state;
    uint32_t key;
    NativePreconnectQueuedKey queue[UI_KEY_QUEUE_CAP];
    unsigned head;
    unsigned tail;
    bool overflow_pending;
} NativePreconnectKeyDriver;

typedef struct NativeUiCube {
    lv_obj_t *obj;
    int32_t angle_tenths;
    float half_edge;
} NativeUiCube;

struct NativePreconnectUi {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    lv_disp_draw_buf_t draw_buf;
    lv_disp_drv_t disp_drv;
    lv_draw_sdl_drv_param_t draw_param;
    lv_disp_t *disp;
    lv_indev_drv_t pointer_drv;
    lv_indev_t *pointer_indev;
    NativePreconnectUiBackgroundDrawFn background_draw;
    void *background_draw_ctx;
    bool hardware_video_plane;
    NativePreconnectKeyDriver key_drv;
    lv_indev_t *key_indev;
    lv_group_t *group;
    lv_obj_t *root;
    lv_obj_t *slot_buttons[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *card_badges[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *card_badge_labels[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *card_name_labels[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *card_tabs[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *hero_chip;
    lv_obj_t *hero_slot_label;
    lv_obj_t *hero_name_label;
    lv_obj_t *hero_state_dot;
    lv_obj_t *hero_state_label;
    lv_obj_t *hero_meta_label;
    lv_obj_t *hero_detail_panel;
    lv_obj_t *hero_detail_label;
    lv_obj_t *hero_audio_group;
    lv_obj_t *hero_audio_label;
    lv_obj_t *hero_action_btn;
    lv_obj_t *hero_action_label;
    lv_obj_t *hero_edit_btn;
    lv_obj_t *keyboard_pill;
    lv_obj_t *keyboard_dot;
    lv_obj_t *mouse_dot;
    lv_obj_t *keyboard_warning;
    lv_obj_t *keyboard_warning_label;
    lv_obj_t *help_btn;
    NativeUiCube brand_cube;
    lv_obj_t *setup_scrim;
    lv_obj_t *setup_panel;
    lv_obj_t *form_title;
    lv_obj_t *name_input;
    lv_obj_t *host_input;
    lv_obj_t *port_input;
    lv_obj_t *color_choices[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *username_input;
    lv_obj_t *domain_input;
    lv_obj_t *password_input;
    lv_obj_t *fps_dropdown;
    lv_obj_t *audio_codec_dropdown;
    lv_obj_t *connect_btn;
    lv_obj_t *save_btn;
    lv_obj_t *cancel_btn;
    lv_obj_t *delete_btn;
    lv_obj_t *delete_label;
    lv_obj_t *status_label;
    lv_obj_t *onboarding_scrim;
    lv_obj_t *onboarding_btn;
    NativeUiCube onboarding_cube;
    lv_obj_t *card_audio_groups[NATIVE_SETTINGS_MAX_SESSIONS];
    lv_obj_t *card_audio_meter_clips[NATIVE_SETTINGS_MAX_SESSIONS][UI_CARD_AUDIO_METER_COUNT];
    lv_obj_t *card_audio_meter_grads[NATIVE_SETTINGS_MAX_SESSIONS][UI_CARD_AUDIO_METER_COUNT];
    int card_audio_meter_px[NATIVE_SETTINGS_MAX_SESSIONS][UI_CARD_AUDIO_METER_COUNT];
    float card_audio_meter_db[NATIVE_SETTINGS_MAX_SESSIONS][UI_CARD_AUDIO_METER_COUNT];
    int32_t slot_audio_peaks[NATIVE_SETTINGS_MAX_SESSIONS][UI_CARD_AUDIO_METER_COUNT];
    uint32_t card_audio_meter_ticks;
    lv_grad_dsc_t hub_horizontal_gradients[UI_HUB_MASK_SEGMENT_COUNT];
    lv_grad_dsc_t hub_vertical_gradients[UI_HUB_MASK_SEGMENT_COUNT];
    /* Volume-mixer overlay (ui_mixer.c): a separate TRANSPARENT screen on this display,
     * shown over the video plane during streaming. Owned here (shares the renderer /
     * screen texture / lv display), driven from main.c via native_preconnect_ui_mixer. */
    NativeUiMixer *mixer;
    lv_style_t root_style;
    lv_style_t nav_style;
    lv_style_t detail_style;
    lv_style_t title_style;
    lv_style_t hero_title_style;
    lv_style_t label_style;
    lv_style_t eyebrow_style;
    lv_style_t muted_style;
    lv_style_t input_style;
    lv_style_t input_focus_style;
    lv_style_t input_cursor_style;
    lv_style_t dropdown_style;
    lv_style_t dropdown_focus_style;
    lv_style_t dropdown_list_style;
    lv_style_t dropdown_selected_style;
    lv_style_t button_style;
    lv_style_t secondary_button_style;
    lv_style_t button_focus_style;
    lv_style_t button_disabled_style;
    lv_style_t card_style;
    lv_style_t card_focus_style;
    lv_style_t status_style;
    int width;
    int height;
    int pointer_x;
    int pointer_y;
    bool pointer_pressed;
    bool visible;
    bool hidden_cleared;
    bool connecting;
    bool connect_requested;
    bool activate_requested;
    bool hub_close_requested;
    bool hub_closing;
    bool save_requested;
    bool delete_requested;
    bool save_pending;
    bool connect_save_pending;
    bool delete_pending;
    bool delete_armed;
    bool loading_form;
    bool rebuilding_group;
    bool setup_visible;
    bool onboarding_visible;
    bool keyboard_available;
    bool current_fps_option;
    uint16_t current_fps;
    uint16_t selected_fps;
    /* Per-slot stored form values; the form shows slot_values[selected_slot]. */
    NativeSessionConfig slot_values[NATIVE_SETTINGS_MAX_SESSIONS];
    NativeSessionConfig committed_values[NATIVE_SETTINGS_MAX_SESSIONS];
    char slot_port_text[NATIVE_SETTINGS_MAX_SESSIONS][UI_PORT_MAX];
    bool slot_port_valid[NATIVE_SETTINGS_MAX_SESSIONS];
    NativePreconnectSessionState slot_states[NATIVE_SETTINGS_MAX_SESSIONS];
    char slot_details[NATIVE_SETTINGS_MAX_SESSIONS][UI_DETAIL_MAX];
    uint16_t slot_desktop_width[NATIVE_SETTINGS_MAX_SESSIONS];
    uint16_t slot_desktop_height[NATIVE_SETTINGS_MAX_SESSIONS];
    uint32_t slot_session_minutes[NATIVE_SETTINGS_MAX_SESSIONS];
    bool slot_audio_stream_open[NATIVE_SETTINGS_MAX_SESSIONS];
    uint32_t slot_audio_codec[NATIVE_SETTINGS_MAX_SESSIONS];
    uint32_t slot_audio_sample_rate[NATIVE_SETTINGS_MAX_SESSIONS];
    uint16_t slot_audio_channels[NATIVE_SETTINGS_MAX_SESSIONS];
    int selected_slot;
    int requested_slot;
    int activated_slot;
    int saved_slot;
    int deleted_slot;
    NativePreconnectSessionState requested_previous_state;
    char requested_previous_detail[UI_DETAIL_MAX];
    char requested_host[UI_HOST_MAX];
    char requested_username[UI_USERNAME_MAX];
    char requested_domain[UI_DOMAIN_MAX];
    char requested_password[UI_PASSWORD_MAX];
    uint16_t requested_port;
    uint16_t requested_fps;
    uint16_t requested_audio_codec;
    uint16_t committed_audio_codec;
    bool requested_requires_save;
};

static void ui_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *src);
static void ui_clear(lv_disp_drv_t *drv, uint8_t *buf, uint32_t size);
static void ui_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
static void ui_key_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
static void ui_reset_input_state(NativePreconnectUi *ui);
static void ui_input_changed(lv_event_t *event);
static void ui_profile_name_insert(lv_event_t *event);
static void ui_fps_changed(lv_event_t *event);
static void ui_form_key_event(lv_event_t *event);
static void ui_hub_key_event(lv_event_t *event);
static void ui_connect_clicked(lv_event_t *event);
static void ui_save_clicked(lv_event_t *event);
static void ui_cancel_clicked(lv_event_t *event);
static void ui_delete_clicked(lv_event_t *event);
static void ui_setup_scrim_clicked(lv_event_t *event);
static void ui_edit_clicked(lv_event_t *event);
static void ui_hero_action_clicked(lv_event_t *event);
static void ui_help_clicked(lv_event_t *event);
static void ui_onboarding_close_clicked(lv_event_t *event);
static void ui_slot_button_clicked(lv_event_t *event);
static void ui_slot_button_focused(lv_event_t *event);
static void ui_scroll_focused_into_view(NativePreconnectUi *ui);
static void ui_store_form_to_slot(NativePreconnectUi *ui, int slot);
static void ui_discard_form_changes(NativePreconnectUi *ui);
static void ui_load_slot_into_form(NativePreconnectUi *ui, int slot);
static void ui_update_slot_buttons(NativePreconnectUi *ui);
static void ui_update_hub(NativePreconnectUi *ui);
static void ui_show_setup(NativePreconnectUi *ui, bool visible);
static void ui_show_onboarding(NativePreconnectUi *ui, bool visible);
static bool ui_queue_connect(NativePreconnectUi *ui, int slot, bool force_save);

static bool ui_parse_port(const char *text, uint16_t *port) {
    if (!text || !text[0]) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static bool ui_host_valid(const char *host) {
    char normalized[UI_HOST_MAX];
    return native_ui_host_normalize(host, normalized, sizeof(normalized));
}

static size_t ui_fps_option_count(void) {
    return sizeof(UI_FPS_OPTIONS) / sizeof(UI_FPS_OPTIONS[0]);
}

static bool ui_find_fps_option(uint16_t fps, size_t *index) {
    for (size_t i = 0; i < ui_fps_option_count(); i++) {
        if (UI_FPS_OPTIONS[i] == fps) {
            if (index) {
                *index = i;
            }
            return true;
        }
    }
    return false;
}

static size_t ui_select_fps_index(NativePreconnectUi *ui, uint16_t fps) {
    size_t builtin_index = 0;
    /* Recomputed per load: a stale custom entry from the PREVIOUS slot would otherwise
     * stay in every slot's dropdown and could save that slot's fps into this one. The
     * callers always follow with ui_set_fps_options + ui_set_selected_fps, which keeps
     * the option list and index consistent with this flag. */
    ui->current_fps_option = false;
    if (ui_find_fps_option(fps, &builtin_index)) {
        return builtin_index;
    }
    ui->current_fps_option = true;
    ui->current_fps = fps;
    return 0;
}

static void ui_set_selected_fps(NativePreconnectUi *ui, size_t index) {
    if (ui->current_fps_option && index == 0) {
        ui->selected_fps = ui->current_fps;
        if (ui->fps_dropdown) {
            lv_dropdown_set_selected(ui->fps_dropdown, 0);
        }
        return;
    }

    size_t builtin_index = ui->current_fps_option ? index - 1u : index;
    if (builtin_index >= ui_fps_option_count()) {
        builtin_index = 1u;
        index = ui->current_fps_option ? builtin_index + 1u : builtin_index;
    }
    ui->selected_fps = UI_FPS_OPTIONS[builtin_index];
    if (ui->fps_dropdown) {
        lv_dropdown_set_selected(ui->fps_dropdown, (uint16_t)index);
    }
}

static void ui_set_fps_options(NativePreconnectUi *ui) {
    char options[48];
    if (ui->current_fps_option) {
        (void)snprintf(options, sizeof(options), "%u FPS (Current)\n%u FPS\n%u FPS", (unsigned)ui->current_fps,
                       (unsigned)UI_FPS_OPTIONS[0], (unsigned)UI_FPS_OPTIONS[1]);
    } else {
        (void)snprintf(options, sizeof(options), "%u FPS\n%u FPS", (unsigned)UI_FPS_OPTIONS[0], (unsigned)UI_FPS_OPTIONS[1]);
    }
    lv_dropdown_set_options(ui->fps_dropdown, options);
}

static bool ui_form_valid(NativePreconnectUi *ui) {
    uint16_t port = 0;
    const char *host = lv_textarea_get_text(ui->host_input);
    const char *port_text = lv_textarea_get_text(ui->port_input);
    const char *username = lv_textarea_get_text(ui->username_input);
    const char *password = lv_textarea_get_text(ui->password_input);
    return ui_host_valid(host) && port_text && ui_parse_port(port_text, &port) && username && username[0] && password &&
           password[0];
}

static void ui_update_connect_state(NativePreconnectUi *ui) {
    if (!ui || !ui->connect_btn || !ui->save_btn) {
        return;
    }
    bool action_pending = ui->connecting || ui->save_pending || ui->connect_save_pending || ui->delete_pending;
    bool slot_connecting = ui->selected_slot >= 0 && ui->selected_slot < NATIVE_SETTINGS_MAX_SESSIONS &&
                           ui->slot_states[ui->selected_slot] == NATIVE_PRECONNECT_SESSION_CONNECTING;
    bool disabled = action_pending || slot_connecting || !ui_form_valid(ui);
    if (disabled) {
        lv_obj_add_state(ui->connect_btn, LV_STATE_DISABLED);
        lv_obj_add_state(ui->save_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(ui->connect_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(ui->save_btn, LV_STATE_DISABLED);
    }
    if (ui->delete_btn) {
        if (action_pending || slot_connecting) {
            lv_obj_add_state(ui->delete_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(ui->delete_btn, LV_STATE_DISABLED);
        }
    }
    if (ui->cancel_btn) {
        if (action_pending) {
            lv_obj_add_state(ui->cancel_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(ui->cancel_btn, LV_STATE_DISABLED);
        }
    }
}

static void ui_set_text(lv_obj_t *label, const char *text) {
    lv_label_set_text(label, text && text[0] ? text : "");
}

static void ui_init_styles(NativePreconnectUi *ui) {
    lv_style_init(&ui->root_style);
    lv_style_set_bg_opa(&ui->root_style, LV_OPA_TRANSP);
    lv_style_set_border_width(&ui->root_style, 0);
    lv_style_set_pad_all(&ui->root_style, 0);
    lv_style_set_radius(&ui->root_style, 0);

    lv_style_init(&ui->nav_style);
    lv_style_set_bg_color(&ui->nav_style, lv_color_white());
    lv_style_set_bg_opa(&ui->nav_style, LV_OPA_10);
    lv_style_set_border_width(&ui->nav_style, 1);
    lv_style_set_border_color(&ui->nav_style, lv_color_white());
    lv_style_set_border_opa(&ui->nav_style, LV_OPA_10);
    lv_style_set_radius(&ui->nav_style, LV_RADIUS_CIRCLE);
    lv_style_set_pad_hor(&ui->nav_style, 20);
    lv_style_set_pad_ver(&ui->nav_style, 10);

    lv_style_init(&ui->detail_style);
    lv_style_set_bg_color(&ui->detail_style, lv_color_hex(0x101319));
    lv_style_set_bg_opa(&ui->detail_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->detail_style, 1);
    lv_style_set_border_color(&ui->detail_style, lv_color_white());
    lv_style_set_border_opa(&ui->detail_style, LV_OPA_10);
    lv_style_set_radius(&ui->detail_style, 0);
    lv_style_set_pad_all(&ui->detail_style, 0);

    lv_style_init(&ui->title_style);
    lv_style_set_text_color(&ui->title_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->title_style, &lv_font_ibm_plex_sans_semibold_24);
    lv_style_set_text_letter_space(&ui->title_style, -1);

    lv_style_init(&ui->hero_title_style);
    lv_style_set_text_color(&ui->hero_title_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->hero_title_style, &lv_font_ibm_plex_sans_semibold_86);
    lv_style_set_text_letter_space(&ui->hero_title_style, -2);

    lv_style_init(&ui->label_style);
    lv_style_set_text_color(&ui->label_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->label_style, &lv_font_ibm_plex_sans_regular_24);

    lv_style_init(&ui->eyebrow_style);
    lv_style_set_text_color(&ui->eyebrow_style, lv_color_hex(0xe9eef8));
    lv_style_set_text_opa(&ui->eyebrow_style, LV_OPA_60);
    lv_style_set_text_font(&ui->eyebrow_style, &lv_font_ibm_plex_mono_semibold_18);
    lv_style_set_text_letter_space(&ui->eyebrow_style, 2);

    lv_style_init(&ui->muted_style);
    lv_style_set_text_color(&ui->muted_style, lv_color_hex(0xe9eef8));
    lv_style_set_text_opa(&ui->muted_style, LV_OPA_60);
    lv_style_set_text_font(&ui->muted_style, &lv_font_ibm_plex_mono_regular_22);

    lv_style_init(&ui->input_style);
    lv_style_set_bg_color(&ui->input_style, lv_color_white());
    lv_style_set_bg_opa(&ui->input_style, LV_OPA_10);
    lv_style_set_border_width(&ui->input_style, 1);
    lv_style_set_border_color(&ui->input_style, lv_color_white());
    lv_style_set_border_opa(&ui->input_style, LV_OPA_20);
    lv_style_set_radius(&ui->input_style, 12);
    lv_style_set_pad_hor(&ui->input_style, 20);
    lv_style_set_pad_ver(&ui->input_style, 14);
    lv_style_set_text_color(&ui->input_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->input_style, &lv_font_ibm_plex_mono_regular_22);

    lv_style_init(&ui->input_focus_style);
    lv_style_set_bg_color(&ui->input_focus_style, lv_color_white());
    lv_style_set_bg_opa(&ui->input_focus_style, LV_OPA_10);
    lv_style_set_outline_color(&ui->input_focus_style, lv_color_hex(0xeef3fd));
    lv_style_set_outline_opa(&ui->input_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->input_focus_style, 3);
    lv_style_set_outline_pad(&ui->input_focus_style, 2);

    lv_style_init(&ui->input_cursor_style);
    lv_style_set_bg_opa(&ui->input_cursor_style, LV_OPA_TRANSP);
    lv_style_set_border_side(&ui->input_cursor_style, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_color(&ui->input_cursor_style, lv_color_hex(0xffffff));
    lv_style_set_border_width(&ui->input_cursor_style, 2);
    lv_style_set_pad_left(&ui->input_cursor_style, -1);
    lv_style_set_anim_time(&ui->input_cursor_style, 450);

    lv_style_init(&ui->dropdown_style);
    lv_style_set_bg_color(&ui->dropdown_style, lv_color_white());
    lv_style_set_bg_opa(&ui->dropdown_style, LV_OPA_10);
    lv_style_set_border_width(&ui->dropdown_style, 1);
    lv_style_set_border_color(&ui->dropdown_style, lv_color_white());
    lv_style_set_border_opa(&ui->dropdown_style, LV_OPA_20);
    lv_style_set_radius(&ui->dropdown_style, 12);
    lv_style_set_pad_hor(&ui->dropdown_style, 16);
    lv_style_set_pad_ver(&ui->dropdown_style, 12);
    lv_style_set_text_color(&ui->dropdown_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->dropdown_style, &lv_font_ibm_plex_mono_regular_22);

    lv_style_init(&ui->dropdown_focus_style);
    lv_style_set_bg_color(&ui->dropdown_focus_style, lv_color_white());
    lv_style_set_bg_opa(&ui->dropdown_focus_style, LV_OPA_10);
    lv_style_set_outline_color(&ui->dropdown_focus_style, lv_color_hex(0xeef3fd));
    lv_style_set_outline_opa(&ui->dropdown_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->dropdown_focus_style, 3);
    lv_style_set_outline_pad(&ui->dropdown_focus_style, 2);

    lv_style_init(&ui->dropdown_list_style);
    lv_style_set_bg_color(&ui->dropdown_list_style, lv_color_hex(0x171b23));
    lv_style_set_bg_opa(&ui->dropdown_list_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->dropdown_list_style, 1);
    lv_style_set_border_color(&ui->dropdown_list_style, lv_color_white());
    lv_style_set_border_opa(&ui->dropdown_list_style, LV_OPA_20);
    lv_style_set_radius(&ui->dropdown_list_style, 12);
    lv_style_set_pad_all(&ui->dropdown_list_style, 8);
    lv_style_set_text_color(&ui->dropdown_list_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->dropdown_list_style, &lv_font_ibm_plex_mono_regular_22);

    lv_style_init(&ui->dropdown_selected_style);
    lv_style_set_bg_color(&ui->dropdown_selected_style, lv_color_hex(0x3f8dea));
    lv_style_set_bg_opa(&ui->dropdown_selected_style, LV_OPA_COVER);
    lv_style_set_text_color(&ui->dropdown_selected_style, lv_color_hex(0xffffff));

    lv_style_init(&ui->button_style);
    lv_style_set_bg_color(&ui->button_style, lv_color_hex(0xf2f5fb));
    lv_style_set_bg_opa(&ui->button_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->button_style, 0);
    lv_style_set_radius(&ui->button_style, 16);
    lv_style_set_pad_hor(&ui->button_style, 36);
    lv_style_set_pad_ver(&ui->button_style, 16);
    lv_style_set_text_color(&ui->button_style, lv_color_hex(0x0b0d11));
    lv_style_set_text_font(&ui->button_style, &lv_font_ibm_plex_sans_semibold_24);

    lv_style_init(&ui->secondary_button_style);
    lv_style_set_bg_color(&ui->secondary_button_style, lv_color_white());
    lv_style_set_bg_opa(&ui->secondary_button_style, LV_OPA_10);
    lv_style_set_border_width(&ui->secondary_button_style, 1);
    lv_style_set_border_color(&ui->secondary_button_style, lv_color_white());
    lv_style_set_border_opa(&ui->secondary_button_style, LV_OPA_30);
    lv_style_set_radius(&ui->secondary_button_style, 16);
    lv_style_set_pad_hor(&ui->secondary_button_style, 30);
    lv_style_set_pad_ver(&ui->secondary_button_style, 16);
    lv_style_set_text_color(&ui->secondary_button_style, lv_color_hex(0xeef1f7));
    lv_style_set_text_font(&ui->secondary_button_style, &lv_font_ibm_plex_sans_semibold_24);

    lv_style_init(&ui->button_focus_style);
    lv_style_set_outline_color(&ui->button_focus_style, lv_color_hex(0xeef3fd));
    lv_style_set_outline_opa(&ui->button_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->button_focus_style, 4);
    lv_style_set_outline_pad(&ui->button_focus_style, 3);

    lv_style_init(&ui->button_disabled_style);
    lv_style_set_bg_opa(&ui->button_disabled_style, LV_OPA_30);
    lv_style_set_text_opa(&ui->button_disabled_style, LV_OPA_40);

    lv_style_init(&ui->card_style);
    lv_style_set_bg_color(&ui->card_style, lv_color_hex(0x11151c));
    lv_style_set_bg_grad_color(&ui->card_style, lv_color_hex(0x080a0e));
    lv_style_set_bg_grad_dir(&ui->card_style, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&ui->card_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->card_style, 1);
    lv_style_set_border_color(&ui->card_style, lv_color_white());
    lv_style_set_border_opa(&ui->card_style, LV_OPA_10);
    lv_style_set_radius(&ui->card_style, 18);
    lv_style_set_shadow_width(&ui->card_style, 24);
    lv_style_set_shadow_ofs_y(&ui->card_style, 8);
    lv_style_set_shadow_color(&ui->card_style, lv_color_black());
    lv_style_set_shadow_opa(&ui->card_style, LV_OPA_40);

    lv_style_init(&ui->card_focus_style);
    lv_style_set_outline_color(&ui->card_focus_style, lv_color_hex(0xeef3fd));
    lv_style_set_outline_opa(&ui->card_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->card_focus_style, 4);
    lv_style_set_outline_pad(&ui->card_focus_style, 3);

    lv_style_init(&ui->status_style);
    lv_style_set_text_color(&ui->status_style, lv_color_hex(0xe9eef8));
    lv_style_set_text_opa(&ui->status_style, LV_OPA_60);
    lv_style_set_text_font(&ui->status_style, &lv_font_ibm_plex_sans_regular_20);
}

static void ui_reset_styles(NativePreconnectUi *ui) {
    lv_style_reset(&ui->root_style);
    lv_style_reset(&ui->nav_style);
    lv_style_reset(&ui->detail_style);
    lv_style_reset(&ui->title_style);
    lv_style_reset(&ui->hero_title_style);
    lv_style_reset(&ui->label_style);
    lv_style_reset(&ui->eyebrow_style);
    lv_style_reset(&ui->muted_style);
    lv_style_reset(&ui->input_style);
    lv_style_reset(&ui->input_focus_style);
    lv_style_reset(&ui->input_cursor_style);
    lv_style_reset(&ui->dropdown_style);
    lv_style_reset(&ui->dropdown_focus_style);
    lv_style_reset(&ui->dropdown_list_style);
    lv_style_reset(&ui->dropdown_selected_style);
    lv_style_reset(&ui->button_style);
    lv_style_reset(&ui->secondary_button_style);
    lv_style_reset(&ui->button_focus_style);
    lv_style_reset(&ui->button_disabled_style);
    lv_style_reset(&ui->card_style);
    lv_style_reset(&ui->card_focus_style);
    lv_style_reset(&ui->status_style);
}

static lv_obj_t *ui_make_label(lv_obj_t *parent, const char *text, lv_style_t *style) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_add_style(label, style, 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *ui_make_dropdown(NativePreconnectUi *ui, lv_obj_t *parent, lv_coord_t width) {
    lv_obj_t *dropdown = lv_dropdown_create(parent);
    lv_obj_remove_style_all(dropdown);
    lv_obj_add_style(dropdown, &ui->dropdown_style, 0);
    lv_obj_add_style(dropdown, &ui->dropdown_focus_style, LV_STATE_FOCUSED);
    lv_obj_set_size(dropdown, width, 58);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(0xf6f8fb), LV_PART_INDICATOR);
    /* JetBrains Mono intentionally contains no FontAwesome private-use glyphs. Keep
     * LVGL's dropdown chevron on the built-in icon-capable font. */
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_20, LV_PART_INDICATOR);
    lv_obj_set_style_pad_right(dropdown, 12, LV_PART_INDICATOR);
    lv_dropdown_set_symbol(dropdown, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(dropdown, LV_DIR_BOTTOM);
    lv_dropdown_set_selected_highlight(dropdown, true);
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    lv_obj_remove_style_all(list);
    lv_obj_add_style(list, &ui->dropdown_list_style, 0);
    lv_obj_add_style(list, &ui->dropdown_selected_style, LV_PART_SELECTED);
    return dropdown;
}

/* Dropdown option order must match NATIVE_AUDIO_CODEC_AUTO (0) and NATIVE_AUDIO_CODEC_PCM (1). */
static uint16_t ui_current_audio_codec(const NativePreconnectUi *ui) {
    if (!ui->audio_codec_dropdown) {
        return NATIVE_AUDIO_CODEC_AUTO;
    }
    return lv_dropdown_get_selected(ui->audio_codec_dropdown) == 1 ? NATIVE_AUDIO_CODEC_PCM
                                                                   : NATIVE_AUDIO_CODEC_AUTO;
}

static bool ui_profile_dirty(const NativePreconnectUi *ui, int slot) {
    const NativeSessionConfig *draft = &ui->slot_values[slot];
    const NativeSessionConfig *saved = &ui->committed_values[slot];
    return strcmp(draft->name, saved->name) != 0 || strcmp(draft->host, saved->host) != 0 ||
           strcmp(draft->username, saved->username) != 0 || strcmp(draft->password, saved->password) != 0 ||
           strcmp(draft->domain, saved->domain) != 0 || draft->port != saved->port || draft->fps != saved->fps ||
           ui_current_audio_codec(ui) != ui->committed_audio_codec;
}

static const char *const UI_SLOT_NAMES[NATIVE_SETTINGS_MAX_SESSIONS] = {"Red", "Green", "Yellow", "Blue"};
static const char *const UI_SLOT_NAMES_UPPER[NATIVE_SETTINGS_MAX_SESSIONS] = {"RED", "GREEN", "YELLOW", "BLUE"};

static lv_obj_t *ui_make_box(lv_obj_t *parent, int x, int y, int width, int height) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_color_t ui_color_with_alpha(uint32_t rgb, lv_opa_t alpha) {
    lv_color_t color = lv_color_hex(rgb);
    LV_COLOR_SET_A(color, alpha);
    return color;
}

static lv_obj_t *ui_make_alpha_gradient(lv_obj_t *parent, int x, int y, int width, int height,
                                        lv_grad_dir_t direction, lv_opa_t start_alpha,
                                        lv_opa_t end_alpha, lv_grad_dsc_t *gradient) {
    memset(gradient, 0, sizeof(*gradient));
    gradient->dir = direction;
    gradient->stops_count = 2;
    gradient->stops[0].color = ui_color_with_alpha(UI_HUB_MASK_COLOR, start_alpha);
    gradient->stops[0].frac = 0;
    gradient->stops[1].color = ui_color_with_alpha(UI_HUB_MASK_COLOR, end_alpha);
    gradient->stops[1].frac = 255;

    lv_obj_t *layer = ui_make_box(parent, x, y, width, height);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad(layer, gradient, 0);
    return layer;
}

typedef struct UiCubeVec3 {
    float x;
    float y;
    float z;
} UiCubeVec3;

typedef struct UiCubeFace {
    uint8_t vertices[4];
    UiCubeVec3 normal;
    uint32_t color;
    float depth;
} UiCubeFace;

static UiCubeVec3 ui_cube_rotate(UiCubeVec3 point, float sin_y, float cos_y) {
    const float sin_x = -0.43837115f; /* rotateX(-26deg), matching the HTML prototype */
    const float cos_x = 0.89879405f;
    float x1 = point.x * cos_y + point.z * sin_y;
    float z1 = -point.x * sin_y + point.z * cos_y;
    UiCubeVec3 result = {x1, point.y * cos_x - z1 * sin_x, point.y * sin_x + z1 * cos_x};
    return result;
}

static void ui_brand_cube_draw(lv_event_t *event) {
    NativeUiCube *cube = (NativeUiCube *)lv_event_get_user_data(event);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    if (!cube || !cube->obj || !draw_ctx) {
        return;
    }

    static const UiCubeVec3 base_vertices[8] = {
        {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, -1.0f}, {-1.0f, 1.0f, -1.0f},
        {-1.0f, -1.0f, 1.0f},  {1.0f, -1.0f, 1.0f},  {1.0f, 1.0f, 1.0f},  {-1.0f, 1.0f, 1.0f},
    };
    static const UiCubeFace face_templates[6] = {
        {{4, 5, 6, 7}, {0.0f, 0.0f, 1.0f}, NATIVE_UI_SLOT_RED_RGB, 0.0f}, /* front / red */
        {{5, 1, 2, 6}, {1.0f, 0.0f, 0.0f}, NATIVE_UI_SLOT_BLUE_RGB, 0.0f}, /* right / blue */
        {{1, 0, 3, 2}, {0.0f, 0.0f, -1.0f}, NATIVE_UI_SLOT_GREEN_RGB, 0.0f}, /* back / green */
        {{0, 4, 7, 3}, {-1.0f, 0.0f, 0.0f}, NATIVE_UI_SLOT_YELLOW_RGB, 0.0f}, /* left / yellow */
        {{0, 1, 5, 4}, {0.0f, -1.0f, 0.0f}, 0x2a303c, 0.0f}, /* top */
        {{7, 6, 2, 3}, {0.0f, 1.0f, 0.0f}, 0x12151b, 0.0f}, /* bottom */
    };

    float radians = (float)cube->angle_tenths * (3.14159265359f / 1800.0f);
    float sin_y = sinf(radians);
    float cos_y = cosf(radians);
    UiCubeVec3 vertices[8];
    for (size_t i = 0; i < 8; i++) {
        vertices[i] = ui_cube_rotate(base_vertices[i], sin_y, cos_y);
    }

    UiCubeFace visible[6];
    size_t visible_count = 0;
    for (size_t i = 0; i < 6; i++) {
        UiCubeVec3 normal = ui_cube_rotate(face_templates[i].normal, sin_y, cos_y);
        if (normal.z <= 0.001f) {
            continue;
        }
        visible[visible_count] = face_templates[i];
        visible[visible_count].depth = 0.0f;
        for (size_t p = 0; p < 4; p++) {
            visible[visible_count].depth += vertices[visible[visible_count].vertices[p]].z;
        }
        visible[visible_count].depth *= 0.25f;
        visible_count++;
    }
    for (size_t i = 1; i < visible_count; i++) {
        UiCubeFace face = visible[i];
        size_t j = i;
        while (j > 0 && visible[j - 1].depth > face.depth) {
            visible[j] = visible[j - 1];
            j--;
        }
        visible[j] = face;
    }

    lv_area_t area;
    lv_obj_get_coords(cube->obj, &area);
    float center_x = ((float)area.x1 + (float)area.x2) * 0.5f;
    float center_y = ((float)area.y1 + (float)area.y2) * 0.5f;
    for (size_t i = 0; i < visible_count; i++) {
        lv_point_t points[4];
        for (size_t p = 0; p < 4; p++) {
            UiCubeVec3 point = vertices[visible[i].vertices[p]];
            points[p].x = (lv_coord_t)lroundf(center_x + point.x * cube->half_edge);
            points[p].y = (lv_coord_t)lroundf(center_y + point.y * cube->half_edge);
        }
        lv_draw_rect_dsc_t draw_dsc;
        lv_draw_rect_dsc_init(&draw_dsc);
        draw_dsc.bg_color = lv_color_hex(visible[i].color);
        draw_dsc.bg_opa = LV_OPA_COVER;
        lv_draw_polygon(draw_ctx, &draw_dsc, points, 4);
    }
}

static void ui_brand_cube_anim_exec(void *object, int32_t angle_tenths) {
    lv_obj_t *obj = (lv_obj_t *)object;
    NativeUiCube *cube = obj ? (NativeUiCube *)lv_obj_get_user_data(obj) : NULL;
    if (!cube) {
        return;
    }
    cube->angle_tenths = angle_tenths;
    lv_obj_invalidate(obj);
}

static void ui_make_brand_cube(lv_obj_t *parent, int x, int y, int size, float half_edge, NativeUiCube *cube) {
    memset(cube, 0, sizeof(*cube));
    cube->half_edge = half_edge;
    cube->obj = ui_make_box(parent, x, y, size, size);
    lv_obj_clear_flag(cube->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(cube->obj, cube);
    lv_obj_add_event_cb(cube->obj, ui_brand_cube_draw, LV_EVENT_DRAW_MAIN, cube);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, cube->obj);
    lv_anim_set_exec_cb(&animation, ui_brand_cube_anim_exec);
    lv_anim_set_values(&animation, 0, 3599);
    lv_anim_set_time(&animation, 14000);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&animation);
}

static void ui_make_wordmark(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, const lv_font_t *font,
                             lv_opa_t underscore_opa) {
    lv_obj_t *word = ui_make_label(parent, "gnomecast", &ui->title_style);
    lv_obj_set_pos(word, x, y);
    lv_obj_set_style_text_font(word, font, 0);
    lv_obj_set_style_text_letter_space(word, -1, 0);
    lv_obj_update_layout(word);

    lv_obj_t *underscore = ui_make_label(parent, "_", &ui->title_style);
    lv_obj_set_pos(underscore, x + lv_obj_get_width(word), y);
    lv_obj_set_style_text_font(underscore, font, 0);
    lv_obj_set_style_text_letter_space(underscore, -1, 0);
    lv_obj_set_style_text_opa(underscore, underscore_opa, 0);
}

static lv_obj_t *ui_make_audio_indicator(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, bool compact,
                                         lv_obj_t *meter_clips[UI_CARD_AUDIO_METER_COUNT],
                                         lv_obj_t *meter_grads[UI_CARD_AUDIO_METER_COUNT], lv_obj_t **label_out) {
    lv_obj_t *group = ui_make_box(parent, x, y, compact ? 40 : 600, compact ? 38 : 30);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_CLICKABLE);
    if (compact) {
        lv_obj_set_style_bg_color(group, lv_color_hex(0x040508), 0);
        lv_obj_set_style_bg_opa(group, LV_OPA_50, 0);
        lv_obj_set_style_radius(group, 8, 0);
        lv_obj_set_style_clip_corner(group, true, 0);
        for (int side = 0; side < UI_CARD_AUDIO_METER_COUNT; side++) {
            int meter_x = side == 0 ? 11 : 23;
            lv_obj_t *rail = ui_make_box(group, meter_x, UI_CARD_AUDIO_METER_TOP,
                                         UI_CARD_AUDIO_METER_WIDTH, UI_CARD_AUDIO_METER_HEIGHT);
            lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(rail, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(rail, (lv_opa_t)15, 0);
            lv_obj_set_style_radius(rail, 3, 0);

            lv_obj_t *clip = ui_make_box(group, meter_x, UI_CARD_AUDIO_METER_BASELINE,
                                         UI_CARD_AUDIO_METER_WIDTH, 1);
            lv_obj_clear_flag(clip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
            lv_obj_set_style_radius(clip, 2, 0);
            lv_obj_set_style_clip_corner(clip, true, 0);
            lv_obj_set_style_pad_all(clip, 0, 0);
            lv_obj_add_flag(clip, LV_OBJ_FLAG_HIDDEN);

            lv_obj_t *grad = ui_make_box(clip, 0, -UI_CARD_AUDIO_METER_HEIGHT,
                                         UI_CARD_AUDIO_METER_WIDTH, UI_CARD_AUDIO_METER_HEIGHT);
            lv_obj_clear_flag(grad, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(grad, lv_color_hex(0xf7d038), 0);
            lv_obj_set_style_bg_grad_color(grad, lv_color_hex(0x27cf5a), 0);
            lv_obj_set_style_bg_grad_dir(grad, LV_GRAD_DIR_VER, 0);
            lv_obj_set_style_bg_opa(grad, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(grad, 3, 0);

            if (meter_clips) {
                meter_clips[side] = clip;
            }
            if (meter_grads) {
                meter_grads[side] = grad;
            }
        }
    } else {
        lv_obj_t *label = ui_make_label(group, "", &ui->muted_style);
        lv_obj_set_pos(label, 0, -2);
        lv_obj_set_style_text_color(label, lv_color_hex(0x7ccd8e), 0);
        lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
        if (label_out) {
            *label_out = label;
        }
    }
    return group;
}

static void ui_update_card_audio_meters(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }

    const uint32_t ticks = SDL_GetTicks();
    const float fall_db = ui->card_audio_meter_ticks == 0
                              ? 0.0f
                              : (float)(ticks - ui->card_audio_meter_ticks) *
                                    (NATIVE_MIXER_METER_DECAY_DB_S / 1000.0f);
    ui->card_audio_meter_ticks = ticks;

    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (!ui->card_audio_groups[slot]) {
            continue;
        }
        bool group_hidden = lv_obj_has_flag(ui->card_audio_groups[slot], LV_OBJ_FLAG_HIDDEN);
        for (int side = 0; side < UI_CARD_AUDIO_METER_COUNT; side++) {
            lv_obj_t *clip = ui->card_audio_meter_clips[slot][side];
            if (!ui->slot_audio_stream_open[slot]) {
                ui->card_audio_meter_db[slot][side] = NATIVE_MIXER_METER_FLOOR_DB;
                if (ui->card_audio_meter_px[slot][side] != 0) {
                    ui->card_audio_meter_px[slot][side] = 0;
                    lv_obj_add_flag(clip, LV_OBJ_FLAG_HIDDEN);
                }
                continue;
            }
            if (group_hidden) {
                continue;
            }
            const int32_t peak = ui->slot_audio_peaks[slot][side];
            const float target_db = peak > 0 ? 20.0f * log10f((float)peak / 32768.0f)
                                             : NATIVE_MIXER_METER_FLOOR_DB;
            const float fallen_db = ui->card_audio_meter_db[slot][side] - fall_db;
            float level_db = target_db > fallen_db ? target_db : fallen_db;
            if (level_db < NATIVE_MIXER_METER_FLOOR_DB) {
                level_db = NATIVE_MIXER_METER_FLOOR_DB;
            }
            ui->card_audio_meter_db[slot][side] = level_db;

            int height = 0;
            if (level_db > (float)NATIVE_MIXER_FADER_MIN_DB) {
                float scaled = (level_db - (float)NATIVE_MIXER_FADER_MIN_DB) *
                               (float)UI_CARD_AUDIO_METER_HEIGHT /
                               (float)(NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB);
                height = (int)lroundf(scaled);
                if (height > UI_CARD_AUDIO_METER_HEIGHT) {
                    height = UI_CARD_AUDIO_METER_HEIGHT;
                }
                if (height < 2) {
                    height = 0;
                }
            }
            if (height == ui->card_audio_meter_px[slot][side]) {
                continue;
            }
            ui->card_audio_meter_px[slot][side] = height;
            if (height == 0) {
                lv_obj_add_flag(clip, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_clear_flag(clip, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(clip, UI_CARD_AUDIO_METER_BASELINE - height);
            lv_obj_set_height(clip, height);
            lv_obj_set_y(ui->card_audio_meter_grads[slot][side], height - UI_CARD_AUDIO_METER_HEIGHT);
        }
    }
}

static lv_obj_t *ui_make_button(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, int width, int height,
                                const char *text, bool primary, lv_event_cb_t callback) {
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, primary ? &ui->button_style : &ui->secondary_button_style, 0);
    lv_obj_add_style(button, &ui->button_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(button, &ui->button_disabled_style, LV_STATE_DISABLED);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    if (callback) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, ui);
    }
    lv_obj_add_event_cb(button, ui_form_key_event, UI_FORM_KEY_EVENT, ui);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *ui_make_field_label(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, int width,
                                     const char *text) {
    lv_obj_t *label = ui_make_label(parent, text, &ui->eyebrow_style);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, &lv_font_ibm_plex_mono_semibold_18, 0);
    return label;
}

static lv_obj_t *ui_make_input(NativePreconnectUi *ui, lv_obj_t *parent, int x, int y, int width, const char *text,
                               const char *placeholder, size_t max_length, const char *accepted, bool password) {
    lv_obj_t *input = lv_textarea_create(parent);
    lv_obj_remove_style_all(input);
    lv_obj_add_style(input, &ui->input_style, 0);
    lv_obj_add_style(input, &ui->input_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(input, &ui->input_cursor_style, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_pos(input, x, y);
    lv_obj_set_size(input, width, 58);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_max_length(input, (uint32_t)max_length);
    if (accepted) {
        lv_textarea_set_accepted_chars(input, accepted);
    }
    lv_textarea_set_password_mode(input, password);
    lv_textarea_set_placeholder_text(input, placeholder ? placeholder : "");
    lv_textarea_set_text(input, text ? text : "");
    lv_textarea_set_cursor_pos(input, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_event_cb(input, ui_input_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(input, ui_form_key_event, UI_FORM_KEY_EVENT, ui);
    return input;
}

static void ui_build(NativePreconnectUi *ui, const char *host, uint16_t port, const char *username, const char *password,
                     const char *domain, uint16_t fps, uint16_t audio_codec) {
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    ui->root = lv_obj_create(screen);
    lv_obj_remove_style_all(ui->root);
    lv_obj_add_style(ui->root, &ui->root_style, 0);
    lv_obj_set_size(ui->root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mask = ui_make_box(ui->root, 0, 0, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(mask, LV_OPA_TRANSP, 0);

    int h22 = UI_HUB_X_AT_PERCENT(22);
    int h42 = UI_HUB_X_AT_PERCENT(42);
    int h62 = UI_HUB_X_AT_PERCENT(62);
    int h82 = UI_HUB_X_AT_PERCENT(82);
    (void)ui_make_alpha_gradient(mask, 0, 0, h22, UI_CANVAS_HEIGHT, LV_GRAD_DIR_HOR,
                                 (lv_opa_t)245, (lv_opa_t)230, &ui->hub_horizontal_gradients[0]);
    (void)ui_make_alpha_gradient(mask, h22, 0, h42 - h22, UI_CANVAS_HEIGHT, LV_GRAD_DIR_HOR,
                                 (lv_opa_t)230, (lv_opa_t)158, &ui->hub_horizontal_gradients[1]);
    (void)ui_make_alpha_gradient(mask, h42, 0, h62 - h42, UI_CANVAS_HEIGHT, LV_GRAD_DIR_HOR,
                                 (lv_opa_t)158, (lv_opa_t)56, &ui->hub_horizontal_gradients[2]);
    (void)ui_make_alpha_gradient(mask, h62, 0, h82 - h62, UI_CANVAS_HEIGHT, LV_GRAD_DIR_HOR,
                                 (lv_opa_t)56, LV_OPA_TRANSP, &ui->hub_horizontal_gradients[3]);

    int v74 = UI_HUB_Y_FROM_BOTTOM_PERCENT(74);
    int v60 = UI_HUB_Y_FROM_BOTTOM_PERCENT(60);
    int v45 = UI_HUB_Y_FROM_BOTTOM_PERCENT(45);
    int v28 = UI_HUB_Y_FROM_BOTTOM_PERCENT(28);
    (void)ui_make_alpha_gradient(mask, 0, v74, UI_CANVAS_WIDTH, v60 - v74, LV_GRAD_DIR_VER,
                                 LV_OPA_TRANSP, (lv_opa_t)46, &ui->hub_vertical_gradients[0]);
    (void)ui_make_alpha_gradient(mask, 0, v60, UI_CANVAS_WIDTH, v45 - v60, LV_GRAD_DIR_VER,
                                 (lv_opa_t)46, (lv_opa_t)128, &ui->hub_vertical_gradients[1]);
    (void)ui_make_alpha_gradient(mask, 0, v45, UI_CANVAS_WIDTH, v28 - v45, LV_GRAD_DIR_VER,
                                 (lv_opa_t)128, (lv_opa_t)204, &ui->hub_vertical_gradients[2]);
    (void)ui_make_alpha_gradient(mask, 0, v28, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT - v28,
                                 LV_GRAD_DIR_VER, (lv_opa_t)204, (lv_opa_t)235,
                                 &ui->hub_vertical_gradients[3]);

    ui_make_brand_cube(ui->root, 96, 44, UI_BRAND_CUBE_SIZE, 10.0f, &ui->brand_cube);
    ui_make_wordmark(ui, ui->root, 146, 48, &lv_font_jetbrains_mono_semibold_28, LV_OPA_40);

    ui->keyboard_pill = ui_make_box(ui->root, 1622, 46, 64, 46);
    lv_obj_add_style(ui->keyboard_pill, &ui->nav_style, 0);
    lv_obj_set_style_pad_all(ui->keyboard_pill, 0, 0);
    lv_obj_t *keyboard_icon = ui_make_label(ui->keyboard_pill, LV_SYMBOL_KEYBOARD, &ui->eyebrow_style);
    lv_obj_set_style_text_font(keyboard_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(keyboard_icon, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_text_opa(keyboard_icon, LV_OPA_80, 0);
    lv_obj_set_style_text_letter_space(keyboard_icon, 0, 0);
    lv_obj_align(keyboard_icon, LV_ALIGN_LEFT_MID, 10, 0);
    ui->keyboard_dot = ui_make_box(ui->keyboard_pill, 48, 18, 10, 10);
    lv_obj_set_style_bg_opa(ui->keyboard_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui->keyboard_dot, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *mouse_pill = ui_make_box(ui->root, 1700, 46, 64, 46);
    lv_obj_add_style(mouse_pill, &ui->nav_style, 0);
    lv_obj_set_style_pad_all(mouse_pill, 0, 0);
    lv_obj_t *mouse_body = ui_make_box(mouse_pill, 12, 8, 20, 30);
    lv_obj_set_style_bg_opa(mouse_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mouse_body, 2, 0);
    lv_obj_set_style_border_color(mouse_body, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_border_opa(mouse_body, LV_OPA_80, 0);
    lv_obj_set_style_radius(mouse_body, 10, 0);
    lv_obj_t *mouse_wheel = ui_make_box(mouse_body, 7, 5, 4, 8);
    lv_obj_set_style_bg_color(mouse_wheel, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_bg_opa(mouse_wheel, LV_OPA_80, 0);
    lv_obj_set_style_radius(mouse_wheel, LV_RADIUS_CIRCLE, 0);
    ui->mouse_dot = ui_make_box(mouse_pill, 48, 18, 10, 10);
    lv_obj_set_style_bg_color(ui->mouse_dot, lv_color_hex(0x43d47e), 0);
    lv_obj_set_style_bg_opa(ui->mouse_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui->mouse_dot, LV_RADIUS_CIRCLE, 0);

    ui->help_btn = ui_make_button(ui, ui->root, 1778, 46, 46, 46, "?", false, ui_help_clicked);
    lv_obj_set_style_pad_all(ui->help_btn, 0, 0);
    lv_obj_set_style_radius(ui->help_btn, LV_RADIUS_CIRCLE, 0);

    ui->keyboard_warning = ui_make_box(ui->root, 96, 116, 940, 62);
    lv_obj_set_style_bg_color(ui->keyboard_warning, lv_color_hex(0xe8c15a), 0);
    lv_obj_set_style_bg_opa(ui->keyboard_warning, LV_OPA_20, 0);
    lv_obj_set_style_border_width(ui->keyboard_warning, 1, 0);
    lv_obj_set_style_border_color(ui->keyboard_warning, lv_color_hex(0xe8c15a), 0);
    lv_obj_set_style_border_opa(ui->keyboard_warning, LV_OPA_40, 0);
    lv_obj_set_style_radius(ui->keyboard_warning, 14, 0);
    ui->keyboard_warning_label = ui_make_label(
        ui->keyboard_warning, "No USB keyboard detected - connect one to type on the remote desktop.", &ui->label_style);
    lv_obj_set_pos(ui->keyboard_warning_label, 24, 7);
    lv_obj_set_style_text_color(ui->keyboard_warning_label, lv_color_hex(0xf0e3bd), 0);
    lv_obj_set_style_text_font(ui->keyboard_warning_label, &lv_font_ibm_plex_sans_regular_24, 0);
    lv_obj_add_flag(ui->keyboard_warning, LV_OBJ_FLAG_HIDDEN);

    ui->hero_chip = ui_make_box(ui->root, 96, 238, 26, 26);
    lv_obj_set_style_bg_opa(ui->hero_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui->hero_chip, 8, 0);
    lv_obj_set_style_shadow_width(ui->hero_chip, 24, 0);
    lv_obj_set_style_shadow_opa(ui->hero_chip, LV_OPA_60, 0);
    ui->hero_slot_label = ui_make_label(ui->root, "RED", &ui->eyebrow_style);
    lv_obj_set_pos(ui->hero_slot_label, 136, 237);
    lv_obj_set_style_text_font(ui->hero_slot_label, &lv_font_ibm_plex_mono_semibold_18, 0);
    lv_obj_set_style_text_opa(ui->hero_slot_label, LV_OPA_50, 0);
    ui->hero_name_label = ui_make_label(ui->root, "RED", &ui->hero_title_style);
    lv_obj_set_pos(ui->hero_name_label, 96, 282);
    lv_obj_set_width(ui->hero_name_label, 900);
    lv_label_set_long_mode(ui->hero_name_label, LV_LABEL_LONG_DOT);
    ui->hero_state_dot = ui_make_box(ui->root, 96, 403, 16, 16);
    lv_obj_set_style_bg_opa(ui->hero_state_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui->hero_state_dot, LV_RADIUS_CIRCLE, 0);
    ui->hero_state_label = ui_make_label(ui->root, "Not set up", &ui->title_style);
    lv_obj_set_pos(ui->hero_state_label, 126, 392);
    lv_obj_set_style_text_font(ui->hero_state_label, &lv_font_ibm_plex_mono_semibold_28, 0);
    lv_obj_set_style_text_letter_space(ui->hero_state_label, 0, 0);
    ui->hero_meta_label = ui_make_label(ui->root, "Assign a computer to this colour key.", &ui->muted_style);
    lv_obj_set_pos(ui->hero_meta_label, 96, 438);
    lv_obj_set_width(ui->hero_meta_label, 820);
    lv_label_set_long_mode(ui->hero_meta_label, LV_LABEL_LONG_WRAP);
    ui->hero_audio_group = ui_make_audio_indicator(ui, ui->root, 96, 482, false, NULL, NULL,
                                                    &ui->hero_audio_label);
    lv_obj_add_flag(ui->hero_audio_group, LV_OBJ_FLAG_HIDDEN);
    ui->hero_detail_panel = ui_make_box(ui->root, 96, 484, 820, 74);
    lv_obj_set_style_bg_color(ui->hero_detail_panel, lv_color_hex(0xe35d55), 0);
    lv_obj_set_style_bg_opa(ui->hero_detail_panel, LV_OPA_20, 0);
    lv_obj_set_style_border_width(ui->hero_detail_panel, 1, 0);
    lv_obj_set_style_border_color(ui->hero_detail_panel, lv_color_hex(0xe35d55), 0);
    lv_obj_set_style_border_opa(ui->hero_detail_panel, LV_OPA_40, 0);
    lv_obj_set_style_radius(ui->hero_detail_panel, 14, 0);
    ui->hero_detail_label = ui_make_label(ui->hero_detail_panel, "", &ui->label_style);
    lv_obj_set_pos(ui->hero_detail_label, 20, 10);
    lv_obj_set_width(ui->hero_detail_label, 780);
    lv_label_set_long_mode(ui->hero_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(ui->hero_detail_label, &lv_font_ibm_plex_sans_regular_24, 0);
    lv_obj_add_flag(ui->hero_detail_panel, LV_OBJ_FLAG_HIDDEN);
    ui->hero_action_btn = ui_make_button(ui, ui->root, 96, 590, 220, 72, "Set up", true, ui_hero_action_clicked);
    ui->hero_action_label = lv_obj_get_child(ui->hero_action_btn, 0);
    ui->hero_edit_btn = ui_make_button(ui, ui->root, 334, 590, 150, 72, "Edit", false, ui_edit_clicked);
    lv_obj_set_style_text_font(ui->hero_action_label, &lv_font_ibm_plex_sans_semibold_24, 0);
    lv_obj_set_style_text_letter_space(ui->hero_action_label, 0, 0);
    lv_obj_t *hero_edit_label = lv_obj_get_child(ui->hero_edit_btn, 0);
    lv_obj_set_style_text_font(hero_edit_label, &lv_font_ibm_plex_sans_semibold_24, 0);
    lv_obj_set_style_text_letter_space(hero_edit_label, 0, 0);

    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        int card_x = 96 + slot * (411 + UI_CARD_GAP);
        lv_obj_t *card = lv_btn_create(ui->root);
        lv_obj_remove_style_all(card);
        lv_obj_add_style(card, &ui->card_style, 0);
        lv_obj_add_style(card, &ui->card_focus_style, LV_STATE_FOCUSED);
        lv_obj_add_style(card, &ui->card_focus_style, LV_STATE_CHECKED);
        lv_obj_set_pos(card, card_x, 752);
        lv_obj_set_size(card, 411, UI_CARD_HEIGHT);
        lv_obj_set_style_bg_color(card, lv_color_mix(lv_color_hex(native_ui_slot_rgb(slot)), lv_color_hex(0x10131a),
                                                     LV_OPA_20), 0);
        lv_obj_add_event_cb(card, ui_slot_button_clicked, LV_EVENT_CLICKED, ui);
        lv_obj_add_event_cb(card, ui_slot_button_focused, LV_EVENT_FOCUSED, ui);
        lv_obj_add_event_cb(card, ui_hub_key_event, UI_FORM_KEY_EVENT, ui);
        ui->slot_buttons[slot] = card;

        ui->card_tabs[slot] = ui_make_box(card, 20, 18, 54, 8);
        lv_obj_clear_flag(ui->card_tabs[slot], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(ui->card_tabs[slot], lv_color_hex(native_ui_slot_rgb(slot)), 0);
        lv_obj_set_style_bg_opa(ui->card_tabs[slot], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ui->card_tabs[slot], 4, 0);

        ui->card_audio_groups[slot] = ui_make_audio_indicator(
            ui, card, 207, 14, true, ui->card_audio_meter_clips[slot],
            ui->card_audio_meter_grads[slot], NULL);
        for (int side = 0; side < UI_CARD_AUDIO_METER_COUNT; side++) {
            ui->card_audio_meter_db[slot][side] = NATIVE_MIXER_METER_FLOOR_DB;
        }
        lv_obj_add_flag(ui->card_audio_groups[slot], LV_OBJ_FLAG_HIDDEN);

        ui->card_badges[slot] = ui_make_box(card, 255, 14, 140, 38);
        lv_obj_clear_flag(ui->card_badges[slot], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(ui->card_badges[slot], 9, 0);
        ui->card_badge_labels[slot] = ui_make_label(ui->card_badges[slot], "NOT SET UP", &ui->eyebrow_style);
        lv_obj_set_style_text_font(ui->card_badge_labels[slot], &lv_font_ibm_plex_mono_semibold_18, 0);
        lv_obj_set_style_text_letter_space(ui->card_badge_labels[slot], 1, 0);
        lv_obj_center(ui->card_badge_labels[slot]);

        lv_obj_t *number = ui_make_box(card, 20, 168, 30, 30);
        lv_obj_clear_flag(number, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(number, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(number, LV_OPA_10, 0);
        lv_obj_set_style_border_width(number, 1, 0);
        lv_obj_set_style_border_color(number, lv_color_white(), 0);
        lv_obj_set_style_border_opa(number, LV_OPA_20, 0);
        lv_obj_set_style_radius(number, 8, 0);
        lv_obj_t *number_label = ui_make_label(number, "", &ui->label_style);
        lv_label_set_text_fmt(number_label, "%d", slot + 1);
        lv_obj_set_style_text_font(number_label, &lv_font_ibm_plex_mono_semibold_18, 0);
        lv_obj_set_style_text_opa(number_label, LV_OPA_70, 0);
        lv_obj_center(number_label);
        ui->card_name_labels[slot] = ui_make_label(card, UI_SLOT_NAMES[slot], &ui->title_style);
        lv_obj_set_pos(ui->card_name_labels[slot], 64, 166);
        lv_obj_set_style_text_font(ui->card_name_labels[slot], &lv_font_ibm_plex_sans_semibold_24, 0);
        lv_obj_set_style_text_letter_space(ui->card_name_labels[slot], 0, 0);
        lv_obj_set_width(ui->card_name_labels[slot], 320);
        lv_label_set_long_mode(ui->card_name_labels[slot], LV_LABEL_LONG_DOT);
    }

    lv_obj_t *footer = ui_make_label(
        ui->root, "< >  Browse computers       OK  Connect / resume       COLOUR KEYS  Switch instantly",
        &ui->status_style);
    lv_obj_set_pos(footer, 96, 1005);
    lv_obj_set_width(footer, 1728);
    lv_obj_set_style_text_font(footer, &lv_font_ibm_plex_sans_regular_20, 0);

    ui->setup_scrim = ui_make_box(ui->root, 0, 0, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(ui->setup_scrim, lv_color_hex(0x030406), 0);
    lv_obj_set_style_bg_opa(ui->setup_scrim, LV_OPA_70, 0);
    lv_obj_add_event_cb(ui->setup_scrim, ui_setup_scrim_clicked, LV_EVENT_CLICKED, ui);
    ui->setup_panel = ui_make_box(ui->setup_scrim, UI_CANVAS_WIDTH - UI_SETUP_PANEL_WIDTH, 0,
                                  UI_SETUP_PANEL_WIDTH, UI_CANVAS_HEIGHT);
    lv_obj_add_style(ui->setup_panel, &ui->detail_style, 0);

    ui->form_title = ui_make_label(ui->setup_panel, "Set up computer", &ui->title_style);
    lv_obj_set_pos(ui->form_title, 48, 38);
    lv_obj_set_style_text_font(ui->form_title, &lv_font_ibm_plex_sans_semibold_40, 0);
    lv_obj_t *setup_help = ui_make_label(ui->setup_panel,
                                        "Enable Remote Desktop in GNOME Settings > Sharing, then enter its credentials.",
                                        &ui->status_style);
    lv_obj_set_pos(setup_help, 48, 88);
    lv_obj_set_width(setup_help, 584);
    lv_label_set_long_mode(setup_help, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(setup_help, &lv_font_ibm_plex_sans_regular_24, 0);

    ui_make_field_label(ui, ui->setup_panel, 48, 150, 584, "NAME");
    ui->name_input = ui_make_input(ui, ui->setup_panel, 48, 174, 584,
                                   ui->slot_values[ui->selected_slot].name, "e.g. Studio PC", UI_NAME_MAX - 1,
                                   native_ui_profile_name_accepted_chars(), false);
    lv_obj_add_event_cb(ui->name_input, ui_profile_name_insert, LV_EVENT_INSERT, ui);
    ui_make_field_label(ui, ui->setup_panel, 48, 246, 584, "REMOTE COLOUR KEY (FIXED)");
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        ui->color_choices[slot] = ui_make_box(ui->setup_panel, 48 + slot * 68, 272, 52, 52);
        lv_obj_set_style_bg_color(ui->color_choices[slot], lv_color_hex(native_ui_slot_rgb(slot)), 0);
        lv_obj_set_style_radius(ui->color_choices[slot], 15, 0);
    }

    ui_make_field_label(ui, ui->setup_panel, 48, 344, 394, "ADDRESS");
    ui_make_field_label(ui, ui->setup_panel, 458, 344, 174, "PORT");
    ui->host_input = ui_make_input(ui, ui->setup_panel, 48, 368, 394, host, "192.168.1.40", UI_HOST_MAX - 1,
                                   native_ui_host_accepted_chars(), false);
    char port_text[UI_PORT_MAX];
    (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    ui->port_input = ui_make_input(ui, ui->setup_panel, 458, 368, 174, port_text, "3389", UI_PORT_MAX - 1,
                                   "0123456789", false);

    ui_make_field_label(ui, ui->setup_panel, 48, 440, 282, "USERNAME");
    ui_make_field_label(ui, ui->setup_panel, 346, 440, 286, "DOMAIN (OPTIONAL)");
    ui->username_input = ui_make_input(ui, ui->setup_panel, 48, 464, 282, username, "username", UI_USERNAME_MAX - 1,
                                       NULL, false);
    ui->domain_input = ui_make_input(ui, ui->setup_panel, 346, 464, 286, domain, "optional", UI_DOMAIN_MAX - 1,
                                     NULL, false);

    ui_make_field_label(ui, ui->setup_panel, 48, 536, 584, "PASSWORD");
    ui->password_input = ui_make_input(ui, ui->setup_panel, 48, 560, 584, password, "password",
                                       UI_PASSWORD_TEXT_MAX, NULL, true);
    lv_obj_t *password_help = ui_make_label(ui->setup_panel, "Stored privately on this TV when you save.",
                                            &ui->status_style);
    lv_obj_set_pos(password_help, 48, 624);

    ui_make_field_label(ui, ui->setup_panel, 48, 668, 266, "FRAME RATE");
    ui_make_field_label(ui, ui->setup_panel, 330, 668, 302, "AUDIO QUALITY");
    ui->fps_dropdown = ui_make_dropdown(ui, ui->setup_panel, 266);
    lv_obj_set_pos(ui->fps_dropdown, 48, 692);
    size_t selected_fps = ui_select_fps_index(ui, fps);
    ui_set_fps_options(ui);
    ui_set_selected_fps(ui, selected_fps);
    lv_obj_add_event_cb(ui->fps_dropdown, ui_fps_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->fps_dropdown, ui_form_key_event, UI_FORM_KEY_EVENT, ui);
    ui->audio_codec_dropdown = ui_make_dropdown(ui, ui->setup_panel, 302);
    lv_obj_set_pos(ui->audio_codec_dropdown, 330, 692);
    lv_dropdown_set_options_static(ui->audio_codec_dropdown, "Auto (Opus)\nLossless PCM");
    lv_dropdown_set_selected(ui->audio_codec_dropdown, audio_codec == NATIVE_AUDIO_CODEC_PCM ? 1 : 0);
    lv_obj_add_event_cb(ui->audio_codec_dropdown, ui_input_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->audio_codec_dropdown, ui_form_key_event, UI_FORM_KEY_EVENT, ui);

    ui->status_label = ui_make_label(ui->setup_panel, "", &ui->status_style);
    lv_obj_set_pos(ui->status_label, 48, 770);
    lv_obj_set_width(ui->status_label, 584);
    lv_label_set_long_mode(ui->status_label, LV_LABEL_LONG_WRAP);
    ui->delete_btn = ui_make_button(ui, ui->setup_panel, 48, 846, 210, 52, "Delete profile", false,
                                    ui_delete_clicked);
    ui->delete_label = lv_obj_get_child(ui->delete_btn, 0);
    lv_obj_set_style_text_color(ui->delete_label, lv_color_hex(0xf0958f), 0);
    lv_obj_add_flag(ui->delete_btn, LV_OBJ_FLAG_HIDDEN);
    ui->connect_btn = ui_make_button(ui, ui->setup_panel, 48, 944, 270, 74, "Save and connect", true,
                                     ui_connect_clicked);
    ui->save_btn = ui_make_button(ui, ui->setup_panel, 332, 944, 128, 74, "Save", false, ui_save_clicked);
    ui->cancel_btn = ui_make_button(ui, ui->setup_panel, 474, 944, 158, 74, "Cancel", false, ui_cancel_clicked);
    lv_obj_add_flag(ui->setup_scrim, LV_OBJ_FLAG_HIDDEN);

    ui->onboarding_scrim = ui_make_box(ui->root, 0, 0, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT);
    lv_obj_set_style_bg_color(ui->onboarding_scrim, lv_color_hex(0x030406), 0);
    lv_obj_set_style_bg_opa(ui->onboarding_scrim, LV_OPA_90, 0);
    ui_make_brand_cube(ui->onboarding_scrim, 370, 134, UI_ONBOARDING_CUBE_SIZE, 11.0f, &ui->onboarding_cube);
    ui_make_wordmark(ui, ui->onboarding_scrim, 424, 141, &lv_font_jetbrains_mono_semibold_28, LV_OPA_30);
    lv_obj_t *ob_title = ui_make_label(ui->onboarding_scrim,
                                      "Four computers. Four colour keys. One TV.", &ui->title_style);
    lv_obj_set_pos(ob_title, 370, 200);
    lv_obj_set_style_text_font(ob_title, &lv_font_ibm_plex_sans_semibold_40, 0);
    lv_obj_t *ob_subtitle = ui_make_label(
        ui->onboarding_scrim, "GnomeCast turns this TV into a screen and KVM switch for your GNOME desktops.",
        &ui->muted_style);
    lv_obj_set_pos(ob_subtitle, 370, 258);
    lv_obj_set_width(ob_subtitle, 1180);
    lv_obj_set_style_text_font(ob_subtitle, &lv_font_ibm_plex_sans_regular_24, 0);
    static const char *const step_titles[4] = {"Prepare the computer", "Plug input into the TV",
                                               "Colour keys are computers", "Press the active colour again"};
    static const char *const step_text[4] = {
        "Enable GNOME Remote Desktop in Settings > Sharing and note the address, username and password.",
        "A USB keyboard and mouse control the remote desktop directly. The LG remote handles switching.",
        "Red, green, yellow and blue each belong to one computer. Press a colour to switch instantly.",
        "The active colour opens the audio mixer, with one channel per computer and a TV master."};
    for (int step = 0; step < 4; step++) {
        int col = step % 2;
        int row = step / 2;
        lv_obj_t *card = ui_make_box(ui->onboarding_scrim, 370 + col * 601, 332 + row * 220, 579, 198);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_10, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_white(), 0);
        lv_obj_set_style_border_opa(card, LV_OPA_10, 0);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_t *number = ui_make_box(card, 30, 28, 38, 38);
        lv_obj_set_style_bg_color(number, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(number, LV_OPA_10, 0);
        lv_obj_set_style_radius(number, 11, 0);
        lv_obj_t *number_label = ui_make_label(number, "", &ui->label_style);
        lv_label_set_text_fmt(number_label, "%d", step + 1);
        lv_obj_set_style_text_font(number_label, &lv_font_ibm_plex_mono_semibold_18, 0);
        lv_obj_center(number_label);
        lv_obj_t *heading = ui_make_label(card, step_titles[step], &ui->title_style);
        lv_obj_set_pos(heading, 84, 25);
        lv_obj_t *body = ui_make_label(card, step_text[step], &ui->status_style);
        lv_obj_set_pos(body, 30, 84);
        lv_obj_set_width(body, 519);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(body, &lv_font_ibm_plex_sans_regular_24, 0);
    }
    ui->onboarding_btn = ui_make_button(ui, ui->onboarding_scrim, 370, 828, 252, 72, "Get started", true,
                                        ui_onboarding_close_clicked);
    lv_obj_t *ob_hint = ui_make_label(ui->onboarding_scrim, "Reopen anytime from the ? button.", &ui->status_style);
    lv_obj_set_pos(ob_hint, 646, 846);
    lv_obj_set_style_text_font(ob_hint, &lv_font_ibm_plex_sans_regular_20, 0);
    lv_obj_add_flag(ui->onboarding_scrim, LV_OBJ_FLAG_HIDDEN);

    ui->group = lv_group_create();
    ui_update_hub(ui);
    ui_show_setup(ui, false);
    ui_show_onboarding(ui, ui->onboarding_visible);
    ui_update_connect_state(ui);
}

static bool ui_slot_configured(const NativeSessionConfig *values) {
    return values && ui_host_valid(values->host) && values->username[0] && values->password[0];
}

static const char *ui_slot_display_name(const NativePreconnectUi *ui, int slot, char *fallback, size_t fallback_cap) {
    const NativeSessionConfig *values = &ui->slot_values[slot];
    if (values->name[0] && native_ui_profile_name_valid(values->name, sizeof(values->name))) {
        return values->name;
    }
    if (ui_slot_configured(values) && values->host[0]) {
        return values->host;
    }
    (void)snprintf(fallback, fallback_cap, "%s", UI_SLOT_NAMES_UPPER[slot]);
    return fallback;
}

static const char *ui_state_text(NativePreconnectSessionState state) {
    switch (state) {
    case NATIVE_PRECONNECT_SESSION_OFFLINE:
        return "Offline";
    case NATIVE_PRECONNECT_SESSION_CONNECTING:
        return "Connecting...";
    case NATIVE_PRECONNECT_SESSION_CONNECTED:
        return "Connected";
    case NATIVE_PRECONNECT_SESSION_ERROR:
        return "Connection failed";
    case NATIVE_PRECONNECT_SESSION_NOT_SET_UP:
    default:
        return "Not set up";
    }
}

static const char *ui_badge_text(NativePreconnectSessionState state) {
    switch (state) {
    case NATIVE_PRECONNECT_SESSION_OFFLINE:
        return "OFFLINE";
    case NATIVE_PRECONNECT_SESSION_CONNECTING:
        return "CONNECTING";
    case NATIVE_PRECONNECT_SESSION_CONNECTED:
        return "CONNECTED";
    case NATIVE_PRECONNECT_SESSION_ERROR:
        return "ERROR";
    case NATIVE_PRECONNECT_SESSION_NOT_SET_UP:
    default:
        return "NOT SET UP";
    }
}

static uint32_t ui_state_color(NativePreconnectSessionState state) {
    switch (state) {
    case NATIVE_PRECONNECT_SESSION_CONNECTED:
        return 0x43d47e;
    case NATIVE_PRECONNECT_SESSION_CONNECTING:
        return 0xe8c15a;
    case NATIVE_PRECONNECT_SESSION_ERROR:
        return 0xe35d55;
    case NATIVE_PRECONNECT_SESSION_OFFLINE:
    case NATIVE_PRECONNECT_SESSION_NOT_SET_UP:
    default:
        return 0x8a909b;
    }
}

static bool ui_audio_stream_text(const NativePreconnectUi *ui, int slot, char *text, size_t text_cap) {
    if (!ui || !text || text_cap == 0 || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return false;
    }

    if (!ui->slot_audio_stream_open[slot] || ui->slot_audio_sample_rate[slot] == 0 ||
        ui->slot_audio_channels[slot] == 0) {
        (void)snprintf(text, text_cap, "Audio  \xc2\xb7  Waiting for stream");
        return true;
    }

    const char *codec = NULL;
    if (ui->slot_audio_codec[slot] == RDP_AUDIO_CODEC_OPUS) {
        codec = "Opus";
    } else if (ui->slot_audio_codec[slot] == RDP_AUDIO_CODEC_PCM_S16LE) {
        codec = "PCM S16LE";
    } else {
        return false;
    }

    char sample_rate[20];
    uint32_t rate = ui->slot_audio_sample_rate[slot];
    if (rate % 1000u == 0u) {
        (void)snprintf(sample_rate, sizeof(sample_rate), "%u kHz", (unsigned)(rate / 1000u));
    } else if (rate % 100u == 0u) {
        (void)snprintf(sample_rate, sizeof(sample_rate), "%u.%u kHz", (unsigned)(rate / 1000u),
                       (unsigned)((rate % 1000u) / 100u));
    } else if (rate % 10u == 0u) {
        (void)snprintf(sample_rate, sizeof(sample_rate), "%u.%02u kHz", (unsigned)(rate / 1000u),
                       (unsigned)((rate % 1000u) / 10u));
    } else {
        (void)snprintf(sample_rate, sizeof(sample_rate), "%u.%03u kHz", (unsigned)(rate / 1000u),
                       (unsigned)(rate % 1000u));
    }

    char channel_count[16];
    const char *channels = channel_count;
    if (ui->slot_audio_channels[slot] == 1) {
        channels = "Mono";
    } else if (ui->slot_audio_channels[slot] == 2) {
        channels = "Stereo";
    } else {
        (void)snprintf(channel_count, sizeof(channel_count), "%u ch",
                       (unsigned)ui->slot_audio_channels[slot]);
    }
    (void)snprintf(text, text_cap, "%s  \xc2\xb7  %s  \xc2\xb7  %s", codec, sample_rate, channels);
    return true;
}

static void ui_focus_hub(NativePreconnectUi *ui) {
    if (!ui || !ui->group) {
        return;
    }
    int selected = ui->selected_slot;
    ui->rebuilding_group = true;
    lv_group_remove_all_objs(ui->group);
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        lv_group_add_obj(ui->group, ui->slot_buttons[slot]);
    }
    lv_group_add_obj(ui->group, ui->hero_action_btn);
    lv_group_add_obj(ui->group, ui->hero_edit_btn);
    lv_group_add_obj(ui->group, ui->help_btn);
    lv_group_focus_obj(ui->slot_buttons[selected]);
    ui->rebuilding_group = false;
}

static void ui_focus_setup(NativePreconnectUi *ui) {
    lv_group_remove_all_objs(ui->group);
    lv_group_add_obj(ui->group, ui->name_input);
    lv_group_add_obj(ui->group, ui->host_input);
    lv_group_add_obj(ui->group, ui->port_input);
    lv_group_add_obj(ui->group, ui->username_input);
    lv_group_add_obj(ui->group, ui->domain_input);
    lv_group_add_obj(ui->group, ui->password_input);
    lv_group_add_obj(ui->group, ui->fps_dropdown);
    lv_group_add_obj(ui->group, ui->audio_codec_dropdown);
    lv_group_add_obj(ui->group, ui->delete_btn);
    lv_group_add_obj(ui->group, ui->connect_btn);
    lv_group_add_obj(ui->group, ui->save_btn);
    lv_group_add_obj(ui->group, ui->cancel_btn);
    lv_group_focus_obj(ui->name_input);
}

static void ui_show_setup(NativePreconnectUi *ui, bool visible) {
    if (!ui || !ui->setup_scrim) {
        return;
    }
    ui->setup_visible = visible;
    if (visible) {
        ui->delete_armed = false;
        if (ui->delete_label) {
            lv_label_set_text(ui->delete_label, "Delete profile");
        }
        lv_obj_clear_flag(ui->setup_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui->setup_scrim);
        if (!ui->onboarding_visible) {
            ui_focus_setup(ui);
        }
    } else {
        lv_obj_add_flag(ui->setup_scrim, LV_OBJ_FLAG_HIDDEN);
        if (!ui->onboarding_visible) {
            ui_focus_hub(ui);
        }
    }
}

static void ui_show_onboarding(NativePreconnectUi *ui, bool visible) {
    if (!ui || !ui->onboarding_scrim || !ui->group) {
        return;
    }
    ui->onboarding_visible = visible;
    if (visible) {
        lv_obj_clear_flag(ui->onboarding_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui->onboarding_scrim);
        lv_group_remove_all_objs(ui->group);
        lv_group_add_obj(ui->group, ui->onboarding_btn);
        lv_group_focus_obj(ui->onboarding_btn);
    } else {
        lv_obj_add_flag(ui->onboarding_scrim, LV_OBJ_FLAG_HIDDEN);
        if (ui->setup_visible) {
            lv_obj_move_foreground(ui->setup_scrim);
            ui_focus_setup(ui);
        } else {
            ui_focus_hub(ui);
        }
    }
}

static void ui_store_form_to_slot(NativePreconnectUi *ui, int slot) {
    if (slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    NativeSessionConfig *values = &ui->slot_values[slot];
    const char *name = lv_textarea_get_text(ui->name_input);
    const char *host = lv_textarea_get_text(ui->host_input);
    const char *port_text = lv_textarea_get_text(ui->port_input);
    const char *username = lv_textarea_get_text(ui->username_input);
    const char *domain = lv_textarea_get_text(ui->domain_input);
    const char *password = lv_textarea_get_text(ui->password_input);
    if (native_ui_profile_name_valid(name, sizeof(values->name))) {
        (void)snprintf(values->name, sizeof(values->name), "%s", name);
    } else {
        values->name[0] = '\0';
    }
    if (!native_ui_host_normalize(host, values->host, sizeof(values->host))) {
        /* Preserve an invalid in-progress edit in the draft model. Validation keeps it
         * out of saved settings and connection requests. */
        (void)snprintf(values->host, sizeof(values->host), "%s", host ? host : "");
    }
    uint16_t parsed_port = 0;
    (void)snprintf(ui->slot_port_text[slot], UI_PORT_MAX, "%s", port_text ? port_text : "");
    ui->slot_port_valid[slot] = ui_parse_port(port_text, &parsed_port);
    if (ui->slot_port_valid[slot]) {
        values->port = parsed_port;
    }
    (void)snprintf(values->username, sizeof(values->username), "%s", username ? username : "");
    (void)snprintf(values->domain, sizeof(values->domain), "%s", domain ? domain : "");
    (void)snprintf(values->password, sizeof(values->password), "%s", password ? password : "");
    values->fps = ui->selected_fps;
    native_ui_mixer_set_profiles(ui->mixer, ui->slot_values);
}

static void ui_discard_form_changes(NativePreconnectUi *ui) {
    if (!ui || ui->selected_slot < 0 || ui->selected_slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    int slot = ui->selected_slot;
    ui->slot_values[slot] = ui->committed_values[slot];
    (void)snprintf(ui->slot_port_text[slot], UI_PORT_MAX, "%u",
                   (unsigned)ui->slot_values[slot].port);
    ui->slot_port_valid[slot] = true;
    ui->loading_form = true;
    lv_dropdown_set_selected(ui->audio_codec_dropdown,
                             ui->committed_audio_codec == NATIVE_AUDIO_CODEC_PCM ? 1 : 0);
    ui->loading_form = false;
    ui->delete_armed = false;
    if (ui->delete_label) {
        lv_label_set_text(ui->delete_label, "Delete profile");
    }
    native_ui_mixer_set_profiles(ui->mixer, ui->slot_values);
}

static void ui_load_slot_into_form(NativePreconnectUi *ui, int slot) {
    if (slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    const NativeSessionConfig *values = &ui->slot_values[slot];
    ui->loading_form = true;
    lv_textarea_set_text(ui->name_input, values->name);
    lv_textarea_set_cursor_pos(ui->name_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_text(ui->host_input, values->host);
    lv_textarea_set_cursor_pos(ui->host_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_text(ui->port_input, ui->slot_port_text[slot]);
    lv_textarea_set_cursor_pos(ui->port_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_text(ui->username_input, values->username);
    lv_textarea_set_cursor_pos(ui->username_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_text(ui->domain_input, values->domain);
    lv_textarea_set_cursor_pos(ui->domain_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_set_text(ui->password_input, values->password);
    lv_textarea_set_cursor_pos(ui->password_input, LV_TEXTAREA_CURSOR_LAST);
    size_t fps_index = ui_select_fps_index(ui, values->fps);
    ui_set_fps_options(ui);
    ui_set_selected_fps(ui, fps_index);
    ui->loading_form = false;
    char title[UI_NAME_MAX + 32u];
    char fallback[32];
    const char *name = ui_slot_display_name(ui, slot, fallback, sizeof(fallback));
    (void)snprintf(title, sizeof(title), "%s %s", ui_slot_configured(values) ? "Edit" : "Set up", name);
    lv_label_set_text(ui->form_title, title);
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        lv_obj_set_style_outline_width(ui->color_choices[i], i == slot ? 4 : 0, 0);
        lv_obj_set_style_outline_color(ui->color_choices[i], lv_color_hex(0xeef3fd), 0);
        lv_obj_set_style_outline_opa(ui->color_choices[i], i == slot ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_outline_pad(ui->color_choices[i], 3, 0);
        lv_obj_set_style_bg_opa(ui->color_choices[i], i == slot ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    if (ui->delete_btn) {
        if (ui_slot_configured(values)) {
            lv_obj_clear_flag(ui->delete_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui->delete_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    ui_update_connect_state(ui);
}

static void ui_update_slot_buttons(NativePreconnectUi *ui) {
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (!ui->slot_buttons[slot]) {
            continue;
        }
        if (slot == ui->selected_slot) {
            lv_obj_add_state(ui->slot_buttons[slot], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(ui->slot_buttons[slot], LV_STATE_CHECKED);
        }
    }
}

static void ui_update_hub(NativePreconnectUi *ui) {
    if (!ui || !ui->hero_name_label) {
        return;
    }
    int selected = ui->selected_slot;
    const NativeSessionConfig *values = &ui->slot_values[selected];
    NativePreconnectSessionState state = ui->slot_states[selected];
    char fallback[32];
    const char *display_name = ui_slot_display_name(ui, selected, fallback, sizeof(fallback));
    lv_label_set_text(ui->hero_slot_label, UI_SLOT_NAMES_UPPER[selected]);
    lv_label_set_text(ui->hero_name_label, display_name);
    lv_label_set_text(ui->hero_state_label, ui_state_text(state));
    lv_obj_set_style_bg_color(ui->hero_chip, lv_color_hex(native_ui_slot_rgb(selected)), 0);
    lv_obj_set_style_shadow_color(ui->hero_chip, lv_color_hex(native_ui_slot_rgb(selected)), 0);
    uint32_t state_color = ui_state_color(state);
    lv_obj_set_style_bg_color(ui->hero_state_dot, lv_color_hex(state_color), 0);
    lv_obj_set_style_shadow_color(ui->hero_state_dot, lv_color_hex(state_color), 0);
    lv_obj_set_style_shadow_width(ui->hero_state_dot, 18, 0);
    lv_obj_set_style_shadow_opa(ui->hero_state_dot,
                                state == NATIVE_PRECONNECT_SESSION_CONNECTED ? LV_OPA_60 : LV_OPA_TRANSP, 0);

    char meta[NATIVE_SETTINGS_STRING_MAX + 96u];
    if (state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP) {
        (void)snprintf(meta, sizeof(meta), "Assign a computer to the %s key to switch to it instantly.",
                       UI_SLOT_NAMES[selected]);
    } else if (state == NATIVE_PRECONNECT_SESSION_CONNECTED && ui->slot_desktop_width[selected] != 0 &&
               ui->slot_desktop_height[selected] != 0) {
        (void)snprintf(meta, sizeof(meta), "%u\xc3\x97%u  \xc2\xb7  %u fps  \xc2\xb7  Session %u min",
                       (unsigned)ui->slot_desktop_width[selected], (unsigned)ui->slot_desktop_height[selected],
                       (unsigned)values->fps, (unsigned)ui->slot_session_minutes[selected]);
    } else {
        (void)snprintf(meta, sizeof(meta), "%s:%u - %u fps", values->host, (unsigned)values->port,
                       (unsigned)values->fps);
    }
    lv_label_set_text(ui->hero_meta_label, meta);
    char audio_meta[80];
    if (state == NATIVE_PRECONNECT_SESSION_CONNECTED &&
        ui_audio_stream_text(ui, selected, audio_meta, sizeof(audio_meta))) {
        lv_label_set_text(ui->hero_audio_label, audio_meta);
        lv_obj_clear_flag(ui->hero_audio_group, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->hero_audio_group, LV_OBJ_FLAG_HIDDEN);
    }
    bool show_detail = ui->slot_details[selected][0] != '\0' && state == NATIVE_PRECONNECT_SESSION_ERROR;
    if (show_detail) {
        lv_label_set_text(ui->hero_detail_label, ui->slot_details[selected]);
        lv_obj_clear_flag(ui->hero_detail_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->hero_detail_panel, LV_OBJ_FLAG_HIDDEN);
    }

    const char *action = "Set up";
    if (state == NATIVE_PRECONNECT_SESSION_CONNECTED) {
        action = "Resume";
    } else if (state == NATIVE_PRECONNECT_SESSION_CONNECTING) {
        action = "Connecting...";
    } else if (state == NATIVE_PRECONNECT_SESSION_OFFLINE) {
        action = "Connect";
    } else if (state == NATIVE_PRECONNECT_SESSION_ERROR) {
        action = "Retry";
    }
    lv_label_set_text(ui->hero_action_label, action);
    if (state == NATIVE_PRECONNECT_SESSION_CONNECTING) {
        lv_obj_add_state(ui->hero_action_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(ui->hero_action_btn, LV_STATE_DISABLED);
    }
    if (state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP || state == NATIVE_PRECONNECT_SESSION_CONNECTING) {
        lv_obj_add_flag(ui->hero_edit_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(ui->hero_edit_btn, LV_OBJ_FLAG_HIDDEN);
    }

    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        char card_fallback[32];
        const char *card_name = ui_slot_display_name(ui, slot, card_fallback, sizeof(card_fallback));
        NativePreconnectSessionState card_state = ui->slot_states[slot];
        lv_label_set_text(ui->card_name_labels[slot], card_name);
        lv_label_set_text(ui->card_badge_labels[slot], ui_badge_text(card_state));
        uint32_t badge_color = ui_state_color(card_state);
        lv_obj_set_style_bg_color(ui->card_badges[slot], lv_color_hex(badge_color), 0);
        lv_obj_set_style_bg_opa(ui->card_badges[slot], card_state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP
                                                                  ? LV_OPA_TRANSP
                                                                  : LV_OPA_20,
                                0);
        lv_obj_set_style_border_width(ui->card_badges[slot], 1, 0);
        lv_obj_set_style_border_color(ui->card_badges[slot], lv_color_hex(badge_color), 0);
        lv_obj_set_style_border_opa(ui->card_badges[slot], LV_OPA_40, 0);
        lv_obj_set_style_text_color(ui->card_badge_labels[slot], lv_color_hex(badge_color), 0);
        lv_obj_set_style_text_opa(ui->card_badge_labels[slot], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(ui->slot_buttons[slot], card_state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP
                                                              ? LV_OPA_70
                                                              : LV_OPA_COVER,
                                0);
        lv_obj_set_style_bg_opa(ui->card_tabs[slot],
                                card_state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP ? (lv_opa_t)115
                                                                                 : LV_OPA_COVER,
                                0);
        if (card_state == NATIVE_PRECONNECT_SESSION_CONNECTED) {
            lv_obj_clear_flag(ui->card_audio_groups[slot], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui->card_audio_groups[slot], LV_OBJ_FLAG_HIDDEN);
        }
    }
    ui_update_slot_buttons(ui);
    if (!ui->setup_visible && !ui->onboarding_visible && ui->group) {
        /* Session state can change asynchronously while a HUB control owns focus. Do
         * not leave the keypad stranded on an action that just became disabled or an
         * Edit button that was hidden when the selected slot started connecting. */
        lv_obj_t *focused = lv_group_get_focused(ui->group);
        if ((focused == ui->hero_action_btn && lv_obj_has_state(ui->hero_action_btn, LV_STATE_DISABLED)) ||
            (focused == ui->hero_edit_btn && lv_obj_has_flag(ui->hero_edit_btn, LV_OBJ_FLAG_HIDDEN))) {
            lv_group_focus_obj(ui->slot_buttons[selected]);
        }
    }
}

bool native_preconnect_ui_get_slot_values(NativePreconnectUi *ui, int slot, NativeSessionConfig *out) {
    if (!ui || !out || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return false;
    }
    /* Make sure the on-screen form's latest edits are reflected for its own slot. */
    if (slot == ui->selected_slot) {
        ui_store_form_to_slot(ui, slot);
    }
    if (!ui->slot_port_valid[slot] || !ui_host_valid(ui->slot_values[slot].host)) {
        return false;
    }
    *out = ui->slot_values[slot];
    return true;
}

int native_preconnect_ui_selected_slot(const NativePreconnectUi *ui) {
    return ui ? ui->selected_slot : -1;
}

bool native_preconnect_ui_session_keys_enabled(const NativePreconnectUi *ui) {
    return ui && !ui->connecting && !ui->onboarding_visible && !ui->connect_save_pending && !ui->save_pending &&
           !ui->delete_pending;
}

void native_preconnect_ui_select_slot(NativePreconnectUi *ui, int slot) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    bool discarded_form =
        ui->setup_visible && !ui->save_pending && !ui->connect_save_pending && !ui->delete_pending;
    if (discarded_form) {
        /* Slot and colour-key navigation leaves the setup drawer without accepting it.
         * Restore both the model and widgets so a later get_slot_values() cannot
         * resurrect and persist the abandoned form. Save and Save-and-connect snapshot
         * the form before their pending flag is set and retain those explicit edits. */
        ui_discard_form_changes(ui);
        ui_set_text(ui->status_label, "");
    }
    if (slot == ui->selected_slot) {
        if (discarded_form) {
            ui_load_slot_into_form(ui, slot);
        }
        ui_update_hub(ui);
        return;
    }
    ui->selected_slot = slot;
    ui_load_slot_into_form(ui, slot);
    ui_update_hub(ui);
    if (!ui->setup_visible && !ui->onboarding_visible && ui->group) {
        lv_group_focus_obj(ui->slot_buttons[slot]);
    }
}

void native_preconnect_ui_show_hub(NativePreconnectUi *ui, int active_slot) {
    if (!ui) {
        return;
    }
    if (active_slot >= 0 && active_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
        native_preconnect_ui_select_slot(ui, active_slot);
    }
    /* A previous HUB close happens on BACK-down, before its physical release reaches
     * the app. Do not carry LVGL's PRESSED/long-press state into this new visit. */
    ui_reset_input_state(ui);
    ui->activate_requested = false;
    ui->hub_close_requested = false;
    ui->hub_closing = false;
    ui_show_onboarding(ui, false);
    ui_show_setup(ui, false);
}

bool native_preconnect_ui_cycle_slot(NativePreconnectUi *ui, int direction) {
    if (!ui || direction == 0 || ui->connecting || ui->connect_save_pending || ui->save_pending ||
        ui->delete_pending || ui->setup_visible || ui->onboarding_visible) {
        return false;
    }
    int step_direction = direction > 0 ? 1 : -1;
    for (int step = 1; step <= NATIVE_SETTINGS_MAX_SESSIONS; step++) {
        int candidate = (ui->selected_slot + step_direction * step) % NATIVE_SETTINGS_MAX_SESSIONS;
        if (candidate < 0) {
            candidate += NATIVE_SETTINGS_MAX_SESSIONS;
        }
        if (ui_slot_configured(&ui->slot_values[candidate])) {
            native_preconnect_ui_select_slot(ui, candidate);
            return true;
        }
    }
    return false;
}

void native_preconnect_ui_open_setup(NativePreconnectUi *ui, int slot) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS || ui->connecting) {
        return;
    }
    native_preconnect_ui_select_slot(ui, slot);
    ui_load_slot_into_form(ui, slot);
    ui_set_text(ui->status_label, "");
    ui_show_setup(ui, true);
}

bool native_preconnect_ui_request_connect(NativePreconnectUi *ui, int slot) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS || ui->onboarding_visible) {
        return false;
    }
    native_preconnect_ui_select_slot(ui, slot);
    if (!ui_slot_configured(&ui->slot_values[slot]) || !ui->slot_port_valid[slot]) {
        native_preconnect_ui_open_setup(ui, slot);
        return false;
    }
    return ui_queue_connect(ui, slot, false);
}

void native_preconnect_ui_cancel_pending_navigation(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    ui->activate_requested = false;
    if (ui->connect_requested) {
        ui->connect_requested = false;
        ui->connect_save_pending = false;
        ui->connecting = false;
        if (ui->requested_slot >= 0 && ui->requested_slot < NATIVE_SETTINGS_MAX_SESSIONS) {
            ui->slot_states[ui->requested_slot] = ui->requested_previous_state;
            (void)snprintf(ui->slot_details[ui->requested_slot], UI_DETAIL_MAX, "%s",
                           ui->requested_previous_detail);
        }
        ui_update_hub(ui);
        ui_update_connect_state(ui);
    }
}

static void ui_slot_button_focused(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->hub_closing || ui->rebuilding_group || ui->setup_visible || ui->onboarding_visible ||
        ui->connect_save_pending || ui->save_pending || ui->delete_pending) {
        return;
    }
    lv_indev_t *active_indev = lv_indev_get_act();
    if (!active_indev || lv_indev_get_type(active_indev) != LV_INDEV_TYPE_KEYPAD) {
        return;
    }
    lv_obj_t *target = lv_event_get_current_target(event);
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (ui->slot_buttons[slot] == target && ui->selected_slot != slot) {
            ui->selected_slot = slot;
            ui_load_slot_into_form(ui, slot);
            ui_update_hub(ui);
            return;
        }
    }
}

static void ui_slot_button_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->hub_closing || ui->connect_save_pending || ui->save_pending || ui->delete_pending) {
        return;
    }
    lv_obj_t *target = lv_event_get_current_target(event);
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (ui->slot_buttons[slot] == target) {
            if (ui->selected_slot == slot) {
                ui_hero_action_clicked(event);
            } else {
                native_preconnect_ui_select_slot(ui, slot);
            }
            break;
        }
    }
    ui_update_slot_buttons(ui);
}

NativePreconnectUi *native_preconnect_ui_create(SDL_Window *window, SDL_Renderer *renderer,
                                                const NativeSessionConfig *sessions, uint16_t audio_codec) {
    if (!window || !renderer || !sessions) {
        return NULL;
    }

    NativePreconnectUi *ui = (NativePreconnectUi *)calloc(1, sizeof(*ui));
    if (!ui) {
        return NULL;
    }
    ui->window = window;
    ui->renderer = renderer;
    ui->visible = true;
    ui->keyboard_available = true;
    ui->selected_slot = NATIVE_SESSION_SLOT_RED;
    bool found_configured = false;
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        ui->slot_values[slot] = sessions[slot];
        ui->committed_values[slot] = sessions[slot];
        (void)snprintf(ui->slot_port_text[slot], UI_PORT_MAX, "%u",
                       (unsigned)(sessions[slot].port ? sessions[slot].port : 3389));
        ui->slot_port_valid[slot] = true;
        if (ui_slot_configured(&sessions[slot])) {
            ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_OFFLINE;
            if (!found_configured) {
                ui->selected_slot = slot;
                found_configured = true;
            }
        } else {
            ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_NOT_SET_UP;
        }
    }
    ui->onboarding_visible = !found_configured;
    ui->requested_slot = ui->selected_slot;
    ui->activated_slot = ui->selected_slot;
    ui->saved_slot = ui->selected_slot;
    ui->deleted_slot = ui->selected_slot;
    ui->committed_audio_codec = audio_codec;

    SDL_GetWindowSize(window, &ui->width, &ui->height);
    if (ui->width <= 0 || ui->height <= 0) {
        ui->width = 1920;
        ui->height = 1080;
    }
    ui->pointer_x = ui->width / 2;
    ui->pointer_y = ui->height / 2;

    lv_init();
    ui->texture = lv_draw_sdl_create_screen_texture(renderer, ui->width, ui->height);
    if (!ui->texture) {
        clog(cLogLevelError, "failed to create LVGL screen texture");
        free(ui);
        return NULL;
    }
    lv_disp_draw_buf_init(&ui->draw_buf, ui->texture, NULL, (uint32_t)ui->width * (uint32_t)ui->height);
    lv_disp_drv_init(&ui->disp_drv);
    ui->draw_param.renderer = renderer;
    ui->draw_param.user_data = ui;
    ui->disp_drv.user_data = &ui->draw_param;
    ui->disp_drv.draw_buf = &ui->draw_buf;
    ui->disp_drv.flush_cb = ui_flush;
    ui->disp_drv.clear_cb = ui_clear;
    ui->disp_drv.hor_res = (lv_coord_t)ui->width;
    ui->disp_drv.ver_res = (lv_coord_t)ui->height;
    ui->disp_drv.dpi = LV_DPI_DEF;
    SDL_SetRenderTarget(renderer, ui->texture);
    ui->disp = lv_disp_drv_register(&ui->disp_drv);
    if (!ui->disp) {
        SDL_DestroyTexture(ui->texture);
        free(ui);
        return NULL;
    }
    ui->disp->bg_opa = LV_OPA_TRANSP;

    lv_indev_drv_init(&ui->pointer_drv);
    ui->pointer_drv.type = LV_INDEV_TYPE_POINTER;
    ui->pointer_drv.user_data = ui;
    ui->pointer_drv.read_cb = ui_pointer_read;
    ui->pointer_indev = lv_indev_drv_register(&ui->pointer_drv);

    memset(&ui->key_drv, 0, sizeof(ui->key_drv));
    lv_indev_drv_init(&ui->key_drv.base);
    ui->key_drv.base.type = LV_INDEV_TYPE_KEYPAD;
    ui->key_drv.base.user_data = ui;
    ui->key_drv.base.read_cb = ui_key_read;
    ui->key_drv.state = LV_INDEV_STATE_RELEASED;
    ui->key_indev = lv_indev_drv_register(&ui->key_drv.base);

    ui_init_styles(ui);
    const NativeSessionConfig *initial = &ui->slot_values[ui->selected_slot];
    ui_build(ui, initial->host, initial->port, initial->username, initial->password, initial->domain, initial->fps,
             audio_codec);
    ui_load_slot_into_form(ui, ui->selected_slot);
    ui_update_hub(ui);
    native_preconnect_ui_set_keyboard_available(ui, true);
    native_preconnect_ui_set_mouse_available(ui, true);
    if (ui->key_indev && ui->group) {
        lv_indev_set_group(ui->key_indev, ui->group);
    }
    /* A failed mixer create degrades gracefully: the live overlay remains unavailable. */
    ui->mixer = native_ui_mixer_create(renderer);
    native_ui_mixer_set_texture(ui->mixer, ui->texture);
    native_ui_mixer_set_profiles(ui->mixer, ui->slot_values);
    return ui;
}

void native_preconnect_ui_destroy(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    if (ui->pointer_indev) {
        lv_indev_delete(ui->pointer_indev);
        ui->pointer_indev = NULL;
    }
    if (ui->key_indev) {
        lv_indev_delete(ui->key_indev);
        ui->key_indev = NULL;
    }
    if (ui->group) {
        lv_group_remove_all_objs(ui->group);
        lv_group_del(ui->group);
    }
    if (ui->root) {
        lv_obj_del(ui->root);
    }
    native_ui_mixer_destroy(ui->mixer);
    ui_reset_styles(ui);
    if (ui->disp) {
        lv_disp_remove(ui->disp);
    }
    if (ui->texture) {
        SDL_DestroyTexture(ui->texture);
    }
    free(ui);
}

void native_preconnect_ui_resize(NativePreconnectUi *ui, int width, int height) {
    if (!ui || width <= 0 || height <= 0) {
        return;
    }
    ui->width = width;
    ui->height = height;
    ui->pointer_x = ui->pointer_x >= width ? width - 1 : ui->pointer_x;
    ui->pointer_y = ui->pointer_y >= height ? height - 1 : ui->pointer_y;
    if (ui->texture) {
        SDL_DestroyTexture(ui->texture);
    }
    ui->texture = lv_draw_sdl_create_screen_texture(ui->renderer, width, height);
    lv_disp_draw_buf_init(&ui->draw_buf, ui->texture, NULL, (uint32_t)width * (uint32_t)height);
    ui->disp_drv.hor_res = (lv_coord_t)width;
    ui->disp_drv.ver_res = (lv_coord_t)height;
    SDL_SetRenderTarget(ui->renderer, ui->texture);
    lv_disp_drv_update(ui->disp, &ui->disp_drv);
    native_ui_mixer_set_texture(ui->mixer, ui->texture);
    lv_obj_invalidate(lv_scr_act());
}

void native_preconnect_ui_tick(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    if (!ui->visible) {
        if (ui->hidden_cleared) {
            return;
        }
        SDL_SetRenderTarget(ui->renderer, NULL);
        SDL_SetRenderDrawBlendMode(ui->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ui->renderer, 0, 0, 0, 0);
        SDL_RenderClear(ui->renderer);
        SDL_RenderPresent(ui->renderer);
        SDL_SetRenderTarget(ui->renderer, ui->texture);
        ui->hidden_cleared = true;
        return;
    }
    ui->hidden_cleared = false;
    SDL_SetRenderTarget(ui->renderer, ui->texture);
    ui_update_card_audio_meters(ui);
    lv_timer_handler();
}

void native_preconnect_ui_set_visible(NativePreconnectUi *ui, bool visible) {
    if (!ui || ui->visible == visible) {
        return;
    }
    /* Visibility can change while LVGL still considers BACK or the pointer pressed.
     * Reset both the driver queue and LVGL's processed state at the boundary; otherwise
     * a hidden BACK-up is consumed by the streaming loop and the stale PRESSED state
     * repeats as soon as HUB is shown again. */
    ui_reset_input_state(ui);
    ui->visible = visible;
    ui->hidden_cleared = false;
    if (!visible) {
        ui->hub_close_requested = false;
        ui->hub_closing = false;
    }
    if (ui->root) {
        if (visible) {
            lv_obj_clear_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui->root);
        } else {
            lv_obj_add_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void native_preconnect_ui_set_background_drawer(NativePreconnectUi *ui,
                                                NativePreconnectUiBackgroundDrawFn draw, void *ctx) {
    if (!ui) {
        return;
    }
    ui->background_draw = draw;
    ui->background_draw_ctx = ctx;
}

void native_preconnect_ui_set_hardware_video_plane(NativePreconnectUi *ui, bool available) {
    if (!ui || ui->hardware_video_plane == available) {
        return;
    }
    ui->hardware_video_plane = available;
    if (ui->root) {
        lv_obj_invalidate(ui->root);
    }
}

void native_preconnect_ui_set_connecting(NativePreconnectUi *ui, int slot, bool connecting, const char *status) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    ui->connecting = connecting;
    if (ui->host_input) {
        if (connecting) {
            lv_obj_add_state(ui->name_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->host_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->port_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->username_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->domain_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->password_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->fps_dropdown, LV_STATE_DISABLED);
            lv_obj_add_state(ui->audio_codec_dropdown, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(ui->name_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->host_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->port_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->username_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->domain_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->password_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->fps_dropdown, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->audio_codec_dropdown, LV_STATE_DISABLED);
        }
        if (connecting) {
            ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_CONNECTING;
            (void)snprintf(ui->slot_details[slot], UI_DETAIL_MAX, "%s", status ? status : "");
            ui_show_setup(ui, false);
        } else if (ui->slot_states[slot] == NATIVE_PRECONNECT_SESSION_CONNECTING) {
            ui->slot_states[slot] = ui_slot_configured(&ui->slot_values[slot]) ? NATIVE_PRECONNECT_SESSION_OFFLINE
                                                                               : NATIVE_PRECONNECT_SESSION_NOT_SET_UP;
        }
    }
    native_preconnect_ui_set_status(ui, status, false);
    ui_update_hub(ui);
    ui_update_connect_state(ui);
}

void native_preconnect_ui_set_status(NativePreconnectUi *ui, const char *status, bool error) {
    if (!ui || !ui->status_label) {
        return;
    }
    ui_set_text(ui->status_label, status);
    lv_obj_set_style_text_color(ui->status_label, error ? lv_color_hex(0xf0958f) : lv_color_hex(0xaeb6bf), 0);
}

void native_preconnect_ui_set_slot_state(NativePreconnectUi *ui, int slot,
                                         NativePreconnectSessionState state, const char *detail) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    if (!ui_slot_configured(&ui->slot_values[slot]) && state != NATIVE_PRECONNECT_SESSION_CONNECTING &&
        state != NATIVE_PRECONNECT_SESSION_ERROR) {
        state = NATIVE_PRECONNECT_SESSION_NOT_SET_UP;
    }
    const char *next_detail = detail ? detail : "";
    bool same_detail;
    if (detail) {
        same_detail = strcmp(ui->slot_details[slot], next_detail) == 0;
    } else {
        same_detail = state == NATIVE_PRECONNECT_SESSION_ERROR || ui->slot_details[slot][0] == '\0';
    }
    if (ui->slot_states[slot] == state && same_detail) {
        return;
    }
    ui->slot_states[slot] = state;
    if (detail) {
        (void)snprintf(ui->slot_details[slot], UI_DETAIL_MAX, "%s", detail);
    } else if (state != NATIVE_PRECONNECT_SESSION_ERROR) {
        ui->slot_details[slot][0] = '\0';
    }
    if (slot == ui->selected_slot) {
        ui_update_hub(ui);
    } else {
        char fallback[32];
        lv_label_set_text(ui->card_name_labels[slot], ui_slot_display_name(ui, slot, fallback, sizeof(fallback)));
        lv_label_set_text(ui->card_badge_labels[slot], ui_badge_text(state));
        ui_update_hub(ui);
    }
    ui_update_connect_state(ui);
}

void native_preconnect_ui_set_slot_runtime(NativePreconnectUi *ui, int slot, uint16_t desktop_width,
                                           uint16_t desktop_height, uint32_t session_minutes,
                                           bool audio_stream_open, uint32_t audio_codec,
                                           uint32_t audio_sample_rate, uint16_t audio_channels,
                                           int32_t audio_peak_left, int32_t audio_peak_right) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    ui->slot_audio_peaks[slot][0] = audio_peak_left > 0 ? audio_peak_left : 0;
    ui->slot_audio_peaks[slot][1] = audio_peak_right > 0 ? audio_peak_right : 0;
    bool format_valid = audio_stream_open &&
                        (audio_codec == RDP_AUDIO_CODEC_OPUS || audio_codec == RDP_AUDIO_CODEC_PCM_S16LE) &&
                        audio_sample_rate != 0 && audio_channels != 0;
    if (!format_valid) {
        audio_codec = 0;
        audio_sample_rate = 0;
        audio_channels = 0;
    }
    if (ui->slot_desktop_width[slot] == desktop_width && ui->slot_desktop_height[slot] == desktop_height &&
        ui->slot_session_minutes[slot] == session_minutes &&
        ui->slot_audio_stream_open[slot] == audio_stream_open && ui->slot_audio_codec[slot] == audio_codec &&
        ui->slot_audio_sample_rate[slot] == audio_sample_rate && ui->slot_audio_channels[slot] == audio_channels) {
        return;
    }
    ui->slot_desktop_width[slot] = desktop_width;
    ui->slot_desktop_height[slot] = desktop_height;
    ui->slot_session_minutes[slot] = session_minutes;
    ui->slot_audio_stream_open[slot] = audio_stream_open;
    ui->slot_audio_codec[slot] = audio_codec;
    ui->slot_audio_sample_rate[slot] = audio_sample_rate;
    ui->slot_audio_channels[slot] = audio_channels;
    ui_update_hub(ui);
}

void native_preconnect_ui_set_keyboard_available(NativePreconnectUi *ui, bool available) {
    if (!ui) {
        return;
    }
    ui->keyboard_available = available;
    lv_obj_set_style_bg_color(ui->keyboard_dot, lv_color_hex(available ? 0x43d47e : 0xe8c15a), 0);
    if (available) {
        lv_obj_add_flag(ui->keyboard_warning, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(ui->keyboard_warning_label,
                          "No USB keyboard detected - connect one to type on the remote desktop.");
        lv_obj_clear_flag(ui->keyboard_warning, LV_OBJ_FLAG_HIDDEN);
    }
}

void native_preconnect_ui_set_mouse_available(NativePreconnectUi *ui, bool available) {
    if (!ui) {
        return;
    }
    lv_obj_set_style_bg_color(ui->mouse_dot, lv_color_hex(available ? 0x43d47e : 0xe8c15a), 0);
}

void native_preconnect_ui_set_input_unavailable(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    ui->keyboard_available = false;
    lv_obj_set_style_bg_color(ui->keyboard_dot, lv_color_hex(0xe35d55), 0);
    lv_obj_set_style_bg_color(ui->mouse_dot, lv_color_hex(0xe35d55), 0);
    lv_label_set_text(ui->keyboard_warning_label,
                      "USB input capture is unavailable - reconnect the devices or restart GnomeCast.");
    lv_obj_clear_flag(ui->keyboard_warning, LV_OBJ_FLAG_HIDDEN);
}

bool native_preconnect_ui_take_connect(NativePreconnectUi *ui, int *slot, char *host, size_t host_cap,
                                       uint16_t *port, char *username, size_t username_cap, char *password,
                                       size_t password_cap, char *domain, size_t domain_cap, uint16_t *fps,
                                       uint16_t *audio_codec, bool *requires_save) {
    if (!ui || !ui->connect_requested) {
        return false;
    }
    ui->connect_requested = false;
    if (slot) {
        *slot = ui->requested_slot;
    }
    if (host && host_cap > 0) {
        size_t len = strlen(ui->requested_host);
        if (len >= host_cap) {
            len = host_cap - 1;
        }
        memcpy(host, ui->requested_host, len);
        host[len] = '\0';
    }
    if (port) {
        *port = ui->requested_port;
    }
    if (fps) {
        *fps = ui->requested_fps;
    }
    if (username && username_cap > 0) {
        size_t len = strlen(ui->requested_username);
        if (len >= username_cap) {
            len = username_cap - 1;
        }
        memcpy(username, ui->requested_username, len);
        username[len] = '\0';
    }
    if (password && password_cap > 0) {
        size_t len = strlen(ui->requested_password);
        if (len >= password_cap) {
            len = password_cap - 1;
        }
        memcpy(password, ui->requested_password, len);
        password[len] = '\0';
    }
    if (domain && domain_cap > 0) {
        size_t len = strlen(ui->requested_domain);
        if (len >= domain_cap) {
            len = domain_cap - 1;
        }
        memcpy(domain, ui->requested_domain, len);
        domain[len] = '\0';
    }
    if (audio_codec) {
        *audio_codec = ui->requested_audio_codec;
    }
    if (requires_save) {
        *requires_save = ui->requested_requires_save;
    }
    return true;
}

bool native_preconnect_ui_take_activate(NativePreconnectUi *ui, int *slot) {
    if (!ui || !ui->activate_requested) {
        return false;
    }
    ui->activate_requested = false;
    if (slot) {
        *slot = ui->activated_slot;
    }
    return true;
}

bool native_preconnect_ui_take_hub_close(NativePreconnectUi *ui) {
    if (!ui || !ui->hub_close_requested) {
        return false;
    }
    ui->hub_close_requested = false;
    return true;
}

void native_preconnect_ui_cancel_hub_close(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    ui->hub_close_requested = false;
    ui->hub_closing = false;
}

bool native_preconnect_ui_take_save(NativePreconnectUi *ui, int *slot, uint16_t *audio_codec) {
    if (!ui || !ui->save_requested) {
        return false;
    }
    ui->save_requested = false;
    if (slot) {
        *slot = ui->saved_slot;
    }
    if (audio_codec) {
        *audio_codec = ui_current_audio_codec(ui);
    }
    return true;
}

bool native_preconnect_ui_take_delete(NativePreconnectUi *ui, int *slot) {
    if (!ui || !ui->delete_requested) {
        return false;
    }
    ui->delete_requested = false;
    if (slot) {
        *slot = ui->deleted_slot;
    }
    return true;
}

void native_preconnect_ui_finish_save(NativePreconnectUi *ui, int slot, bool success, const char *status) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    ui->save_pending = false;
    if (success) {
        ui->committed_values[slot] = ui->slot_values[slot];
        ui->committed_audio_codec = ui_current_audio_codec(ui);
        if (ui->slot_states[slot] != NATIVE_PRECONNECT_SESSION_CONNECTED &&
            ui->slot_states[slot] != NATIVE_PRECONNECT_SESSION_CONNECTING) {
            ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_OFFLINE;
        }
        ui->slot_details[slot][0] = '\0';
        if (ui->selected_slot == slot) {
            ui_set_text(ui->status_label, status ? status : "Saved on this TV.");
            ui_show_setup(ui, false);
        }
    } else if (ui->selected_slot == slot) {
        ui_set_text(ui->status_label, status ? status : "Could not save settings on this TV.");
        lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xf0958f), 0);
        ui_show_setup(ui, true);
    }
    native_ui_mixer_set_profiles(ui->mixer, ui->slot_values);
    ui_update_hub(ui);
    ui_update_connect_state(ui);
}

void native_preconnect_ui_finish_connect_save(NativePreconnectUi *ui, int slot, bool success, bool persisted,
                                               const char *status) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    ui->connect_save_pending = false;
    if (success && persisted) {
        ui->committed_values[slot] = ui->slot_values[slot];
        ui->committed_audio_codec = ui_current_audio_codec(ui);
    } else if (!success) {
        ui->connecting = false;
        /* The request never replaced the card's prior runtime state. Restore its
         * complete presentation; the setup status below carries the new rejection. */
        ui->slot_states[slot] = ui->requested_previous_state;
        (void)snprintf(ui->slot_details[slot], UI_DETAIL_MAX, "%s", ui->requested_previous_detail);
        native_preconnect_ui_open_setup(ui, slot);
        ui_set_text(ui->status_label, status ? status : "Could not save settings on this TV.");
        lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xf0958f), 0);
    }
    ui_update_hub(ui);
    ui_update_connect_state(ui);
}

void native_preconnect_ui_finish_delete(NativePreconnectUi *ui, int slot, bool success, const char *status) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS) {
        return;
    }
    ui->delete_pending = false;
    if (success) {
        NativeSessionConfig empty = {0};
        empty.port = 3389;
        empty.fps = 60;
        ui->slot_values[slot] = empty;
        ui->committed_values[slot] = empty;
        (void)snprintf(ui->slot_port_text[slot], UI_PORT_MAX, "%u", (unsigned)empty.port);
        ui->slot_port_valid[slot] = true;
        ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_NOT_SET_UP;
        ui->slot_details[slot][0] = '\0';
        ui->delete_armed = false;
        native_ui_mixer_set_profiles(ui->mixer, ui->slot_values);
        if (ui->selected_slot == slot) {
            ui_load_slot_into_form(ui, slot);
            ui_set_text(ui->status_label, status ? status : "Profile deleted.");
            ui_show_setup(ui, false);
        }
    } else if (ui->selected_slot == slot) {
        ui_set_text(ui->status_label, status ? status : "Could not delete this profile.");
        lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xf0958f), 0);
        ui->delete_armed = false;
        lv_label_set_text(ui->delete_label, "Delete profile");
    }
    ui_update_hub(ui);
    ui_update_connect_state(ui);
}

bool native_preconnect_ui_read_current(NativePreconnectUi *ui, char *host, size_t host_cap, uint16_t *port,
                                       char *username, size_t username_cap, char *password, size_t password_cap,
                                       char *domain, size_t domain_cap, uint16_t *fps, uint16_t *audio_codec) {
    if (!ui) {
        return false;
    }

    uint16_t parsed_port = 0;
    const char *current_host = lv_textarea_get_text(ui->host_input);
    char normalized_host[UI_HOST_MAX];
    if (!native_ui_host_normalize(current_host, normalized_host, sizeof(normalized_host)) ||
        !ui_parse_port(lv_textarea_get_text(ui->port_input), &parsed_port)) {
        return false;
    }

    const char *current_username = lv_textarea_get_text(ui->username_input);
    const char *current_password = lv_textarea_get_text(ui->password_input);
    const char *current_domain = lv_textarea_get_text(ui->domain_input);
    if (!current_username || !current_password) {
        return false;
    }

    if (host && host_cap > 0) {
        size_t len = strlen(normalized_host);
        if (len >= host_cap) {
            return false;
        }
        memcpy(host, normalized_host, len + 1);
    }
    if (port) {
        *port = parsed_port;
    }
    if (username && username_cap > 0) {
        size_t len = strlen(current_username);
        if (len >= username_cap) {
            return false;
        }
        memcpy(username, current_username, len + 1);
    }
    if (password && password_cap > 0) {
        size_t len = strlen(current_password);
        if (len >= password_cap) {
            return false;
        }
        memcpy(password, current_password, len + 1);
    }
    if (domain && domain_cap > 0) {
        size_t len = strlen(current_domain ? current_domain : "");
        if (len >= domain_cap) {
            return false;
        }
        memcpy(domain, current_domain ? current_domain : "", len + 1);
    }
    if (fps) {
        *fps = ui->selected_fps;
    }
    if (audio_codec) {
        *audio_codec = ui_current_audio_codec(ui);
    }
    return true;
}

NativeUiMixer *native_preconnect_ui_mixer(NativePreconnectUi *ui) {
    return ui ? ui->mixer : NULL;
}

static void ui_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *src) {
    (void)src;
    if (area->x2 < 0 || area->y2 < 0 || area->x1 > drv->hor_res - 1 || area->y1 > drv->ver_res - 1) {
        lv_disp_flush_ready(drv);
        return;
    }
    if (lv_disp_flush_is_last(drv)) {
        lv_draw_sdl_drv_param_t *param = (lv_draw_sdl_drv_param_t *)drv->user_data;
        NativePreconnectUi *ui = (NativePreconnectUi *)param->user_data;
        SDL_SetRenderTarget(ui->renderer, NULL);
        SDL_SetRenderDrawBlendMode(ui->renderer, SDL_BLENDMODE_BLEND);
        bool software_background_drawn =
            ui->background_draw && ui->background_draw(ui->background_draw_ctx, ui->renderer);
        if (!software_background_drawn) {
            bool live_plane = native_ui_mixer_active(ui->mixer) || ui->hardware_video_plane;
            if (live_plane) {
                /* HUB and mixer are translucent chrome over the NDL hardware plane. */
                SDL_SetRenderDrawColor(ui->renderer, 0, 0, 0, 0);
            } else {
                /* Before a session exists there is no video layer to reveal. */
                SDL_SetRenderDrawColor(ui->renderer, 0x07, 0x08, 0x0b, 0xff);
            }
            SDL_RenderClear(ui->renderer);
        }
        SDL_SetTextureBlendMode(ui->texture, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(ui->renderer, ui->texture, NULL, NULL);
        SDL_RenderPresent(ui->renderer);
        SDL_SetRenderTarget(ui->renderer, ui->texture);
    }
    lv_disp_flush_ready(drv);
}

static void ui_clear(lv_disp_drv_t *drv, uint8_t *buf, uint32_t size) {
    /* The SDL GPU backend owns a render-target texture rather than a CPU pixel buffer.
     * Its display-background draw clears each uncovered dirty rect with BLENDMODE_NONE. */
    (void)drv;
    (void)buf;
    (void)size;
}

static void ui_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    NativePreconnectUi *ui = (NativePreconnectUi *)drv->user_data;
    SDL_Event event;
    data->continue_reading = SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEBUTTONUP) > 0;
    if (data->continue_reading) {
        switch (event.type) {
        case SDL_MOUSEMOTION:
            ui->pointer_x = event.motion.x;
            ui->pointer_y = event.motion.y;
            ui->pointer_pressed = (event.motion.state & SDL_BUTTON_LMASK) != 0;
            break;
        case SDL_MOUSEBUTTONDOWN:
            ui->pointer_x = event.button.x;
            ui->pointer_y = event.button.y;
            if (event.button.button == SDL_BUTTON_LEFT) {
                ui->pointer_pressed = true;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            ui->pointer_x = event.button.x;
            ui->pointer_y = event.button.y;
            if (event.button.button == SDL_BUTTON_LEFT) {
                ui->pointer_pressed = false;
            }
            break;
        default:
            break;
        }
    }
    data->point.x = (lv_coord_t)ui->pointer_x;
    data->point.y = (lv_coord_t)ui->pointer_y;
    data->state = ui->pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static bool ui_key_from_sdl(const SDL_KeyboardEvent *event, uint32_t *key) {
    if (event->keysym.scancode == 482 /* SDL_WEBOS_SCANCODE_BACK */) {
        *key = LV_KEY_ESC;
        return true;
    }
    switch (event->keysym.sym) {
    case SDLK_UP:
        *key = LV_KEY_UP;
        return true;
    case SDLK_DOWN:
        *key = LV_KEY_DOWN;
        return true;
    case SDLK_LEFT:
        *key = LV_KEY_LEFT;
        return true;
    case SDLK_RIGHT:
        *key = LV_KEY_RIGHT;
        return true;
    case SDLK_ESCAPE:
        *key = LV_KEY_ESC;
        return true;
    case SDLK_DELETE:
        *key = LV_KEY_DEL;
        return true;
    case SDLK_BACKSPACE:
        *key = LV_KEY_BACKSPACE;
        return true;
    case SDLK_KP_ENTER:
    case SDLK_RETURN:
    case SDLK_RETURN2:
        *key = LV_KEY_ENTER;
        return true;
    case SDLK_TAB:
        *key = (event->keysym.mod & KMOD_SHIFT) ? LV_KEY_PREV : LV_KEY_NEXT;
        return true;
    case SDLK_HOME:
        *key = LV_KEY_HOME;
        return true;
    case SDLK_END:
        *key = LV_KEY_END;
        return true;
    default:
        return false;
    }
}

static bool ui_text_key_from_sdl(const SDL_KeyboardEvent *event, uint32_t *key) {
    if (!event || event->type != SDL_KEYDOWN || !key) {
        return false;
    }

    SDL_Keycode sym = event->keysym.sym;
    bool shift = (event->keysym.mod & KMOD_SHIFT) != 0;
    bool caps = (event->keysym.mod & KMOD_CAPS) != 0;

    if (sym >= SDLK_a && sym <= SDLK_z) {
        char ch = (char)('a' + (sym - SDLK_a));
        if (shift ^ caps) {
            ch = (char)('A' + (sym - SDLK_a));
        }
        *key = (uint8_t)ch;
        return true;
    }
    if (sym >= SDLK_0 && sym <= SDLK_9) {
        static const char shifted_digits[] = ")!@#$%^&*(";
        *key = (uint8_t)(shift ? shifted_digits[sym - SDLK_0] : (char)('0' + (sym - SDLK_0)));
        return true;
    }
    if (sym >= SDLK_KP_0 && sym <= SDLK_KP_9) {
        *key = (uint8_t)('0' + (sym - SDLK_KP_0));
        return true;
    }

    switch (sym) {
    case SDLK_SPACE:
        *key = ' ';
        return true;
    case SDLK_PERIOD:
    case SDLK_KP_PERIOD:
        *key = shift ? '>' : '.';
        return true;
    case SDLK_COMMA:
        *key = shift ? '<' : ',';
        return true;
    case SDLK_SEMICOLON:
        *key = shift ? ':' : ';';
        return true;
    case SDLK_COLON:
        *key = ':';
        return true;
    case SDLK_SLASH:
    case SDLK_KP_DIVIDE:
        *key = shift ? '?' : '/';
        return true;
    case SDLK_BACKSLASH:
        *key = shift ? '|' : '\\';
        return true;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        *key = shift ? '_' : '-';
        return true;
    case SDLK_EQUALS:
    case SDLK_KP_EQUALS:
        *key = shift ? '+' : '=';
        return true;
    case SDLK_LEFTBRACKET:
        *key = shift ? '{' : '[';
        return true;
    case SDLK_RIGHTBRACKET:
        *key = shift ? '}' : ']';
        return true;
    case SDLK_QUOTE:
        *key = shift ? '"' : '\'';
        return true;
    case SDLK_BACKQUOTE:
        *key = shift ? '~' : '`';
        return true;
    case SDLK_KP_MULTIPLY:
        *key = '*';
        return true;
    case SDLK_KP_PLUS:
        *key = '+';
        return true;
    default:
        return false;
    }
}

static bool ui_key_queue_empty(const NativePreconnectKeyDriver *state) {
    return state->head == state->tail;
}

static void ui_key_queue_clear(NativePreconnectKeyDriver *state) {
    state->head = 0;
    state->tail = 0;
    state->overflow_pending = false;
}

static void ui_reset_input_state(NativePreconnectUi *ui) {
    if (!ui) {
        return;
    }
    ui_key_queue_clear(&ui->key_drv);
    ui->key_drv.key = 0;
    ui->key_drv.state = LV_INDEV_STATE_RELEASED;
    ui->pointer_pressed = false;
    if (ui->key_indev) {
        lv_indev_reset(ui->key_indev, NULL);
        /* lv_indev_reset() does not clear the keypad's last_state. Make LVGL
         * consume the synthetic RELEASED read below without turning a stale
         * ENTER-down into a click on the newly focused HUB card. */
        lv_indev_wait_release(ui->key_indev);
    }
    if (ui->pointer_indev) {
        lv_indev_reset(ui->pointer_indev, NULL);
    }
}

static void ui_key_enqueue(NativePreconnectKeyDriver *state, uint32_t key, lv_indev_state_t key_state) {
    unsigned next_tail = (state->tail + 1u) % UI_KEY_QUEUE_CAP;
    if (next_tail == state->head) {
        /* Full. Dropping the oldest event (the previous behavior) would strand half of a
         * press/release pair and make LVGL treat a key as held -- runaway auto-repeat, e.g. a
         * repeated Connect. Instead refuse the event and flag the burst so ui_key_read discards
         * it wholesale and returns to a released state. Input is lost only in this pathological
         * case (a paste larger than a full field between reads), but no key is left stuck. */
        state->overflow_pending = true;
        return;
    }
    state->queue[state->tail].key = key;
    state->queue[state->tail].state = key_state;
    state->tail = next_tail;
}

static bool ui_key_dequeue(NativePreconnectKeyDriver *state, NativePreconnectQueuedKey *event) {
    if (ui_key_queue_empty(state)) {
        return false;
    }
    *event = state->queue[state->head];
    state->head = (state->head + 1u) % UI_KEY_QUEUE_CAP;
    return true;
}

static void ui_key_enqueue_tap(NativePreconnectKeyDriver *state, uint32_t key) {
    ui_key_enqueue(state, key, LV_INDEV_STATE_PRESSED);
    ui_key_enqueue(state, key, LV_INDEV_STATE_RELEASED);
}

static size_t ui_utf8_char_len(unsigned char first) {
    if ((first & 0x80u) == 0) {
        return 1;
    }
    if ((first & 0xe0u) == 0xc0u) {
        return 2;
    }
    if ((first & 0xf0u) == 0xe0u) {
        return 3;
    }
    if ((first & 0xf8u) == 0xf0u) {
        return 4;
    }
    return 1;
}

static void ui_key_enqueue_text(NativePreconnectKeyDriver *state, const char *text) {
    if (!text) {
        return;
    }
    size_t len = strlen(text);
    for (size_t i = 0; i < len;) {
        size_t char_len = ui_utf8_char_len((unsigned char)text[i]);
        if (char_len > len - i) {
            char_len = 1;
        }
        uint32_t key = 0;
        memcpy(&key, text + i, char_len);
        ui_key_enqueue_tap(state, key);
        i += char_len;
    }
}

static bool ui_event_batch_has_textinput(const SDL_Event *events, int count) {
    for (int i = 0; i < count; i++) {
        if (events[i].type == SDL_TEXTINPUT) {
            return true;
        }
    }
    return false;
}

static void ui_key_drain_sdl_events(NativePreconnectKeyDriver *state) {
    SDL_Event events[64];
    for (;;) {
        int count = SDL_PeepEvents(events, (int)(sizeof(events) / sizeof(events[0])), SDL_GETEVENT, SDL_KEYDOWN,
                                   SDL_TEXTINPUT);
        if (count <= 0) {
            break;
        }

        bool text_input_pending = ui_event_batch_has_textinput(events, count) || SDL_HasEvent(SDL_TEXTINPUT);
        for (int i = 0; i < count; i++) {
            uint32_t key = 0;
            switch (events[i].type) {
            case SDL_TEXTINPUT:
                ui_key_enqueue_text(state, events[i].text.text);
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (ui_key_from_sdl(&events[i].key, &key)) {
                    ui_key_enqueue(state, key,
                                   events[i].type == SDL_KEYDOWN ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED);
                } else if (events[i].type == SDL_KEYDOWN && !text_input_pending &&
                           ui_text_key_from_sdl(&events[i].key, &key)) {
                    ui_key_enqueue_tap(state, key);
                }
                break;
            default:
                break;
            }
        }
    }
}

static void ui_key_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    NativePreconnectUi *ui = (NativePreconnectUi *)drv->user_data;
    NativePreconnectKeyDriver *state = (NativePreconnectKeyDriver *)drv;

    if (ui->connecting) {
        SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT);
        ui_key_queue_clear(state);
        state->state = LV_INDEV_STATE_RELEASED;
        data->key = state->key;
        data->state = state->state;
        data->continue_reading = false;
        return;
    }

    ui_key_drain_sdl_events(state);

    if (state->overflow_pending) {
        /* A burst overflowed the ring. Drop it entirely and force a released state so no key is
         * left logically held (which LVGL would auto-repeat). Logged once per burst (the burst
         * is fully consumed here, so this cannot spam across reads). */
        clog(cLogLevelWarning, "pre-connect key queue overflow; dropping burst and resetting key input");
        ui_key_queue_clear(state); /* resets head/tail and the overflow flag */
        state->key = 0;
        state->state = LV_INDEV_STATE_RELEASED;
        data->key = state->key;
        data->state = state->state;
        data->continue_reading = false;
        return;
    }

    NativePreconnectQueuedKey event;
    if (ui_key_dequeue(state, &event)) {
        state->key = event.key;
        state->state = event.state;
    }

    data->key = state->key;
    data->state = state->state;
    data->continue_reading = !ui_key_queue_empty(state);
}

static void ui_input_changed(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->loading_form) {
        return;
    }
    ui->delete_armed = false;
    if (ui->delete_label) {
        lv_label_set_text(ui->delete_label, "Delete profile");
    }
    ui_set_text(ui->status_label, "Changes are not saved yet.");
    lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xaeb6bf), 0);
    ui_update_connect_state(ui);
}

static void ui_profile_name_insert(lv_event_t *event) {
    lv_obj_t *input = lv_event_get_target(event);
    const char *inserted = (const char *)lv_event_get_param(event);
    if (!input || !inserted) {
        return;
    }
    /* LVGL routes deletion through LV_EVENT_INSERT with this sentinel. */
    if ((unsigned char)inserted[0] == LV_KEY_DEL && inserted[1] == '\0') {
        return;
    }
    const char *current = lv_textarea_get_text(input);
    size_t current_bytes = current ? strlen(current) : 0u;
    size_t inserted_bytes = strlen(inserted);
    if (!native_ui_profile_name_valid(inserted, UI_NAME_MAX) ||
        inserted_bytes > UI_NAME_MAX - 1u ||
        current_bytes > UI_NAME_MAX - 1u - inserted_bytes) {
        /* NativeSessionConfig.name is byte-sized while LVGL's max_length counts
         * codepoints. Reject the character instead of truncating a later UTF-8 copy. */
        lv_textarea_set_insert_replace(input, "");
    }
}

static void ui_scroll_focused_into_view(NativePreconnectUi *ui) {
    if (!ui || !ui->group) {
        return;
    }
    lv_obj_t *focused = lv_group_get_focused(ui->group);
    if (focused) {
        lv_obj_scroll_to_view_recursive(focused, LV_ANIM_OFF);
    }
}

/* Spatial remote navigation for the HUB. LEFT/RIGHT always means computer selection;
 * UP/DOWN move between the selected card and its action row, then Edit and Help. The
 * explicit table avoids relying on LVGL's default keypad behavior (buttons consume
 * arrow keys without moving group focus). */
static bool ui_hub_navigate(NativePreconnectUi *ui, lv_obj_t *target, uint32_t key) {
    if (!ui || !target || ui->connecting || ui->connect_save_pending || ui->save_pending ||
        ui->delete_pending || ui->setup_visible || ui->onboarding_visible) {
        return false;
    }

    int card_slot = -1;
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        if (target == ui->slot_buttons[slot]) {
            card_slot = slot;
            break;
        }
    }
    bool hub_control = card_slot >= 0 || target == ui->hero_action_btn || target == ui->hero_edit_btn ||
                       target == ui->help_btn;
    if (!hub_control) {
        return false;
    }

    if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        int direction = key == LV_KEY_RIGHT ? 1 : -1;
        int next = (ui->selected_slot + direction + NATIVE_SETTINGS_MAX_SESSIONS) %
                   NATIVE_SETTINGS_MAX_SESSIONS;
        native_preconnect_ui_select_slot(ui, next);
        return true;
    }
    if (card_slot >= 0 && (key == LV_KEY_UP || key == LV_KEY_DOWN)) {
        lv_group_focus_obj(lv_obj_has_state(ui->hero_action_btn, LV_STATE_DISABLED) ? ui->help_btn
                                                                                   : ui->hero_action_btn);
        return true;
    }

    bool edit_visible = !lv_obj_has_flag(ui->hero_edit_btn, LV_OBJ_FLAG_HIDDEN);
    if (target == ui->hero_action_btn) {
        if (key == LV_KEY_UP) {
            lv_group_focus_obj(ui->slot_buttons[ui->selected_slot]);
            return true;
        }
        if (key == LV_KEY_DOWN) {
            lv_group_focus_obj(edit_visible ? ui->hero_edit_btn : ui->help_btn);
            return true;
        }
    } else if (target == ui->hero_edit_btn) {
        if (key == LV_KEY_UP) {
            lv_group_focus_obj(ui->hero_action_btn);
            return true;
        }
        if (key == LV_KEY_DOWN) {
            lv_group_focus_obj(ui->help_btn);
            return true;
        }
    } else if (target == ui->help_btn) {
        if (key == LV_KEY_UP) {
            lv_obj_t *up_target = edit_visible ? ui->hero_edit_btn : ui->hero_action_btn;
            if (lv_obj_has_state(up_target, LV_STATE_DISABLED)) {
                up_target = ui->slot_buttons[ui->selected_slot];
            }
            lv_group_focus_obj(up_target);
            return true;
        }
        if (key == LV_KEY_DOWN) {
            lv_group_focus_obj(ui->slot_buttons[ui->selected_slot]);
            return true;
        }
    }
    return false;
}

static void ui_form_key_event(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || !ui->group) {
        return;
    }
    uint32_t key = lv_event_get_key(event);
    /* An OPEN dropdown owns UP/DOWN for option selection; only navigate the form with
     * them while every dropdown is closed. */
    lv_obj_t *target = lv_event_get_target(event);
    if ((target == ui->fps_dropdown && lv_dropdown_is_open(ui->fps_dropdown)) ||
        (target == ui->audio_codec_dropdown && lv_dropdown_is_open(ui->audio_codec_dropdown))) {
        if (key == LV_KEY_ESC) {
            if (ui->key_indev) {
                lv_indev_wait_release(ui->key_indev);
            }
        }
        return;
    }

    if (key == LV_KEY_ESC) {
        if (ui->setup_visible || ui->onboarding_visible) {
            if (ui->onboarding_visible) {
                ui_show_onboarding(ui, false);
            } else {
                ui_cancel_clicked(event);
            }
            if (ui->key_indev) {
                lv_indev_wait_release(ui->key_indev);
            }
        } else {
            native_preconnect_ui_cancel_pending_navigation(ui);
            ui->hub_close_requested = true;
            ui->hub_closing = true;
        }
        lv_event_stop_processing(event);
    } else if (ui->hub_closing || ui->connecting) {
        return;
    } else if (ui_hub_navigate(ui, target, key)) {
        lv_event_stop_processing(event);
    } else if (key == LV_KEY_DOWN) {
        lv_group_focus_next(ui->group);
        ui_scroll_focused_into_view(ui);
        lv_event_stop_processing(event);
    } else if (key == LV_KEY_UP) {
        lv_group_focus_prev(ui->group);
        ui_scroll_focused_into_view(ui);
        lv_event_stop_processing(event);
    }
}

static void ui_hub_key_event(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui) {
        return;
    }
    uint32_t key = lv_event_get_key(event);
    if (key == LV_KEY_ESC && !ui->setup_visible && !ui->onboarding_visible) {
        native_preconnect_ui_cancel_pending_navigation(ui);
        ui->hub_close_requested = true;
        ui->hub_closing = true;
        lv_event_stop_processing(event);
    } else if (ui->hub_closing || ui->connecting || ui->connect_save_pending || ui->save_pending ||
               ui->delete_pending || ui->setup_visible || ui->onboarding_visible) {
        return;
    } else if (ui_hub_navigate(ui, lv_event_get_target(event), key)) {
        lv_event_stop_processing(event);
    }
}

static void ui_fps_changed(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || ui->loading_form) {
        return;
    }
    ui_set_selected_fps(ui, lv_dropdown_get_selected(ui->fps_dropdown));
    ui->delete_armed = false;
    lv_label_set_text(ui->delete_label, "Delete profile");
    ui_set_text(ui->status_label, "Changes are not saved yet.");
    lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xaeb6bf), 0);
}

static bool ui_queue_connect(NativePreconnectUi *ui, int slot, bool force_save) {
    if (!ui || slot < 0 || slot >= NATIVE_SETTINGS_MAX_SESSIONS || ui->hub_closing ||
        ui->connect_save_pending || ui->save_pending || ui->delete_pending) {
        return false;
    }
    NativeSessionConfig *values = &ui->slot_values[slot];
    if (!ui_slot_configured(values) || !ui->slot_port_valid[slot] || values->port == 0) {
        native_preconnect_ui_open_setup(ui, slot);
        native_preconnect_ui_set_status(ui, "Enter an address, port, username and password.", true);
        return false;
    }
    if (!native_ui_host_normalize(values->host, ui->requested_host,
                                  sizeof(ui->requested_host))) {
        native_preconnect_ui_set_status(ui, "Enter a valid address.", true);
        return false;
    }
    if (strcmp(values->host, ui->requested_host) != 0) {
        /* Profiles saved by the split-field UI regression can still contain [IPv6].
         * Migrate the draft as part of this request: it then differs from the committed
         * value, which makes main persist and use its normalized candidate. */
        (void)snprintf(values->host, sizeof(values->host), "%s", ui->requested_host);
    }
    size_t len = strlen(values->username);
    if (len >= sizeof(ui->requested_username)) {
        native_preconnect_ui_set_status(ui, "Username value is too long.", true);
        return false;
    }
    memcpy(ui->requested_username, values->username, len + 1);
    len = strlen(values->domain);
    if (len >= sizeof(ui->requested_domain)) {
        native_preconnect_ui_set_status(ui, "Domain value is too long.", true);
        return false;
    }
    memcpy(ui->requested_domain, values->domain, len + 1);
    len = strlen(values->password);
    if (len >= sizeof(ui->requested_password)) {
        native_preconnect_ui_set_status(ui, "Password value is too long.", true);
        return false;
    }
    memcpy(ui->requested_password, values->password, len + 1);
    ui->requested_port = values->port;
    ui->requested_fps = values->fps;
    ui->requested_audio_codec = ui_current_audio_codec(ui);
    ui->requested_slot = slot;
    ui->requested_previous_state = ui->slot_states[slot];
    (void)snprintf(ui->requested_previous_detail, UI_DETAIL_MAX, "%s", ui->slot_details[slot]);
    ui->requested_requires_save = force_save || ui_profile_dirty(ui, slot);
    ui->connect_requested = true;
    ui->connect_save_pending = true;
    ui->slot_states[slot] = NATIVE_PRECONNECT_SESSION_CONNECTING;
    ui->slot_details[slot][0] = '\0';
    ui_update_hub(ui);
    ui_show_setup(ui, false);
    return true;
}

static void ui_connect_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || !ui_form_valid(ui)) {
        ui_update_connect_state(ui);
        return;
    }
    ui_store_form_to_slot(ui, ui->selected_slot);
    (void)ui_queue_connect(ui, ui->selected_slot, true);
}

static void ui_save_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || !ui_form_valid(ui)) {
        ui_update_connect_state(ui);
        return;
    }
    ui_store_form_to_slot(ui, ui->selected_slot);
    ui->saved_slot = ui->selected_slot;
    ui->save_requested = true;
    ui->save_pending = true;
    ui_set_text(ui->status_label, "Saving...");
    ui_update_connect_state(ui);
}

static void ui_cancel_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || ui->save_pending || ui->connect_save_pending || ui->delete_pending) {
        return;
    }
    ui_discard_form_changes(ui);
    ui_load_slot_into_form(ui, ui->selected_slot);
    ui_set_text(ui->status_label, "");
    ui_update_hub(ui);
    ui_show_setup(ui, false);
}

static void ui_delete_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || ui->save_pending || ui->connect_save_pending || ui->delete_pending ||
        !ui_slot_configured(&ui->slot_values[ui->selected_slot])) {
        return;
    }
    if (!ui->delete_armed) {
        ui->delete_armed = true;
        lv_label_set_text(ui->delete_label, "Confirm delete");
        ui_set_text(ui->status_label, "Press Confirm delete to remove this profile and its saved password.");
        lv_obj_set_style_text_color(ui->status_label, lv_color_hex(0xf0958f), 0);
        return;
    }
    ui->deleted_slot = ui->selected_slot;
    ui->delete_requested = true;
    ui->delete_pending = true;
    ui_set_text(ui->status_label, "Deleting...");
    ui_update_connect_state(ui);
}

static void ui_setup_scrim_clicked(lv_event_t *event) {
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) {
        return;
    }
    ui_cancel_clicked(event);
}

static void ui_edit_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (ui && !ui->hub_closing && !ui->connect_save_pending && !ui->save_pending &&
        !ui->delete_pending) {
        native_preconnect_ui_open_setup(ui, ui->selected_slot);
    }
}

static void ui_hero_action_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->hub_closing || ui->connecting || ui->connect_save_pending || ui->save_pending ||
        ui->delete_pending) {
        return;
    }
    NativePreconnectSessionState state = ui->slot_states[ui->selected_slot];
    if (state == NATIVE_PRECONNECT_SESSION_CONNECTED) {
        ui->activated_slot = ui->selected_slot;
        ui->activate_requested = true;
    } else if (state == NATIVE_PRECONNECT_SESSION_NOT_SET_UP) {
        native_preconnect_ui_open_setup(ui, ui->selected_slot);
    } else if (state != NATIVE_PRECONNECT_SESSION_CONNECTING) {
        (void)ui_queue_connect(ui, ui->selected_slot, false);
    }
}

static void ui_help_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (ui && !ui->hub_closing && !ui->connect_save_pending && !ui->save_pending &&
        !ui->delete_pending) {
        ui_show_onboarding(ui, true);
    }
}

static void ui_onboarding_close_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (ui) {
        ui_show_onboarding(ui, false);
    }
}
