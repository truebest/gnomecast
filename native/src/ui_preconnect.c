#include "ui_preconnect.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw/sdl/lv_draw_sdl.h"
#include "lvgl.h"

#define UI_HOST_MAX 512u
#define UI_PORT_MAX 8u
#define UI_ENDPOINT_MAX (UI_HOST_MAX + UI_PORT_MAX + 4u)
#define UI_WIDE_FIELD_WIDTH LV_DPX(420)
#define UI_COMPACT_FIELD_WIDTH LV_DPX(202)
#define UI_CONNECT_BUTTON_WIDTH LV_DPX(180)
#define UI_FORM_KEY_EVENT (LV_EVENT_KEY | LV_EVENT_PREPROCESS)

static const uint16_t UI_FPS_OPTIONS[] = {30, 60};
/* Audio jitter prebuffer slider bounds (milliseconds); 0 disables buffering. Trades
 * audio latency for resilience against audio packets stalling behind large video frames
 * on the shared RDP connection. */
#define UI_AUDIO_BUFFER_MAX_MS 300
#define UI_AUDIO_BUFFER_STEP_MS 10

typedef struct NativePreconnectKeyDriver {
    lv_indev_drv_t base;
    lv_indev_state_t state;
    uint32_t key;
    char text[SDL_TEXTINPUTEVENT_TEXT_SIZE];
    size_t text_len;
    size_t text_pos;
    bool text_key_down;
} NativePreconnectKeyDriver;

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
    NativePreconnectKeyDriver key_drv;
    lv_indev_t *key_indev;
    lv_group_t *group;
    lv_obj_t *root;
    lv_obj_t *host_input;
    lv_obj_t *username_input;
    lv_obj_t *domain_input;
    lv_obj_t *password_input;
    lv_obj_t *fps_dropdown;
    lv_obj_t *audio_buffer_slider;
    lv_obj_t *audio_buffer_value_label;
    lv_obj_t *mouse_mode_switch;
    lv_obj_t *mouse_absolute_label;
    lv_obj_t *mouse_relative_label;
    lv_obj_t *jump_filter_switch;
    lv_obj_t *connect_btn;
    lv_obj_t *status_label;
    lv_style_t root_style;
    lv_style_t nav_style;
    lv_style_t detail_style;
    lv_style_t title_style;
    lv_style_t label_style;
    lv_style_t input_style;
    lv_style_t input_focus_style;
    lv_style_t input_cursor_style;
    lv_style_t dropdown_style;
    lv_style_t dropdown_focus_style;
    lv_style_t dropdown_list_style;
    lv_style_t dropdown_selected_style;
    lv_style_t button_style;
    lv_style_t button_focus_style;
    lv_style_t button_disabled_style;
    lv_style_t status_style;
    lv_style_t switch_style;
    lv_style_t switch_checked_style;
    lv_style_t switch_knob_style;
    lv_coord_t root_cols[3];
    lv_coord_t root_rows[2];
    int width;
    int height;
    int pointer_x;
    int pointer_y;
    bool pointer_pressed;
    bool visible;
    bool hidden_cleared;
    bool connecting;
    bool connect_requested;
    bool relative_mouse;
    bool requested_relative_mouse;
    bool jump_filter;
    bool requested_jump_filter;
    bool current_fps_option;
    uint16_t current_fps;
    uint16_t selected_fps;
    char requested_host[UI_HOST_MAX];
    char requested_username[UI_HOST_MAX];
    char requested_domain[UI_HOST_MAX];
    char requested_password[UI_HOST_MAX];
    uint16_t requested_port;
    uint16_t requested_fps;
    uint16_t selected_audio_buffer;
    uint16_t requested_audio_buffer;
};

static void ui_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *src);
static void ui_clear(lv_disp_drv_t *drv, uint8_t *buf, uint32_t size);
static void ui_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
static void ui_key_read(lv_indev_drv_t *drv, lv_indev_data_t *data);
static void ui_input_changed(lv_event_t *event);
static void ui_fps_changed(lv_event_t *event);
static void ui_audio_buffer_changed(lv_event_t *event);
static void ui_form_key_event(lv_event_t *event);
static void ui_mouse_mode_changed(lv_event_t *event);
static void ui_jump_filter_changed(lv_event_t *event);
static void ui_connect_clicked(lv_event_t *event);
static void ui_scroll_focused_into_view(NativePreconnectUi *ui);

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

static bool ui_parse_endpoint(const char *text, char *host, size_t host_cap, uint16_t *port) {
    if (!text || !text[0] || !host || host_cap == 0 || !port) {
        return false;
    }

    const char *host_begin = text;
    const char *host_end = NULL;
    const char *port_text = NULL;

    if (text[0] == '[') {
        host_begin = text + 1;
        host_end = strchr(host_begin, ']');
        if (!host_end || host_end == host_begin || host_end[1] != ':') {
            return false;
        }
        port_text = host_end + 2;
    } else {
        host_end = strrchr(text, ':');
        if (!host_end || host_end == text) {
            return false;
        }
        port_text = host_end + 1;
    }

    if (!port_text[0] || !ui_parse_port(port_text, port)) {
        return false;
    }

    size_t len = (size_t)(host_end - host_begin);
    if (len == 0 || len >= host_cap) {
        return false;
    }
    memcpy(host, host_begin, len);
    host[len] = '\0';
    return true;
}

static void ui_format_endpoint(char *dest, size_t cap, const char *host, uint16_t port) {
    if (!dest || cap == 0) {
        return;
    }
    if (!host || !host[0]) {
        dest[0] = '\0';
        return;
    }
    if (strchr(host, ':') && host[0] != '[') {
        (void)snprintf(dest, cap, "[%s]:%u", host, (unsigned)port);
    } else {
        (void)snprintf(dest, cap, "%s:%u", host, (unsigned)port);
    }
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
    if (ui_find_fps_option(fps, &builtin_index)) {
        return ui->current_fps_option ? builtin_index + 1u : builtin_index;
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

static uint16_t ui_audio_buffer_clamp(uint16_t value) {
    if (value > UI_AUDIO_BUFFER_MAX_MS) {
        value = UI_AUDIO_BUFFER_MAX_MS;
    }
    /* Snap to the slider step so remote-control adjustments land on round values. */
    return (uint16_t)((value + UI_AUDIO_BUFFER_STEP_MS / 2) / UI_AUDIO_BUFFER_STEP_MS * UI_AUDIO_BUFFER_STEP_MS);
}

static void ui_update_audio_buffer_label(NativePreconnectUi *ui) {
    if (!ui->audio_buffer_value_label) {
        return;
    }
    if (ui->selected_audio_buffer == 0) {
        lv_label_set_text(ui->audio_buffer_value_label, "Off");
    } else {
        lv_label_set_text_fmt(ui->audio_buffer_value_label, "%u ms", (unsigned)ui->selected_audio_buffer);
    }
}

/* The slider works in UI_AUDIO_BUFFER_STEP_MS units so one remote-control key press
 * moves the value by a meaningful step instead of 1 ms. */
static void ui_set_audio_buffer_value(NativePreconnectUi *ui, uint16_t value) {
    ui->selected_audio_buffer = ui_audio_buffer_clamp(value);
    if (ui->audio_buffer_slider) {
        lv_slider_set_value(ui->audio_buffer_slider, ui->selected_audio_buffer / UI_AUDIO_BUFFER_STEP_MS, LV_ANIM_OFF);
    }
    ui_update_audio_buffer_label(ui);
}

static bool ui_form_valid(NativePreconnectUi *ui) {
    uint16_t port = 0;
    char host[UI_HOST_MAX];
    const char *endpoint = lv_textarea_get_text(ui->host_input);
    const char *username = lv_textarea_get_text(ui->username_input);
    const char *password = lv_textarea_get_text(ui->password_input);
    return endpoint && ui_parse_endpoint(endpoint, host, sizeof(host), &port) && username && username[0] && password &&
           password[0];
}

static void ui_update_connect_state(NativePreconnectUi *ui) {
    if (!ui || !ui->connect_btn) {
        return;
    }
    if (ui->connecting || !ui_form_valid(ui)) {
        lv_obj_add_state(ui->connect_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(ui->connect_btn, LV_STATE_DISABLED);
    }
}

static void ui_set_text(lv_obj_t *label, const char *text) {
    lv_label_set_text(label, text && text[0] ? text : "");
}

static void ui_update_mouse_mode_state(NativePreconnectUi *ui) {
    if (!ui || !ui->mouse_mode_switch) {
        return;
    }
    if (ui->relative_mouse) {
        lv_obj_add_state(ui->mouse_mode_switch, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(ui->mouse_absolute_label, lv_color_hex(0xaeb6bf), 0);
        lv_obj_set_style_text_color(ui->mouse_relative_label, lv_color_hex(0xffffff), 0);
    } else {
        lv_obj_clear_state(ui->mouse_mode_switch, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(ui->mouse_absolute_label, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_color(ui->mouse_relative_label, lv_color_hex(0xaeb6bf), 0);
    }
}

static void ui_init_styles(NativePreconnectUi *ui) {
    lv_style_init(&ui->root_style);
    lv_style_set_bg_color(&ui->root_style, lv_color_hex(0x15171a));
    lv_style_set_bg_opa(&ui->root_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->root_style, 0);
    lv_style_set_pad_all(&ui->root_style, 0);
    lv_style_set_radius(&ui->root_style, 0);

    lv_style_init(&ui->nav_style);
    lv_style_set_bg_color(&ui->nav_style, lv_color_hex(0x24272c));
    lv_style_set_bg_opa(&ui->nav_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->nav_style, 0);
    lv_style_set_radius(&ui->nav_style, 0);
    lv_style_set_pad_all(&ui->nav_style, LV_DPX(26));
    lv_style_set_text_color(&ui->nav_style, lv_color_hex(0xf2f5f8));

    lv_style_init(&ui->detail_style);
    lv_style_set_bg_color(&ui->detail_style, lv_color_hex(0x2f3237));
    lv_style_set_bg_opa(&ui->detail_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->detail_style, 0);
    lv_style_set_radius(&ui->detail_style, 0);
    lv_style_set_pad_all(&ui->detail_style, LV_DPX(24));
    lv_style_set_pad_gap(&ui->detail_style, LV_DPX(6));

    lv_style_init(&ui->title_style);
    lv_style_set_text_color(&ui->title_style, lv_color_hex(0xffffff));
    lv_style_set_text_font(&ui->title_style, &lv_font_montserrat_28);

    lv_style_init(&ui->label_style);
    lv_style_set_text_color(&ui->label_style, lv_color_hex(0xd4dae0));
    lv_style_set_text_font(&ui->label_style, &lv_font_montserrat_20);

    lv_style_init(&ui->input_style);
    lv_style_set_bg_color(&ui->input_style, lv_color_hex(0x1f2227));
    lv_style_set_bg_opa(&ui->input_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->input_style, 0);
    lv_style_set_radius(&ui->input_style, LV_DPX(8));
    lv_style_set_pad_hor(&ui->input_style, LV_DPX(12));
    lv_style_set_pad_ver(&ui->input_style, LV_DPX(8));
    lv_style_set_text_color(&ui->input_style, lv_color_hex(0xf6f8fb));
    lv_style_set_text_font(&ui->input_style, &lv_font_montserrat_20);

    lv_style_init(&ui->input_focus_style);
    lv_style_set_bg_color(&ui->input_focus_style, lv_color_hex(0x29313a));
    lv_style_set_outline_color(&ui->input_focus_style, lv_color_hex(0x8fc7ff));
    lv_style_set_outline_opa(&ui->input_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->input_focus_style, LV_DPX(3));
    lv_style_set_outline_pad(&ui->input_focus_style, LV_DPX(2));

    lv_style_init(&ui->input_cursor_style);
    lv_style_set_bg_opa(&ui->input_cursor_style, LV_OPA_TRANSP);
    lv_style_set_border_side(&ui->input_cursor_style, LV_BORDER_SIDE_LEFT);
    lv_style_set_border_color(&ui->input_cursor_style, lv_color_hex(0xffffff));
    lv_style_set_border_width(&ui->input_cursor_style, LV_DPX(2));
    lv_style_set_pad_left(&ui->input_cursor_style, -LV_DPX(1));
    lv_style_set_anim_time(&ui->input_cursor_style, 450);

    lv_style_init(&ui->dropdown_style);
    lv_style_set_bg_color(&ui->dropdown_style, lv_color_hex(0x1f2227));
    lv_style_set_bg_opa(&ui->dropdown_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->dropdown_style, 0);
    lv_style_set_radius(&ui->dropdown_style, LV_DPX(8));
    lv_style_set_pad_hor(&ui->dropdown_style, LV_DPX(12));
    lv_style_set_pad_ver(&ui->dropdown_style, LV_DPX(9));
    lv_style_set_text_color(&ui->dropdown_style, lv_color_hex(0xf6f8fb));
    lv_style_set_text_font(&ui->dropdown_style, &lv_font_montserrat_20);

    lv_style_init(&ui->dropdown_focus_style);
    lv_style_set_bg_color(&ui->dropdown_focus_style, lv_color_hex(0x29313a));
    lv_style_set_outline_color(&ui->dropdown_focus_style, lv_color_hex(0x8fc7ff));
    lv_style_set_outline_opa(&ui->dropdown_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->dropdown_focus_style, LV_DPX(3));
    lv_style_set_outline_pad(&ui->dropdown_focus_style, LV_DPX(2));

    lv_style_init(&ui->dropdown_list_style);
    lv_style_set_bg_color(&ui->dropdown_list_style, lv_color_hex(0x24272c));
    lv_style_set_bg_opa(&ui->dropdown_list_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->dropdown_list_style, 0);
    lv_style_set_radius(&ui->dropdown_list_style, LV_DPX(8));
    lv_style_set_pad_all(&ui->dropdown_list_style, LV_DPX(6));
    lv_style_set_text_color(&ui->dropdown_list_style, lv_color_hex(0xf6f8fb));
    lv_style_set_text_font(&ui->dropdown_list_style, &lv_font_montserrat_20);

    lv_style_init(&ui->dropdown_selected_style);
    lv_style_set_bg_color(&ui->dropdown_selected_style, lv_color_hex(0x2d8cff));
    lv_style_set_bg_opa(&ui->dropdown_selected_style, LV_OPA_COVER);
    lv_style_set_text_color(&ui->dropdown_selected_style, lv_color_hex(0xffffff));

    lv_style_init(&ui->button_style);
    lv_style_set_bg_color(&ui->button_style, lv_color_hex(0x2d8cff));
    lv_style_set_bg_opa(&ui->button_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->button_style, 0);
    lv_style_set_radius(&ui->button_style, LV_DPX(8));
    lv_style_set_pad_hor(&ui->button_style, LV_DPX(24));
    lv_style_set_pad_ver(&ui->button_style, LV_DPX(12));
    lv_style_set_text_color(&ui->button_style, lv_color_hex(0xffffff));
    lv_style_set_text_font(&ui->button_style, &lv_font_montserrat_20);

    lv_style_init(&ui->button_focus_style);
    lv_style_set_bg_color(&ui->button_focus_style, lv_color_hex(0x49a2ff));
    lv_style_set_outline_color(&ui->button_focus_style, lv_color_hex(0x8fc7ff));
    lv_style_set_outline_opa(&ui->button_focus_style, LV_OPA_COVER);
    lv_style_set_outline_width(&ui->button_focus_style, LV_DPX(3));
    lv_style_set_outline_pad(&ui->button_focus_style, LV_DPX(2));

    lv_style_init(&ui->button_disabled_style);
    lv_style_set_bg_color(&ui->button_disabled_style, lv_color_hex(0x505761));
    lv_style_set_text_color(&ui->button_disabled_style, lv_color_hex(0xaeb6bf));

    lv_style_init(&ui->status_style);
    lv_style_set_text_color(&ui->status_style, lv_color_hex(0xaeb6bf));
    lv_style_set_text_font(&ui->status_style, &lv_font_montserrat_14);

    lv_style_init(&ui->switch_style);
    lv_style_set_bg_color(&ui->switch_style, lv_color_hex(0x505761));
    lv_style_set_bg_opa(&ui->switch_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->switch_style, 0);
    lv_style_set_radius(&ui->switch_style, LV_RADIUS_CIRCLE);
    lv_style_set_outline_color(&ui->switch_style, lv_color_hex(0x8fc7ff));
    lv_style_set_outline_opa(&ui->switch_style, LV_OPA_TRANSP);
    lv_style_set_outline_width(&ui->switch_style, LV_DPX(3));
    lv_style_set_outline_pad(&ui->switch_style, LV_DPX(2));

    lv_style_init(&ui->switch_checked_style);
    lv_style_set_bg_color(&ui->switch_checked_style, lv_color_hex(0x2d8cff));
    lv_style_set_bg_opa(&ui->switch_checked_style, LV_OPA_COVER);
    lv_style_set_radius(&ui->switch_checked_style, LV_RADIUS_CIRCLE);

    lv_style_init(&ui->switch_knob_style);
    lv_style_set_bg_color(&ui->switch_knob_style, lv_color_hex(0xffffff));
    lv_style_set_bg_opa(&ui->switch_knob_style, LV_OPA_COVER);
    lv_style_set_border_width(&ui->switch_knob_style, 0);
    lv_style_set_radius(&ui->switch_knob_style, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&ui->switch_knob_style, LV_DPX(2));
}

static void ui_reset_styles(NativePreconnectUi *ui) {
    lv_style_reset(&ui->root_style);
    lv_style_reset(&ui->nav_style);
    lv_style_reset(&ui->detail_style);
    lv_style_reset(&ui->title_style);
    lv_style_reset(&ui->label_style);
    lv_style_reset(&ui->input_style);
    lv_style_reset(&ui->input_focus_style);
    lv_style_reset(&ui->input_cursor_style);
    lv_style_reset(&ui->dropdown_style);
    lv_style_reset(&ui->dropdown_focus_style);
    lv_style_reset(&ui->dropdown_list_style);
    lv_style_reset(&ui->dropdown_selected_style);
    lv_style_reset(&ui->button_style);
    lv_style_reset(&ui->button_focus_style);
    lv_style_reset(&ui->button_disabled_style);
    lv_style_reset(&ui->status_style);
    lv_style_reset(&ui->switch_style);
    lv_style_reset(&ui->switch_checked_style);
    lv_style_reset(&ui->switch_knob_style);
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
    lv_obj_set_size(dropdown, width, LV_DPX(52));
    lv_obj_set_style_text_color(dropdown, lv_color_hex(0xf6f8fb), LV_PART_INDICATOR);
    lv_obj_set_style_pad_right(dropdown, LV_DPX(12), LV_PART_INDICATOR);
    lv_dropdown_set_symbol(dropdown, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(dropdown, LV_DIR_BOTTOM);
    lv_dropdown_set_selected_highlight(dropdown, true);
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    lv_obj_remove_style_all(list);
    lv_obj_add_style(list, &ui->dropdown_list_style, 0);
    lv_obj_add_style(list, &ui->dropdown_selected_style, LV_PART_SELECTED);
    return dropdown;
}

static void ui_build(NativePreconnectUi *ui, const char *host, uint16_t port, const char *username, const char *password,
                     const char *domain, uint16_t fps, uint16_t audio_prebuffer_ms) {
    ui->root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(ui->root);
    lv_obj_add_style(ui->root, &ui->root_style, 0);
    lv_obj_set_size(ui->root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(ui->root, LV_LAYOUT_GRID);

    ui->root_cols[0] = LV_DPX(260);
    ui->root_cols[1] = LV_GRID_FR(1);
    ui->root_cols[2] = LV_GRID_TEMPLATE_LAST;
    ui->root_rows[0] = LV_GRID_FR(1);
    ui->root_rows[1] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(ui->root, ui->root_cols, ui->root_rows);

    lv_obj_t *nav = lv_obj_create(ui->root);
    lv_obj_remove_style_all(nav);
    lv_obj_add_style(nav, &ui->nav_style, 0);
    lv_obj_set_grid_cell(nav, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(nav, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *brand = ui_make_label(nav, "gnomecast", &ui->title_style);
    lv_obj_set_width(brand, LV_PCT(100));
    lv_obj_t *subtitle = ui_make_label(nav, "Native RDP", &ui->label_style);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x9fa8b2), 0);

    lv_obj_t *detail = lv_obj_create(ui->root);
    lv_obj_remove_style_all(detail);
    lv_obj_add_style(detail, &ui->detail_style, 0);
    lv_obj_set_grid_cell(detail, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_add_flag(detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(detail, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(detail, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(detail, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(detail, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title = ui_make_label(detail, "Connect", &ui->title_style);
    lv_obj_set_style_pad_bottom(title, LV_DPX(14), 0);

    ui_make_label(detail, "Address", &ui->label_style);
    ui->host_input = lv_textarea_create(detail);
    lv_obj_remove_style_all(ui->host_input);
    lv_obj_add_style(ui->host_input, &ui->input_style, 0);
    lv_obj_add_style(ui->host_input, &ui->input_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(ui->host_input, &ui->input_cursor_style, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_size(ui->host_input, UI_WIDE_FIELD_WIDTH, LV_DPX(52));
    lv_textarea_set_one_line(ui->host_input, true);
    lv_textarea_set_accepted_chars(ui->host_input, ".-_:[]0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    lv_textarea_set_max_length(ui->host_input, UI_ENDPOINT_MAX - 1);
    lv_textarea_set_placeholder_text(ui->host_input, "host:3389");
    char endpoint_text[UI_ENDPOINT_MAX];
    ui_format_endpoint(endpoint_text, sizeof(endpoint_text), host, port);
    lv_textarea_set_text(ui->host_input, endpoint_text);
    lv_textarea_set_cursor_pos(ui->host_input, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_event_cb(ui->host_input, ui_input_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->host_input, ui_form_key_event, UI_FORM_KEY_EVENT, ui);

    ui_make_label(detail, "Username", &ui->label_style);
    ui->username_input = lv_textarea_create(detail);
    lv_obj_remove_style_all(ui->username_input);
    lv_obj_add_style(ui->username_input, &ui->input_style, 0);
    lv_obj_add_style(ui->username_input, &ui->input_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(ui->username_input, &ui->input_cursor_style, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_size(ui->username_input, UI_WIDE_FIELD_WIDTH, LV_DPX(52));
    lv_textarea_set_one_line(ui->username_input, true);
    lv_textarea_set_max_length(ui->username_input, UI_HOST_MAX - 1);
    lv_textarea_set_placeholder_text(ui->username_input, "username");
    lv_textarea_set_text(ui->username_input, username ? username : "");
    lv_textarea_set_cursor_pos(ui->username_input, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_event_cb(ui->username_input, ui_input_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->username_input, ui_form_key_event, UI_FORM_KEY_EVENT, ui);

    ui_make_label(detail, "Domain", &ui->label_style);
    ui->domain_input = lv_textarea_create(detail);
    lv_obj_remove_style_all(ui->domain_input);
    lv_obj_add_style(ui->domain_input, &ui->input_style, 0);
    lv_obj_add_style(ui->domain_input, &ui->input_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(ui->domain_input, &ui->input_cursor_style, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_size(ui->domain_input, UI_WIDE_FIELD_WIDTH, LV_DPX(52));
    lv_textarea_set_one_line(ui->domain_input, true);
    lv_textarea_set_max_length(ui->domain_input, UI_HOST_MAX - 1);
    lv_textarea_set_placeholder_text(ui->domain_input, "optional");
    lv_textarea_set_text(ui->domain_input, domain ? domain : "");
    lv_textarea_set_cursor_pos(ui->domain_input, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_event_cb(ui->domain_input, ui_input_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->domain_input, ui_form_key_event, UI_FORM_KEY_EVENT, ui);

    ui_make_label(detail, "Password", &ui->label_style);
    ui->password_input = lv_textarea_create(detail);
    lv_obj_remove_style_all(ui->password_input);
    lv_obj_add_style(ui->password_input, &ui->input_style, 0);
    lv_obj_add_style(ui->password_input, &ui->input_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(ui->password_input, &ui->input_cursor_style, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_size(ui->password_input, UI_WIDE_FIELD_WIDTH, LV_DPX(52));
    lv_textarea_set_one_line(ui->password_input, true);
    lv_textarea_set_max_length(ui->password_input, UI_HOST_MAX - 1);
    lv_textarea_set_password_mode(ui->password_input, true);
    lv_textarea_set_placeholder_text(ui->password_input, "password");
    lv_textarea_set_text(ui->password_input, password ? password : "");
    lv_textarea_set_cursor_pos(ui->password_input, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_add_event_cb(ui->password_input, ui_input_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->password_input, ui_form_key_event, UI_FORM_KEY_EVENT, ui);

    ui_make_label(detail, "Display", &ui->label_style);
    lv_obj_t *display_row = lv_obj_create(detail);
    lv_obj_remove_style_all(display_row);
    lv_obj_set_size(display_row, UI_WIDE_FIELD_WIDTH, LV_DPX(52));
    lv_obj_clear_flag(display_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(display_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(display_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(display_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(display_row, LV_DPX(16), 0);

    ui->fps_dropdown = ui_make_dropdown(ui, display_row, UI_COMPACT_FIELD_WIDTH);
    size_t selected_fps = ui_select_fps_index(ui, fps);
    ui_set_fps_options(ui);
    ui_set_selected_fps(ui, selected_fps);
    lv_obj_add_event_cb(ui->fps_dropdown, ui_fps_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->fps_dropdown, ui_form_key_event, UI_FORM_KEY_EVENT, ui);

    ui_make_label(detail, "Audio buffer", &ui->label_style);
    lv_obj_t *audio_row = lv_obj_create(detail);
    lv_obj_remove_style_all(audio_row);
    lv_obj_set_size(audio_row, UI_WIDE_FIELD_WIDTH, LV_DPX(52));
    lv_obj_clear_flag(audio_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(audio_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(audio_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(audio_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(audio_row, LV_DPX(16), 0);

    ui->audio_buffer_slider = lv_slider_create(audio_row);
    lv_obj_remove_style_all(ui->audio_buffer_slider);
    lv_obj_set_size(ui->audio_buffer_slider, UI_COMPACT_FIELD_WIDTH, LV_DPX(10));
    lv_obj_set_style_bg_opa(ui->audio_buffer_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->audio_buffer_slider, lv_color_hex(0x2c3540), LV_PART_MAIN);
    lv_obj_set_style_radius(ui->audio_buffer_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->audio_buffer_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui->audio_buffer_slider, lv_color_hex(0x3f8efc), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui->audio_buffer_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ui->audio_buffer_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(ui->audio_buffer_slider, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_radius(ui->audio_buffer_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ui->audio_buffer_slider, LV_DPX(8), LV_PART_KNOB);
    lv_obj_set_style_outline_width(ui->audio_buffer_slider, LV_DPX(2), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(ui->audio_buffer_slider, lv_color_hex(0x3f8efc), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(ui->audio_buffer_slider, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_slider_set_range(ui->audio_buffer_slider, 0, UI_AUDIO_BUFFER_MAX_MS / UI_AUDIO_BUFFER_STEP_MS);
    lv_obj_add_event_cb(ui->audio_buffer_slider, ui_audio_buffer_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->audio_buffer_slider, ui_form_key_event, UI_FORM_KEY_EVENT, ui);

    ui->audio_buffer_value_label = ui_make_label(audio_row, "", &ui->label_style);
    lv_obj_set_width(ui->audio_buffer_value_label, LV_DPX(90));
    ui_set_audio_buffer_value(ui, audio_prebuffer_ms);

    ui_make_label(detail, "Mouse mode", &ui->label_style);
    lv_obj_t *mouse_row = lv_obj_create(detail);
    lv_obj_remove_style_all(mouse_row);
    lv_obj_set_size(mouse_row, UI_WIDE_FIELD_WIDTH, LV_DPX(46));
    lv_obj_clear_flag(mouse_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(mouse_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(mouse_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mouse_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mouse_row, LV_DPX(14), 0);

    ui->mouse_absolute_label = ui_make_label(mouse_row, "Absolute", &ui->label_style);
    ui->mouse_mode_switch = lv_switch_create(mouse_row);
    lv_obj_add_style(ui->mouse_mode_switch, &ui->switch_style, LV_PART_MAIN);
    lv_obj_add_style(ui->mouse_mode_switch, &ui->switch_checked_style, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_style(ui->mouse_mode_switch, &ui->switch_knob_style, LV_PART_KNOB);
    lv_obj_set_style_outline_opa(ui->mouse_mode_switch, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_size(ui->mouse_mode_switch, LV_DPX(35), LV_DPX(17));
    lv_obj_add_event_cb(ui->mouse_mode_switch, ui_mouse_mode_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->mouse_mode_switch, ui_form_key_event, UI_FORM_KEY_EVENT, ui);
    ui->mouse_relative_label = ui_make_label(mouse_row, "Relative", &ui->label_style);
    ui_update_mouse_mode_state(ui);

    lv_obj_t *jump_filter_label = ui_make_label(mouse_row, "Jump filter", &ui->label_style);
    lv_obj_set_style_pad_left(jump_filter_label, LV_DPX(18), 0);
    ui->jump_filter_switch = lv_switch_create(mouse_row);
    lv_obj_add_style(ui->jump_filter_switch, &ui->switch_style, LV_PART_MAIN);
    lv_obj_add_style(ui->jump_filter_switch, &ui->switch_checked_style, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_style(ui->jump_filter_switch, &ui->switch_knob_style, LV_PART_KNOB);
    lv_obj_set_style_outline_opa(ui->jump_filter_switch, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_size(ui->jump_filter_switch, LV_DPX(35), LV_DPX(17));
    if (ui->jump_filter) {
        lv_obj_add_state(ui->jump_filter_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(ui->jump_filter_switch, ui_jump_filter_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->jump_filter_switch, ui_form_key_event, UI_FORM_KEY_EVENT, ui);

    ui->connect_btn = lv_btn_create(detail);
    lv_obj_remove_style_all(ui->connect_btn);
    lv_obj_add_style(ui->connect_btn, &ui->button_style, 0);
    lv_obj_add_style(ui->connect_btn, &ui->button_focus_style, LV_STATE_FOCUSED);
    lv_obj_add_style(ui->connect_btn, &ui->button_disabled_style, LV_STATE_DISABLED);
    lv_obj_set_size(ui->connect_btn, UI_CONNECT_BUTTON_WIDTH, LV_DPX(58));
    lv_obj_add_event_cb(ui->connect_btn, ui_connect_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->connect_btn, ui_form_key_event, UI_FORM_KEY_EVENT, ui);
    lv_obj_t *button_label = lv_label_create(ui->connect_btn);
    lv_obj_remove_style_all(button_label);
    lv_obj_set_style_text_color(button_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(button_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(button_label, "Connect");
    lv_obj_center(button_label);

    ui->status_label = lv_label_create(detail);
    lv_obj_remove_style_all(ui->status_label);
    lv_obj_add_style(ui->status_label, &ui->status_style, 0);
    lv_obj_set_width(ui->status_label, UI_WIDE_FIELD_WIDTH);
    lv_label_set_long_mode(ui->status_label, LV_LABEL_LONG_WRAP);
    ui_set_text(ui->status_label, "Connection settings are not saved.");

    ui->group = lv_group_create();
    lv_group_add_obj(ui->group, ui->host_input);
    lv_group_add_obj(ui->group, ui->username_input);
    lv_group_add_obj(ui->group, ui->domain_input);
    lv_group_add_obj(ui->group, ui->password_input);
    lv_group_add_obj(ui->group, ui->fps_dropdown);
    lv_group_add_obj(ui->group, ui->audio_buffer_slider);
    lv_group_add_obj(ui->group, ui->mouse_mode_switch);
    lv_group_add_obj(ui->group, ui->jump_filter_switch);
    lv_group_add_obj(ui->group, ui->connect_btn);
    lv_group_focus_obj(ui->host_input);
    ui_scroll_focused_into_view(ui);

    ui_update_connect_state(ui);
}

NativePreconnectUi *native_preconnect_ui_create(SDL_Window *window, SDL_Renderer *renderer, const char *host,
                                                uint16_t port, const char *username, const char *password,
                                                const char *domain, uint16_t fps, uint16_t audio_prebuffer_ms,
                                                bool relative_mouse, bool jump_filter) {
    if (!window || !renderer) {
        return NULL;
    }

    NativePreconnectUi *ui = (NativePreconnectUi *)calloc(1, sizeof(*ui));
    if (!ui) {
        return NULL;
    }
    ui->window = window;
    ui->renderer = renderer;
    ui->visible = true;
    ui->relative_mouse = relative_mouse;
    ui->jump_filter = jump_filter;

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
        fprintf(stderr, "[native-ui] failed to create LVGL screen texture\n");
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
    ui->disp->bg_color = lv_color_hex(0x15171a);
    ui->disp->bg_opa = LV_OPA_COVER;

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
    ui_build(ui, host, port, username, password, domain, fps, audio_prebuffer_ms);
    if (ui->key_indev && ui->group) {
        lv_indev_set_group(ui->key_indev, ui->group);
    }
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
    lv_timer_handler();
}

void native_preconnect_ui_set_visible(NativePreconnectUi *ui, bool visible) {
    if (!ui || ui->visible == visible) {
        return;
    }
    ui->visible = visible;
    ui->hidden_cleared = false;
    if (ui->root) {
        if (visible) {
            lv_obj_clear_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(ui->root);
        } else {
            lv_obj_add_flag(ui->root, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void native_preconnect_ui_set_connecting(NativePreconnectUi *ui, bool connecting, const char *status) {
    if (!ui) {
        return;
    }
    ui->connecting = connecting;
    if (ui->host_input) {
        if (connecting) {
            lv_obj_add_state(ui->host_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->username_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->domain_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->password_input, LV_STATE_DISABLED);
            lv_obj_add_state(ui->fps_dropdown, LV_STATE_DISABLED);
            lv_obj_add_state(ui->mouse_mode_switch, LV_STATE_DISABLED);
            lv_obj_add_state(ui->jump_filter_switch, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(ui->host_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->username_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->domain_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->password_input, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->fps_dropdown, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->mouse_mode_switch, LV_STATE_DISABLED);
            lv_obj_clear_state(ui->jump_filter_switch, LV_STATE_DISABLED);
        }
    }
    native_preconnect_ui_set_status(ui, status, false);
    ui_update_connect_state(ui);
}

void native_preconnect_ui_set_status(NativePreconnectUi *ui, const char *status, bool error) {
    if (!ui || !ui->status_label) {
        return;
    }
    ui_set_text(ui->status_label, status);
    lv_obj_set_style_text_color(ui->status_label, error ? lv_color_hex(0xff7a7a) : lv_color_hex(0xaeb6bf), 0);
}

bool native_preconnect_ui_take_connect(NativePreconnectUi *ui, char *host, size_t host_cap, uint16_t *port,
                                       char *username, size_t username_cap, char *password, size_t password_cap,
                                       char *domain, size_t domain_cap, uint16_t *fps, uint16_t *audio_prebuffer_ms,
                                       bool *relative_mouse, bool *jump_filter) {
    if (!ui || !ui->connect_requested) {
        return false;
    }
    ui->connect_requested = false;
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
    if (audio_prebuffer_ms) {
        *audio_prebuffer_ms = ui->requested_audio_buffer;
    }
    if (relative_mouse) {
        *relative_mouse = ui->requested_relative_mouse;
    }
    if (jump_filter) {
        *jump_filter = ui->requested_jump_filter;
    }
    return true;
}

bool native_preconnect_ui_read_current(NativePreconnectUi *ui, char *host, size_t host_cap, uint16_t *port,
                                       char *username, size_t username_cap, char *password, size_t password_cap,
                                       char *domain, size_t domain_cap, uint16_t *fps, uint16_t *audio_prebuffer_ms,
                                       bool *relative_mouse, bool *jump_filter) {
    if (!ui) {
        return false;
    }

    uint16_t parsed_port = 0;
    char parsed_host[UI_HOST_MAX];
    if (!ui_parse_endpoint(lv_textarea_get_text(ui->host_input), parsed_host, sizeof(parsed_host), &parsed_port)) {
        return false;
    }

    const char *current_username = lv_textarea_get_text(ui->username_input);
    const char *current_password = lv_textarea_get_text(ui->password_input);
    const char *current_domain = lv_textarea_get_text(ui->domain_input);
    if (!current_username || !current_password) {
        return false;
    }

    if (host && host_cap > 0) {
        size_t len = strlen(parsed_host);
        if (len >= host_cap) {
            return false;
        }
        memcpy(host, parsed_host, len + 1);
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
    if (audio_prebuffer_ms) {
        *audio_prebuffer_ms = ui->selected_audio_buffer;
    }
    if (relative_mouse) {
        *relative_mouse = ui->relative_mouse;
    }
    if (jump_filter) {
        *jump_filter = ui->jump_filter;
    }
    return true;
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
        SDL_SetRenderDrawColor(ui->renderer, 0x15, 0x17, 0x1a, 0xff);
        SDL_RenderClear(ui->renderer);
        SDL_SetTextureBlendMode(ui->texture, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(ui->renderer, ui->texture, NULL, NULL);
        SDL_RenderPresent(ui->renderer);
        SDL_SetRenderTarget(ui->renderer, ui->texture);
    }
    lv_disp_flush_ready(drv);
}

static void ui_clear(lv_disp_drv_t *drv, uint8_t *buf, uint32_t size) {
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

static void ui_key_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    NativePreconnectUi *ui = (NativePreconnectUi *)drv->user_data;
    NativePreconnectKeyDriver *state = (NativePreconnectKeyDriver *)drv;
    SDL_Event event;

    if (ui->connecting) {
        while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_KEYDOWN, SDL_TEXTINPUT) > 0) {
        }
        state->text_len = 0;
        state->text_pos = 0;
        state->text_key_down = false;
        state->state = LV_INDEV_STATE_RELEASED;
        data->key = state->key;
        data->state = state->state;
        data->continue_reading = false;
        return;
    }

    if (state->text_key_down) {
        state->text_key_down = false;
        state->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = state->text_pos < state->text_len;
    } else if (state->text_pos < state->text_len) {
        state->key = (unsigned char)state->text[state->text_pos++];
        state->state = LV_INDEV_STATE_PRESSED;
        state->text_key_down = true;
        data->continue_reading = true;
    } else if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_KEYDOWN, SDL_KEYUP) > 0) {
        state->text_key_down = false;
        if (ui_key_from_sdl(&event.key, &state->key)) {
            state->state = event.type == SDL_KEYDOWN ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        } else if (event.type == SDL_KEYDOWN && !SDL_HasEvent(SDL_TEXTINPUT) && ui_text_key_from_sdl(&event.key, &state->key)) {
            state->state = LV_INDEV_STATE_PRESSED;
        } else {
            state->state = LV_INDEV_STATE_RELEASED;
        }
        data->continue_reading = true;
    } else if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_TEXTINPUT, SDL_TEXTINPUT) > 0) {
        size_t len = strlen(event.text.text);
        if (len >= sizeof(state->text)) {
            len = sizeof(state->text) - 1;
        }
        memcpy(state->text, event.text.text, len);
        state->text[len] = '\0';
        state->text_len = len;
        state->text_pos = 0;
        state->text_key_down = false;
        state->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = len > 0;
    } else {
        data->continue_reading = false;
    }

    data->key = state->key;
    data->state = state->state;
}

static void ui_input_changed(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    native_preconnect_ui_set_status(ui, "Connection settings are not saved.", false);
    ui_update_connect_state(ui);
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

static void ui_form_key_event(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || !ui->group || ui->connecting) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(event);
    if (target == ui->fps_dropdown && lv_dropdown_is_open(ui->fps_dropdown)) {
        return;
    }

    uint32_t key = lv_event_get_key(event);
    if (key == LV_KEY_DOWN) {
        lv_group_focus_next(ui->group);
        ui_scroll_focused_into_view(ui);
        lv_event_stop_processing(event);
    } else if (key == LV_KEY_UP) {
        lv_group_focus_prev(ui->group);
        ui_scroll_focused_into_view(ui);
        lv_event_stop_processing(event);
    }
}

static void ui_fps_changed(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting) {
        return;
    }
    ui_set_selected_fps(ui, lv_dropdown_get_selected(ui->fps_dropdown));
    native_preconnect_ui_set_status(ui, "Connection settings are not saved.", false);
}

static void ui_audio_buffer_changed(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting) {
        return;
    }
    int32_t raw = lv_slider_get_value(ui->audio_buffer_slider);
    if (raw < 0) {
        raw = 0;
    }
    ui->selected_audio_buffer = ui_audio_buffer_clamp((uint16_t)(raw * UI_AUDIO_BUFFER_STEP_MS));
    ui_update_audio_buffer_label(ui);
    native_preconnect_ui_set_status(ui, "Connection settings are not saved.", false);
}

static void ui_mouse_mode_changed(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting) {
        return;
    }
    ui->relative_mouse = lv_obj_has_state(ui->mouse_mode_switch, LV_STATE_CHECKED);
    ui_update_mouse_mode_state(ui);
    native_preconnect_ui_set_status(ui, "Connection settings are not saved.", false);
}

static void ui_jump_filter_changed(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting) {
        return;
    }
    ui->jump_filter = lv_obj_has_state(ui->jump_filter_switch, LV_STATE_CHECKED);
    native_preconnect_ui_set_status(ui, "Connection settings are not saved.", false);
}

static void ui_connect_clicked(lv_event_t *event) {
    NativePreconnectUi *ui = (NativePreconnectUi *)lv_event_get_user_data(event);
    if (!ui || ui->connecting || !ui_form_valid(ui)) {
        ui_update_connect_state(ui);
        return;
    }
    const char *endpoint = lv_textarea_get_text(ui->host_input);
    const char *username = lv_textarea_get_text(ui->username_input);
    const char *domain = lv_textarea_get_text(ui->domain_input);
    const char *password = lv_textarea_get_text(ui->password_input);
    uint16_t port = 0;
    char host[UI_HOST_MAX];
    if (!ui_parse_endpoint(endpoint, host, sizeof(host), &port)) {
        native_preconnect_ui_set_status(ui, "Enter address as host:port.", true);
        ui_update_connect_state(ui);
        return;
    }
    if (!username || !username[0] || !password || !password[0]) {
        native_preconnect_ui_set_status(ui, "Enter username and password.", true);
        ui_update_connect_state(ui);
        return;
    }
    size_t len = strlen(host);
    if (len >= sizeof(ui->requested_host)) {
        native_preconnect_ui_set_status(ui, "Host value is too long.", true);
        return;
    }
    memcpy(ui->requested_host, host, len + 1);
    len = strlen(username);
    if (len >= sizeof(ui->requested_username)) {
        native_preconnect_ui_set_status(ui, "Username value is too long.", true);
        return;
    }
    memcpy(ui->requested_username, username, len + 1);
    len = strlen(domain ? domain : "");
    if (len >= sizeof(ui->requested_domain)) {
        native_preconnect_ui_set_status(ui, "Domain value is too long.", true);
        return;
    }
    memcpy(ui->requested_domain, domain ? domain : "", len + 1);
    len = strlen(password);
    if (len >= sizeof(ui->requested_password)) {
        native_preconnect_ui_set_status(ui, "Password value is too long.", true);
        return;
    }
    memcpy(ui->requested_password, password, len + 1);
    ui->requested_port = port;
    ui->requested_fps = ui->selected_fps;
    ui->requested_audio_buffer = ui->selected_audio_buffer;
    ui->requested_relative_mouse = ui->relative_mouse;
    ui->requested_jump_filter = ui->jump_filter;
    ui->connect_requested = true;
}
