#include "ui_mixer.h"

int32_t native_ui_mixer_gain_db_to_q15(int gain_db) {
    static const int32_t table[] = {0,    46,   65,    92,    130,   184,   260,  368,  519,  734,  1036, 1464,
                                    2068, 2920, 4125,  5827,  8231,  11627, 16423, 23198, 32768, 46286, 65381};
    if (gain_db <= NATIVE_MIXER_FADER_MIN_DB) {
        return 0;
    }
    if (gain_db > NATIVE_MIXER_FADER_MAX_DB) {
        gain_db = NATIVE_MIXER_FADER_MAX_DB;
    }
    return table[(gain_db - NATIVE_MIXER_FADER_MIN_DB) / 3];
}

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL

/* Remote-button order red/green/yellow/blue, then the MASTER's neutral white — same
 * palette as the session badges. */
static const uint32_t ui_mixer_channel_colors[NATIVE_UI_MIXER_CHANNELS] = {0xff4136, 0x2ecc40, 0xffdc00, 0x0074d9,
                                                                           0xffffff};

/* Both faders travel the same track; only the value domain differs (dB vs percent). */
static int ui_mixer_master_pct_clamped(int pct) {
    if (pct < 0) {
        return 0; /* unknown volume parks the (dimmed) knob at the bottom stop */
    }
    return pct > 100 ? 100 : pct;
}

/* Horizontal capsule (rounded bar) out of three rects — smooth enough at 1080p from
 * couch distance; SDL2 has no filled-rounded-rect primitive. */
static void ui_mixer_fill_capsule(SDL_Renderer *renderer, SDL_Rect rect) {
    int cap = rect.h / 5;
    if (cap < 1) {
        cap = 1;
    }
    SDL_Rect body = {rect.x, rect.y + cap, rect.w, rect.h - 2 * cap};
    SDL_RenderFillRect(renderer, &body);
    SDL_Rect cap_top = {rect.x + cap + 1, rect.y, rect.w - 2 * (cap + 1), cap};
    SDL_Rect cap_bottom = {rect.x + cap + 1, rect.y + rect.h - cap, rect.w - 2 * (cap + 1), cap};
    SDL_RenderFillRect(renderer, &cap_top);
    SDL_RenderFillRect(renderer, &cap_bottom);
}

void native_ui_mixer_draw_fallback(SDL_Renderer *renderer, const int8_t *gain_db, int master_pct, int selected,
                                   unsigned connected_mask) {
    if (!renderer || !gain_db) {
        return;
    }
    int output_width = 0;
    int output_height = 0;
    if (SDL_GetRendererOutputSize(renderer, &output_width, &output_height) != 0) {
        output_width = 1920; /* the fixed webOS logical canvas */
        output_height = 1080;
    }
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    const int column_w = 150;
    const int track_h = 480;
    const int pad_x = 40;
    const int pad_top = 48;
    const int bottom_area = 96; /* room for the slot color bar under the rails */
    const int handle_w = 60;
    const int handle_h = 18;
    SDL_Rect panel;
    panel.w = NATIVE_UI_MIXER_CHANNELS * column_w + 2 * pad_x;
    panel.h = pad_top + track_h + bottom_area;
    panel.x = (output_width - panel.w) / 2;
    panel.y = (output_height - panel.h) / 2;
    SDL_SetRenderDrawColor(renderer, 8, 10, 14, 200);
    SDL_RenderFillRect(renderer, &panel);

    for (int i = 0; i < NATIVE_UI_MIXER_CHANNELS; i++) {
        int col_x = panel.x + pad_x + i * column_w;
        int track_y = panel.y + pad_top;
        /* Rails sit right of the column center so the tick scale fits on their left. */
        int center_x = col_x + column_w / 2 + 12;
        bool is_selected = i == selected;

        if (i > 0) {
            SDL_Rect divider = {col_x, panel.y + 12, 1, panel.h - 24};
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 22);
            SDL_RenderFillRect(renderer, &divider);
        }
        if (is_selected) {
            SDL_Rect backplate = {col_x + 6, panel.y + 12, column_w - 12, panel.h - 24};
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 16);
            SDL_RenderFillRect(renderer, &backplate);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
            SDL_RenderDrawRect(renderer, &backplate);
        }

        /* Tick scale: a dot per step, a longer dash at the quarter marks. */
        int ticks_x = center_x - 46;
        for (int k = 0; k <= 20; k++) {
            int tick_y = track_y + track_h * k / 20;
            if (k % 5 == 0) {
                SDL_Rect dash = {ticks_x - 2, tick_y - 1, 10, 3};
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 150);
                SDL_RenderFillRect(renderer, &dash);
            } else {
                SDL_Rect dot = {ticks_x + 3, tick_y - 1, 3, 3};
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 80);
                SDL_RenderFillRect(renderer, &dot);
            }
        }

        /* Three thin fader rails. */
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, is_selected ? 120 : 70);
        for (int rail = -1; rail <= 1; rail++) {
            SDL_Rect rail_rect = {center_x + rail * 11 - 1, track_y, 3, track_h};
            SDL_RenderFillRect(renderer, &rail_rect);
        }

        bool connected = (connected_mask & (1u << i)) != 0;

        /* The capsule handle rides the rails: +6 dB at the top, -60 (mute) at the bottom
         * — or 100..0 for the MASTER, whose fader is the system volume. */
        int handle_y;
        if (i == NATIVE_UI_MIXER_MASTER) {
            handle_y = track_y + (track_h - handle_h) * (100 - ui_mixer_master_pct_clamped(master_pct)) / 100;
        } else {
            handle_y = track_y + (track_h - handle_h) * (NATIVE_MIXER_FADER_MAX_DB - (int)gain_db[i]) /
                                     (NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB);
        }
        SDL_Rect handle = {center_x - handle_w / 2, handle_y, handle_w, handle_h};
        uint8_t handle_alpha = connected ? (is_selected ? 255 : 185) : 90;
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, handle_alpha);
        ui_mixer_fill_capsule(renderer, handle);
        /* Grip dots along the handle center. */
        SDL_SetRenderDrawColor(renderer, 20, 22, 26, 200);
        for (int dot = -2; dot <= 1; dot++) {
            SDL_Rect grip = {center_x + dot * 8 + 3, handle_y + handle_h / 2 - 1, 3, 3};
            SDL_RenderFillRect(renderer, &grip);
        }

        /* Channel color bar under the fader — the channel "label" (no fonts here). */
        uint32_t color = ui_mixer_channel_colors[i];
        SDL_SetRenderDrawColor(renderer, (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color,
                               connected ? 235 : 96);
        SDL_Rect color_bar = {center_x - handle_w / 2, track_y + track_h + 40, handle_w, 10};
        ui_mixer_fill_capsule(renderer, color_bar);
    }
    SDL_RenderPresent(renderer);
}

#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI

#include <math.h>
#include <stdlib.h>

#include "lvgl.h"

#define UI_MIXER_CHANNEL_W 170
#define UI_MIXER_CHANNEL_H 620
#define UI_MIXER_FADER_Y 28
#define UI_MIXER_FADER_H 520
#define UI_MIXER_METER_W 14
#define UI_MIXER_METER_GAP 10
#define UI_MIXER_KNOB_W 66
#define UI_MIXER_KNOB_H 22
/* The L/R meter pair sits right of the channel center so the tick scale fits left. */
#define UI_MIXER_PAIR_CX (UI_MIXER_CHANNEL_W / 2 + 14)

struct NativeUiMixer {
    SDL_Renderer *renderer;
    SDL_Texture *texture; /* the host display's screen texture; tracks resizes */
    lv_disp_t *disp;
    lv_obj_t *screen;
    lv_obj_t *restore_screen; /* whatever was active when the overlay opened */
    lv_obj_t *channels[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *knobs[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *meter_clips[NATIVE_UI_MIXER_CHANNELS][2];
    lv_obj_t *meter_grads[NATIVE_UI_MIXER_CHANNELS][2];
    lv_obj_t *color_bars[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *latency_labels[NATIVE_UI_MIXER_CHANNELS]; /* NULL for the MASTER */
    /* Rendered per-frame while up: cache the last-pushed values so only changed widgets
     * are touched (LVGL then redraws only those areas). knob_value carries dB for the
     * slots and percent for the MASTER — it is only ever compared for change. */
    int meter_px[NATIVE_UI_MIXER_CHANNELS][2];
    int knob_value[NATIVE_UI_MIXER_CHANNELS];
    /* Channel-header latency readouts: refreshed at a calm cadence so the number is
     * readable rather than a per-chunk blur. */
    unsigned latency_shown[NATIVE_UI_MIXER_CHANNELS];
    uint32_t latency_ticks;
    int selected;
    unsigned mask;
    /* Displayed level per channel/side in dBFS (instant attack, steady release). */
    float meter_db[NATIVE_UI_MIXER_CHANNELS][2];
    uint32_t meter_ticks;
    bool active;
    bool full_refresh;
};

static int ui_mixer_db_to_y(int db) {
    return UI_MIXER_FADER_Y +
           (NATIVE_MIXER_FADER_MAX_DB - db) * UI_MIXER_FADER_H / (NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB);
}

static int ui_mixer_pct_to_y(int pct) {
    return UI_MIXER_FADER_Y + (100 - ui_mixer_master_pct_clamped(pct)) * UI_MIXER_FADER_H / 100;
}

/* The panel is centered and its layout fully deterministic (flex row, fixed channel
 * width/gaps), so pointer hit-testing is pure arithmetic — no LVGL involved. */
#define UI_MIXER_PANEL_W (NATIVE_UI_MIXER_CHANNELS * UI_MIXER_CHANNEL_W + (NATIVE_UI_MIXER_CHANNELS - 1) * 12 + 2 * 28)
#define UI_MIXER_PANEL_H (UI_MIXER_CHANNEL_H + 2 * 28)

bool native_ui_mixer_hit_test(int win_w, int win_h, int x, int y, int *slot, bool *on_fader) {
    if (slot) {
        *slot = -1;
    }
    if (on_fader) {
        *on_fader = false;
    }
    int panel_x = (win_w - UI_MIXER_PANEL_W) / 2;
    int panel_y = (win_h - UI_MIXER_PANEL_H) / 2;
    if (x < panel_x || x >= panel_x + UI_MIXER_PANEL_W || y < panel_y || y >= panel_y + UI_MIXER_PANEL_H) {
        return false;
    }
    int rel_x = x - panel_x - 28;
    if (slot && rel_x >= 0) {
        int i = rel_x / (UI_MIXER_CHANNEL_W + 12);
        if (i < NATIVE_UI_MIXER_CHANNELS && rel_x - i * (UI_MIXER_CHANNEL_W + 12) < UI_MIXER_CHANNEL_W) {
            *slot = i;
        }
    }
    if (on_fader) {
        /* Within the fader track vertically (a knob's worth of slop at both ends): a
         * click here jumps/drags the fader; elsewhere in the channel it only selects —
         * clicking the color label must not slam the level to the bottom stop. */
        int fy = y - (panel_y + 28 + UI_MIXER_FADER_Y);
        *on_fader = fy >= -UI_MIXER_KNOB_H && fy <= UI_MIXER_FADER_H + UI_MIXER_KNOB_H;
    }
    return true;
}

int native_ui_mixer_fader_db_at(int win_h, int y) {
    const int span = NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB;
    int track_y = (win_h - UI_MIXER_PANEL_H) / 2 + 28 + UI_MIXER_FADER_Y;
    int fy = y - track_y;
    if (fy < 0) {
        fy = 0;
    }
    if (fy > UI_MIXER_FADER_H) {
        fy = UI_MIXER_FADER_H;
    }
    int db = NATIVE_MIXER_FADER_MAX_DB - (fy * span + UI_MIXER_FADER_H / 2) / UI_MIXER_FADER_H;
    /* Snap onto the 3 dB fader steps (the gain LUT is indexed by them). */
    db = ((db - NATIVE_MIXER_FADER_MIN_DB + NATIVE_MIXER_OVERLAY_GAIN_STEP_DB / 2) /
          NATIVE_MIXER_OVERLAY_GAIN_STEP_DB) *
             NATIVE_MIXER_OVERLAY_GAIN_STEP_DB +
         NATIVE_MIXER_FADER_MIN_DB;
    if (db > NATIVE_MIXER_FADER_MAX_DB) {
        db = NATIVE_MIXER_FADER_MAX_DB;
    }
    return db;
}

int native_ui_mixer_fader_pct_at(int win_h, int y) {
    int track_y = (win_h - UI_MIXER_PANEL_H) / 2 + 28 + UI_MIXER_FADER_Y;
    int fy = y - track_y;
    if (fy < 0) {
        fy = 0;
    }
    if (fy > UI_MIXER_FADER_H) {
        fy = UI_MIXER_FADER_H;
    }
    return 100 - (fy * 100 + UI_MIXER_FADER_H / 2) / UI_MIXER_FADER_H;
}

/* One channel per session slot, styled after a hardware fader bank: an L/R pair of
 * meter columns on slim rails, one white pill knob across both, a dBFS tick scale on
 * the left (numbers at the 15s, dots every 3 dB — including the unmarked +6 headroom
 * above the 0 line) and the slot's color bar as the channel label at the bottom. */
static void ui_mixer_build_channel(NativeUiMixer *mixer, lv_obj_t *panel, int slot, uint32_t color) {
    lv_obj_t *channel = lv_obj_create(panel);
    mixer->channels[slot] = channel;
    lv_obj_set_size(channel, UI_MIXER_CHANNEL_W, UI_MIXER_CHANNEL_H);
    lv_obj_clear_flag(channel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(channel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(channel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(channel, 16, 0);
    lv_obj_set_style_border_width(channel, 2, 0);
    lv_obj_set_style_border_color(channel, lv_color_white(), 0);
    lv_obj_set_style_border_opa(channel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(channel, 0, 0);

    /* L/R rails, each carrying an ANCHORED meter gradient: the clip window grows from
     * the bottom while the full-scale gradient child stays fixed against the dB scale,
     * so the yellow end only ever shows at loud levels (like a real meter bridge). */
    for (int side = 0; side < 2; side++) {
        int x = side == 0 ? UI_MIXER_PAIR_CX - UI_MIXER_METER_GAP / 2 - UI_MIXER_METER_W
                          : UI_MIXER_PAIR_CX + UI_MIXER_METER_GAP / 2;
        lv_obj_t *rail = lv_obj_create(channel);
        lv_obj_set_pos(rail, x, UI_MIXER_FADER_Y);
        lv_obj_set_size(rail, UI_MIXER_METER_W, UI_MIXER_FADER_H);
        lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(rail, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(rail, LV_OPA_10, 0);
        lv_obj_set_style_radius(rail, 5, 0);
        lv_obj_set_style_border_width(rail, 0, 0);

        lv_obj_t *clip = lv_obj_create(channel);
        mixer->meter_clips[slot][side] = clip;
        lv_obj_set_pos(clip, x, UI_MIXER_FADER_Y + UI_MIXER_FADER_H);
        lv_obj_set_size(clip, UI_MIXER_METER_W, 10);
        lv_obj_clear_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(clip, 0, 0);
        lv_obj_set_style_radius(clip, 0, 0);
        lv_obj_set_style_pad_all(clip, 0, 0);
        lv_obj_add_flag(clip, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *grad = lv_obj_create(clip);
        mixer->meter_grads[slot][side] = grad;
        lv_obj_set_pos(grad, 0, -UI_MIXER_FADER_H);
        lv_obj_set_size(grad, UI_MIXER_METER_W, UI_MIXER_FADER_H);
        lv_obj_clear_flag(grad, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(grad, 4, 0);
        lv_obj_set_style_border_width(grad, 0, 0);
        lv_obj_set_style_bg_color(grad, lv_color_hex(0xf7d038), 0);      /* loud end */
        lv_obj_set_style_bg_grad_color(grad, lv_color_hex(0x27cf5a), 0); /* quiet end */
        lv_obj_set_style_bg_grad_dir(grad, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(grad, LV_OPA_COVER, 0);
    }

    /* Tick scale: every channel wears the same dBFS artwork — including the MASTER,
     * whose fader actually travels system-volume percent over the full track. The
     * mismatch is deliberate (a uniform bank reads better from the couch); only the
     * knob position and the input mapping are percent there. */
    for (int db = NATIVE_MIXER_FADER_MAX_DB; db >= NATIVE_MIXER_FADER_MIN_DB; db -= 3) {
        int y = ui_mixer_db_to_y(db);
        if (db <= 0 && db % 15 == 0) {
            lv_obj_t *tick = lv_label_create(channel);
            lv_obj_set_style_text_font(tick, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(tick, lv_color_white(), 0);
            lv_obj_set_style_text_opa(tick, LV_OPA_60, 0);
            lv_obj_set_style_text_align(tick, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_width(tick, 34);
            lv_label_set_text_fmt(tick, "%d", -db); /* console style: unsigned numbers */
            lv_obj_set_pos(tick, 22, y - 8);
        } else {
            lv_obj_t *dot = lv_obj_create(channel);
            lv_obj_set_size(dot, 4, 4);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_40, 0);
            lv_obj_set_pos(dot, 58, y - 2);
        }
    }

    lv_obj_t *knob = lv_obj_create(channel);
    mixer->knobs[slot] = knob;
    lv_obj_set_size(knob, UI_MIXER_KNOB_W, UI_MIXER_KNOB_H);
    lv_obj_set_pos(knob, UI_MIXER_PAIR_CX - UI_MIXER_KNOB_W / 2, ui_mixer_db_to_y(0) - UI_MIXER_KNOB_H / 2);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(knob, lv_color_white(), 0);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_shadow_width(knob, 16, 0);
    lv_obj_set_style_shadow_opa(knob, LV_OPA_50, 0);
    lv_obj_set_style_shadow_color(knob, lv_color_black(), 0);

    if (slot != NATIVE_UI_MIXER_MASTER) {
        /* Channel-header readout: the session's client-held audio queue in ms. */
        lv_obj_t *latency = lv_label_create(channel);
        mixer->latency_labels[slot] = latency;
        lv_obj_set_style_text_font(latency, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(latency, lv_color_white(), 0);
        lv_obj_set_style_text_opa(latency, LV_OPA_60, 0);
        /* Right-aligned in a fixed box centered on the channel: the "ms" suffix stays
         * anchored and extra digits grow leftwards, so the readout never jiggles when
         * the value crosses a digit boundary. Sized for four digits (ring caps allow
         * seconds of backlog). */
        lv_obj_set_style_text_align(latency, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(latency, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(latency, 56);
        lv_label_set_text(latency, "");
        lv_obj_set_pos(latency, UI_MIXER_CHANNEL_W / 2 - 28, 6);
    }

    lv_obj_t *bar = lv_obj_create(channel);
    mixer->color_bars[slot] = bar;
    lv_obj_set_size(bar, 64, 8);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -20);
}

NativeUiMixer *native_ui_mixer_create(SDL_Renderer *renderer) {
    if (!renderer) {
        return NULL;
    }
    NativeUiMixer *mixer = (NativeUiMixer *)calloc(1, sizeof(*mixer));
    if (!mixer) {
        return NULL;
    }
    mixer->renderer = renderer;
    mixer->disp = lv_disp_get_default();
    mixer->screen = lv_obj_create(NULL);
    if (!mixer->disp || !mixer->screen) {
        if (mixer->screen) {
            lv_obj_del(mixer->screen);
        }
        free(mixer);
        return NULL;
    }
    /* The screen itself is a hole: only the panel paints, the video plane fills the rest. */
    lv_obj_set_style_bg_opa(mixer->screen, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(mixer->screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(mixer->screen);
    lv_obj_set_size(panel, UI_MIXER_PANEL_W, UI_MIXER_PANEL_H);
    lv_obj_center(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0b0d12), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(panel, 48, 0);
    lv_obj_set_style_shadow_ofs_y(panel, 12, 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_black(), 0);
    lv_obj_set_style_pad_all(panel, 28, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int slot = 0; slot < NATIVE_UI_MIXER_CHANNELS; slot++) {
        ui_mixer_build_channel(mixer, panel, slot, ui_mixer_channel_colors[slot]);
    }
    return mixer;
}

void native_ui_mixer_destroy(NativeUiMixer *mixer) {
    if (!mixer) {
        return;
    }
    if (mixer->screen) {
        lv_obj_del(mixer->screen);
    }
    free(mixer);
}

void native_ui_mixer_set_texture(NativeUiMixer *mixer, SDL_Texture *texture) {
    if (mixer) {
        mixer->texture = texture;
    }
}

bool native_ui_mixer_active(const NativeUiMixer *mixer) {
    return mixer && mixer->active;
}

void native_ui_mixer_show(NativeUiMixer *mixer) {
    if (!mixer || mixer->active) {
        return;
    }
    mixer->active = true;
    /* Poison the caches so the first render pushes every widget; meters start from
     * silence and attack to the live level on the first frames. */
    for (int i = 0; i < NATIVE_UI_MIXER_CHANNELS; i++) {
        mixer->meter_px[i][0] = -1;
        mixer->meter_px[i][1] = -1;
        mixer->knob_value[i] = INT32_MAX;
        mixer->latency_shown[i] = UINT32_MAX;
        mixer->meter_db[i][0] = NATIVE_MIXER_METER_FLOOR_DB;
        mixer->meter_db[i][1] = NATIVE_MIXER_METER_FLOOR_DB;
    }
    mixer->meter_ticks = SDL_GetTicks();
    mixer->latency_ticks = 0; /* first render publishes immediately */
    mixer->selected = -2;
    mixer->mask = 0xffffffffu;
    mixer->full_refresh = true;
    mixer->restore_screen = lv_scr_act();
    /* No display background either — the host's flush clears to transparent while the
     * overlay is active (see native_ui_mixer_active). */
    mixer->disp->bg_opa = LV_OPA_TRANSP;
    lv_scr_load(mixer->screen);
}

void native_ui_mixer_hide(NativeUiMixer *mixer) {
    if (!mixer || !mixer->active) {
        return;
    }
    mixer->active = false;
    mixer->disp->bg_opa = LV_OPA_COVER;
    if (mixer->restore_screen) {
        lv_scr_load(mixer->restore_screen);
    }
    /* No repaint here: during streaming the app repaints the punch frame on its next
     * tick, and back on the configurator the normal tick path re-renders the form. */
}

void native_ui_mixer_render(NativeUiMixer *mixer, const int32_t (*peaks)[2], const unsigned *latency_ms,
                            const int8_t *gain_db, int master_pct, int selected, unsigned connected_mask,
                            uint32_t now_ticks) {
    if (!mixer || !peaks || !latency_ms || !gain_db || !mixer->active) {
        return;
    }
    /* Latency readouts tick at 4 Hz: the queue depth breathes with every 21ms chunk,
     * and a calmer number is a readable one. */
    if (SDL_TICKS_PASSED(now_ticks, mixer->latency_ticks)) {
        mixer->latency_ticks = now_ticks + 250u;
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            bool connected = (connected_mask & (1u << i)) != 0;
            unsigned shown = connected ? latency_ms[i] : UINT32_MAX - 1u;
            if (shown == mixer->latency_shown[i] || !mixer->latency_labels[i]) {
                continue;
            }
            mixer->latency_shown[i] = shown;
            if (connected) {
                lv_label_set_text_fmt(mixer->latency_labels[i], "%u ms", latency_ms[i]);
            } else {
                lv_label_set_text(mixer->latency_labels[i], "");
            }
        }
    }
    /* Meter ballistics: instant attack to the audio mixer's post-fader chunk peak,
     * steady dB/s release. Wrap-safe tick math; a stalled loop just decays further. */
    float fall_db = (float)(now_ticks - mixer->meter_ticks) * (NATIVE_MIXER_METER_DECAY_DB_S / 1000.0f);
    mixer->meter_ticks = now_ticks;

    /* Called every frame while the overlay is up: touch only what changed, so LVGL
     * invalidates (and the GPU repaints) just the moving meters, not the whole panel.
     * With nothing changed lv_refr_now finds no dirty areas and presents nothing. */
    for (int i = 0; i < NATIVE_UI_MIXER_CHANNELS; i++) {
        for (int side = 0; side < 2; side++) {
            float target_db = peaks[i][side] > 0 ? 20.0f * log10f((float)peaks[i][side] / 32768.0f)
                                                 : NATIVE_MIXER_METER_FLOOR_DB;
            float fallen_db = mixer->meter_db[i][side] - fall_db;
            float level_db = target_db > fallen_db ? target_db : fallen_db;
            if (level_db < NATIVE_MIXER_METER_FLOOR_DB) {
                level_db = NATIVE_MIXER_METER_FLOOR_DB;
            }
            mixer->meter_db[i][side] = level_db;

            int px = 0;
            if (level_db > (float)NATIVE_MIXER_FADER_MIN_DB) {
                float f = (level_db - (float)NATIVE_MIXER_FADER_MIN_DB) * (float)UI_MIXER_FADER_H /
                          (float)(NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB);
                px = (int)f;
                if (px > UI_MIXER_FADER_H) {
                    px = UI_MIXER_FADER_H;
                }
                if (px < 4) {
                    px = 0; /* skip sub-radius slivers */
                }
            }
            if (px == mixer->meter_px[i][side]) {
                continue;
            }
            mixer->meter_px[i][side] = px;
            lv_obj_t *clip = mixer->meter_clips[i][side];
            if (px == 0) {
                lv_obj_add_flag(clip, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_clear_flag(clip, LV_OBJ_FLAG_HIDDEN);
            /* The clip window's bottom stays pinned to the scale bottom; the gradient
             * child slides so its full-scale artwork stays anchored to the dB scale. */
            lv_obj_set_y(clip, UI_MIXER_FADER_Y + UI_MIXER_FADER_H - px);
            lv_obj_set_height(clip, px);
            lv_obj_set_y(mixer->meter_grads[i][side], px - UI_MIXER_FADER_H);
        }
        int value = i == NATIVE_UI_MIXER_MASTER ? master_pct : gain_db[i];
        if (value != mixer->knob_value[i]) {
            mixer->knob_value[i] = value;
            int knob_y = i == NATIVE_UI_MIXER_MASTER ? ui_mixer_pct_to_y(value) : ui_mixer_db_to_y(value);
            lv_obj_set_y(mixer->knobs[i], knob_y - UI_MIXER_KNOB_H / 2);
        }
    }
    if (selected != mixer->selected || connected_mask != mixer->mask) {
        mixer->selected = selected;
        mixer->mask = connected_mask;
        for (int i = 0; i < NATIVE_UI_MIXER_CHANNELS; i++) {
            bool connected = (connected_mask & (1u << i)) != 0;
            bool is_selected = i == selected;
            lv_obj_set_style_bg_opa(mixer->channels[i], is_selected ? LV_OPA_10 : LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_opa(mixer->channels[i], is_selected ? LV_OPA_60 : LV_OPA_TRANSP, 0);
            /* Disconnected slots keep their fader (the level applies on connect) but
             * render the knob and label bar dimmed; their meters are empty anyway. */
            lv_obj_set_style_bg_opa(mixer->knobs[i], connected ? LV_OPA_COVER : LV_OPA_40, 0);
            lv_obj_set_style_bg_opa(mixer->color_bars[i], connected ? LV_OPA_COVER : LV_OPA_40, 0);
        }
    }
    if (mixer->full_refresh) {
        /* First frame after the screen switch: the display's clear_cb is a no-op (the
         * opaque form never needed it), but a TRANSPARENT screen must start from a clean
         * texture or stale form pixels linger around the panel. Later frames keep the
         * texture and repaint only the dirty areas. */
        mixer->full_refresh = false;
        if (mixer->texture) {
            SDL_SetRenderTarget(mixer->renderer, mixer->texture);
            SDL_SetRenderDrawColor(mixer->renderer, 0, 0, 0, 0);
            SDL_RenderClear(mixer->renderer);
        }
        lv_obj_invalidate(mixer->screen);
    }
    lv_refr_now(mixer->disp);
}

#endif /* HELLOLG_WITH_PRECONNECT_UI */
#endif /* HELLOLG_WITH_SDL */
