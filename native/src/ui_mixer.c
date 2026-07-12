#include "ui_mixer.h"

#if defined(HELLOLG_WITH_SDL) && HELLOLG_WITH_SDL
#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI
#include <math.h>
#include <stdlib.h>

#include "ui_fonts.h"
#include "ui_profile_name.h"
#include "ui_slot_palette.h"
#include "lvgl.h"
#endif
#endif

#include "clog.h"

clog_define(g_native_log_ui, cLogLevelInfo, cLogFlags_Default, "ui.mixer", NULL);

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

static const char *ui_mixer_channel_labels[NATIVE_UI_MIXER_CHANNELS] = {"RED", "GREEN", "YELLOW", "BLUE", "MASTER"};

/* Both faders travel the same track; only the value domain differs (dB vs percent). */
static int ui_mixer_master_pct_clamped(int pct) {
    if (pct < 0) {
        return 0; /* unknown volume parks the (dimmed) knob at the bottom stop */
    }
    return pct > 100 ? 100 : pct;
}

#if defined(HELLOLG_WITH_PRECONNECT_UI) && HELLOLG_WITH_PRECONNECT_UI

#define UI_MIXER_BANK_W 1260
#define UI_MIXER_BANK_H 668
#define UI_MIXER_PANEL_PAD_X 32
#define UI_MIXER_PANEL_PAD_TOP 22
#define UI_MIXER_PANEL_PAD_BOTTOM 22
#define UI_MIXER_PANEL_W (UI_MIXER_BANK_W + 2 * UI_MIXER_PANEL_PAD_X)
#define UI_MIXER_PANEL_H (UI_MIXER_BANK_H + UI_MIXER_PANEL_PAD_TOP + UI_MIXER_PANEL_PAD_BOTTOM)
#define UI_MIXER_PANEL_BOTTOM 48
#define UI_MIXER_PANEL_RADIUS 30
#define UI_MIXER_CHANNEL_Y 52
#define UI_MIXER_CHANNEL_H 570
#define UI_MIXER_SOURCE_W 218
#define UI_MIXER_MASTER_W 250
#define UI_MIXER_CHANNEL_GAP 18
#define UI_MIXER_MASTER_GAP 16
#define UI_MIXER_GROUP_W                                                                                              \
    (NATIVE_SETTINGS_MAX_SESSIONS * UI_MIXER_SOURCE_W + NATIVE_SETTINGS_MAX_SESSIONS * UI_MIXER_CHANNEL_GAP +        \
     UI_MIXER_MASTER_GAP + UI_MIXER_MASTER_W)
#define UI_MIXER_GROUP_X ((UI_MIXER_BANK_W - UI_MIXER_GROUP_W) / 2)
#define UI_MIXER_FADER_Y 100
#define UI_MIXER_FADER_H 380
#define UI_MIXER_METER_W 16
#define UI_MIXER_METER_GAP 22
#define UI_MIXER_KNOB_W 66
#define UI_MIXER_KNOB_H 26
#define UI_MIXER_PANEL_COLOR 0x10141b
#define UI_MIXER_STRIP_COLOR 0x101319
#define UI_MIXER_MASTER_COLOR 0x171a20
/* Peak-hold marks: a detached segment riding above each meter bar at the loudest
 * recent level — held, then released (console bridge style). */
#define UI_MIXER_PEAK_MARK_H 5
#define UI_MIXER_PEAK_HOLD_MS 1500u
#define UI_MIXER_PEAK_DECAY_DB_S 20.0f
/* Clip threshold: the honest 0 dBFS. The meters see the PRE-saturation sum, so unlike
 * hardware meters (which cannot read past their rail and warn early at ~-0.3 dB) red
 * here means the clamp really cut samples; a legal full-scale hit stays yellow. */
#define UI_MIXER_CLIP_DB (0.0f)
/* The channel's identity color lives in its fader KNOB (the MASTER's is white); the
 * channel bottom carries the M / D / S button row (mute, duck-trigger, solo — session
 * channels only). The split constants divide the bottom pointer band between the three
 * plates at the midpoints between them. */
#define UI_MIXER_MS_W 54
#define UI_MIXER_MS_H 54
#define UI_MIXER_MS_BOTTOM 18
#define UI_MIXER_MS_MUTE_X 18
#define UI_MIXER_MS_DUCK_X 82
#define UI_MIXER_MS_SOLO_X 146
#define UI_MIXER_MS_SPLIT_L 77
#define UI_MIXER_MS_SPLIT_R 141

struct NativeUiMixer {
    SDL_Renderer *renderer;
    SDL_Texture *texture; /* the host display's screen texture; tracks resizes */
    lv_disp_t *disp;
    lv_obj_t *screen;
    lv_obj_t *restore_screen; /* whatever was active when the overlay opened */
    lv_obj_t *channels[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *name_labels[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *knobs[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *value_labels[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *meter_clips[NATIVE_UI_MIXER_CHANNELS][2];
    lv_obj_t *meter_grads[NATIVE_UI_MIXER_CHANNELS][2];
    lv_obj_t *peak_marks[NATIVE_UI_MIXER_CHANNELS][2];
    lv_obj_t *knob_stripes[NATIVE_UI_MIXER_CHANNELS]; /* color (or neutral MASTER) inlay in the knob */
    lv_obj_t *mute_buttons[NATIVE_UI_MIXER_CHANNELS]; /* M/D/S plates+letters: NULL for the MASTER */
    lv_obj_t *mute_labels[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *duck_buttons[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *duck_labels[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *solo_buttons[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *solo_labels[NATIVE_UI_MIXER_CHANNELS];
    lv_obj_t *latency_labels[NATIVE_UI_MIXER_CHANNELS]; /* NULL for the MASTER */
    /* Rendered per-frame while up: cache the last-pushed values so only changed widgets
     * are touched (LVGL then redraws only those areas). knob_value carries dB for the
     * slots and percent for the MASTER — it is only ever compared for change. */
    int meter_px[NATIVE_UI_MIXER_CHANNELS][2];
    int knob_value[NATIVE_UI_MIXER_CHANNELS];
    /* Channel-header latency readouts: refreshed at a calm cadence so the number is
     * readable rather than a per-chunk blur. */
    unsigned queue_shown[NATIVE_UI_MIXER_CHANNELS];
    unsigned target_shown[NATIVE_UI_MIXER_CHANNELS];
    uint32_t latency_ticks;
    int selected;
    unsigned mask;
    /* M/D/S plate look last painted (connected|mute|solo|duck|active bits; -1 repaints). */
    int ms_state[NATIVE_UI_MIXER_CHANNELS];
    /* Displayed level per channel/side in dBFS (instant attack, steady release). */
    float meter_db[NATIVE_UI_MIXER_CHANNELS][2];
    /* Peak-hold ballistics: instant capture of the loudest chunk peak, held for
     * UI_MIXER_PEAK_HOLD_MS, then a steady release. */
    float peak_db[NATIVE_UI_MIXER_CHANNELS][2];
    uint32_t peak_since[NATIVE_UI_MIXER_CHANNELS][2];
    int peak_px[NATIVE_UI_MIXER_CHANNELS][2];
    bool peak_clip[NATIVE_UI_MIXER_CHANNELS][2]; /* mark currently painted clip-red */
    uint32_t meter_ticks;
    bool active;
    bool full_refresh;
};

static int ui_mixer_channel_width(int slot) {
    return slot == NATIVE_UI_MIXER_MASTER ? UI_MIXER_MASTER_W : UI_MIXER_SOURCE_W;
}

static int ui_mixer_channel_x(int slot) {
    if (slot == NATIVE_UI_MIXER_MASTER) {
        return UI_MIXER_GROUP_X + NATIVE_SETTINGS_MAX_SESSIONS * (UI_MIXER_SOURCE_W + UI_MIXER_CHANNEL_GAP) +
               UI_MIXER_MASTER_GAP;
    }
    return UI_MIXER_GROUP_X + slot * (UI_MIXER_SOURCE_W + UI_MIXER_CHANNEL_GAP);
}

static int ui_mixer_pair_cx(int slot) {
    return ui_mixer_channel_width(slot) / 2 + 24;
}

static int ui_mixer_db_to_y(int db) {
    return UI_MIXER_FADER_Y +
           (NATIVE_MIXER_FADER_MAX_DB - db) * UI_MIXER_FADER_H / (NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB);
}

static int ui_mixer_pct_to_y(int pct) {
    return UI_MIXER_FADER_Y + (100 - ui_mixer_master_pct_clamped(pct)) * UI_MIXER_FADER_H / 100;
}

/* The floating console and centered bank are deterministic, so pointer hit-testing
 * remains pure arithmetic — no LVGL traversal in the SDL event path. */

static int ui_mixer_panel_x(int win_w) {
    return (win_w - UI_MIXER_PANEL_W) / 2;
}

static int ui_mixer_panel_y(int win_h) {
    return win_h - UI_MIXER_PANEL_BOTTOM - UI_MIXER_PANEL_H;
}

static bool ui_mixer_point_in_panel(int win_w, int win_h, int x, int y) {
    int panel_x = ui_mixer_panel_x(win_w);
    int panel_y = ui_mixer_panel_y(win_h);
    if (x < panel_x || x >= panel_x + UI_MIXER_PANEL_W || y < panel_y || y >= panel_y + UI_MIXER_PANEL_H) {
        return false;
    }

    /* The shadow is deliberately not interactive, and the transparent pixels outside
     * the 30 px corner arcs count as outside the form too. */
    int rel_x = x - panel_x;
    int rel_y = y - panel_y;
    int dx = 0;
    int dy = 0;
    if (rel_x < UI_MIXER_PANEL_RADIUS) {
        dx = UI_MIXER_PANEL_RADIUS - rel_x;
    } else if (rel_x >= UI_MIXER_PANEL_W - UI_MIXER_PANEL_RADIUS) {
        dx = rel_x - (UI_MIXER_PANEL_W - UI_MIXER_PANEL_RADIUS - 1);
    }
    if (rel_y < UI_MIXER_PANEL_RADIUS) {
        dy = UI_MIXER_PANEL_RADIUS - rel_y;
    } else if (rel_y >= UI_MIXER_PANEL_H - UI_MIXER_PANEL_RADIUS) {
        dy = rel_y - (UI_MIXER_PANEL_H - UI_MIXER_PANEL_RADIUS - 1);
    }
    return dx == 0 || dy == 0 || dx * dx + dy * dy <= UI_MIXER_PANEL_RADIUS * UI_MIXER_PANEL_RADIUS;
}

bool native_ui_mixer_hit_test(int win_w, int win_h, int x, int y, int *slot, NativeUiMixerHit *zone) {
    if (slot) {
        *slot = -1;
    }
    if (zone) {
        *zone = NATIVE_UI_MIXER_HIT_BODY;
    }
    int panel_y = ui_mixer_panel_y(win_h);
    if (!ui_mixer_point_in_panel(win_w, win_h, x, y)) {
        return false;
    }
    int bank_x = (win_w - UI_MIXER_BANK_W) / 2;
    int rel_x = x - bank_x;
    int channel_y = panel_y + UI_MIXER_PANEL_PAD_TOP + UI_MIXER_CHANNEL_Y;
    int cy = y - channel_y;
    int cx = -1; /* channel-relative x once a slot resolves */
    int hit_slot = -1;
    if (cy >= 0 && cy < UI_MIXER_CHANNEL_H) {
        for (int i = 0; i < NATIVE_UI_MIXER_CHANNELS; i++) {
            int channel_x = ui_mixer_channel_x(i);
            int channel_w = ui_mixer_channel_width(i);
            if (rel_x >= channel_x && rel_x < channel_x + channel_w) {
                hit_slot = i;
                if (slot) {
                    *slot = i;
                }
                cx = rel_x - channel_x;
                break;
            }
        }
    }
    if (zone) {
        /* Fader band: within the track vertically (a knob's worth of slop at both
         * ends) — a click there jumps/drags the fader; elsewhere in the channel it only
         * selects, so clicking a label must not slam the level to the bottom stop. */
        int fy = cy - UI_MIXER_FADER_Y;
        if (cx >= 0 && fy >= -UI_MIXER_KNOB_H / 2 && fy <= UI_MIXER_FADER_H + UI_MIXER_KNOB_H / 2) {
            *zone = NATIVE_UI_MIXER_HIT_FADER;
        } else if (hit_slot >= 0 && hit_slot < NATIVE_UI_MIXER_MASTER &&
                   cy >= UI_MIXER_CHANNEL_H - UI_MIXER_MS_BOTTOM - UI_MIXER_MS_H - 6 &&
                   cy <= UI_MIXER_CHANNEL_H - 2) {
            /* Bottom controls band, split at the midpoints between the M / D / S plates. */
            *zone = cx <= UI_MIXER_MS_SPLIT_L   ? NATIVE_UI_MIXER_HIT_MUTE
                    : cx >= UI_MIXER_MS_SPLIT_R ? NATIVE_UI_MIXER_HIT_SOLO
                                                : NATIVE_UI_MIXER_HIT_DUCK;
        }
    }
    return true;
}

int native_ui_mixer_fader_db_at(int win_h, int y) {
    const int span = NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB;
    int track_y = ui_mixer_panel_y(win_h) + UI_MIXER_PANEL_PAD_TOP + UI_MIXER_CHANNEL_Y + UI_MIXER_FADER_Y;
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
    int track_y = ui_mixer_panel_y(win_h) + UI_MIXER_PANEL_PAD_TOP + UI_MIXER_CHANNEL_Y + UI_MIXER_FADER_Y;
    int fy = y - track_y;
    if (fy < 0) {
        fy = 0;
    }
    if (fy > UI_MIXER_FADER_H) {
        fy = UI_MIXER_FADER_H;
    }
    return 100 - (fy * 100 + UI_MIXER_FADER_H / 2) / UI_MIXER_FADER_H;
}

/* One console-style latching plate. render() drives the on/off palette; built dim-off. */
static lv_obj_t *ui_mixer_build_ms_plate(lv_obj_t *channel, const char *text, int x, lv_obj_t **label_out) {
    lv_obj_t *plate = lv_obj_create(channel);
    lv_obj_set_size(plate, UI_MIXER_MS_W, UI_MIXER_MS_H);
    lv_obj_clear_flag(plate, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(plate, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(plate, (lv_opa_t)13, 0);
    lv_obj_set_style_radius(plate, 10, 0);
    lv_obj_set_style_border_width(plate, 1, 0);
    lv_obj_set_style_border_color(plate, lv_color_white(), 0);
    lv_obj_set_style_border_opa(plate, (lv_opa_t)33, 0);
    lv_obj_set_style_pad_all(plate, 0, 0);
    lv_obj_set_pos(plate, x, UI_MIXER_CHANNEL_H - UI_MIXER_MS_BOTTOM - UI_MIXER_MS_H);

    lv_obj_t *label = lv_label_create(plate);
    lv_obj_set_style_text_font(label, &lv_font_ibm_plex_mono_semibold_18, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_text_opa(label, (lv_opa_t)191, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    *label_out = label;
    return plate;
}

/* One channel per fixed colour-key slot, followed by the wider TV master strip. */
static void ui_mixer_build_channel(NativeUiMixer *mixer, lv_obj_t *bank, int slot, uint32_t color) {
    const int channel_w = ui_mixer_channel_width(slot);
    const int pair_cx = ui_mixer_pair_cx(slot);
    lv_obj_t *channel = lv_obj_create(bank);
    mixer->channels[slot] = channel;
    lv_obj_set_size(channel, channel_w, UI_MIXER_CHANNEL_H);
    lv_obj_set_pos(channel, ui_mixer_channel_x(slot), UI_MIXER_CHANNEL_Y);
    lv_obj_clear_flag(channel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(channel,
                              lv_color_hex(slot == NATIVE_UI_MIXER_MASTER ? UI_MIXER_MASTER_COLOR
                                                                          : UI_MIXER_STRIP_COLOR), 0);
    lv_obj_set_style_bg_opa(channel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(channel, 18, 0);
    lv_obj_set_style_border_width(channel, 1, 0);
    lv_obj_set_style_border_color(channel, lv_color_white(), 0);
    lv_obj_set_style_border_opa(channel, slot == NATIVE_UI_MIXER_MASTER ? (lv_opa_t)36 : (lv_opa_t)20, 0);
    lv_obj_set_style_outline_width(channel, 0, 0);
    lv_obj_set_style_outline_color(channel, lv_color_hex(0xeef3fd), 0);
    lv_obj_set_style_outline_opa(channel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_pad(channel, 0, 0);
    lv_obj_set_style_pad_all(channel, 0, 0);

    lv_obj_t *chip = lv_obj_create(channel);
    lv_obj_set_size(chip, 20, 20);
    lv_obj_set_pos(chip, 18, 20);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(chip, 6, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_pad_all(chip, 0, 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    if (slot == NATIVE_UI_MIXER_MASTER) {
        lv_obj_set_style_bg_grad_color(chip, lv_color_hex(0x8a93a5), 0);
        lv_obj_set_style_bg_grad_dir(chip, LV_GRAD_DIR_VER, 0);
    }

    lv_obj_t *name = lv_label_create(channel);
    mixer->name_labels[slot] = name;
    lv_obj_set_style_text_font(name, &lv_font_ibm_plex_sans_semibold_24, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_text_opa(name, (lv_opa_t)217, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(name, channel_w - 66);
    lv_label_set_text(name, ui_mixer_channel_labels[slot]);
    lv_obj_set_pos(name, 48, 18);

    lv_obj_t *value = lv_label_create(channel);
    mixer->value_labels[slot] = value;
    lv_obj_set_style_text_font(value, &lv_font_ibm_plex_mono_semibold_28, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_text_opa(value, LV_OPA_COVER, 0);
    lv_label_set_text(value, slot == NATIVE_UI_MIXER_MASTER ? "--%" : "0 dB");
    lv_obj_set_pos(value, 18, 48);

    if (slot != NATIVE_UI_MIXER_MASTER) {
        /* Keep the existing queue/target telemetry, now as the strip's quiet subline. */
        lv_obj_t *latency = lv_label_create(channel);
        mixer->latency_labels[slot] = latency;
        lv_obj_set_style_text_font(latency, &lv_font_ibm_plex_mono_regular_16, 0);
        lv_obj_set_style_text_color(latency, lv_color_hex(0xe9eef8), 0);
        lv_obj_set_style_text_opa(latency, (lv_opa_t)115, 0);
        lv_obj_set_style_text_align(latency, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(latency, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(latency, channel_w - 36);
        lv_label_set_text(latency, "NO SIGNAL");
        lv_obj_set_pos(latency, 18, 78);
    }

    /* L/R rails, each carrying the original smooth anchored green-to-yellow gradient:
     * the clip window grows from the bottom while the full-scale gradient child stays
     * fixed against the dB scale, so yellow only appears at loud levels. */
    for (int side = 0; side < 2; side++) {
        int x = side == 0 ? pair_cx - UI_MIXER_METER_GAP / 2 - UI_MIXER_METER_W
                          : pair_cx + UI_MIXER_METER_GAP / 2;
        lv_obj_t *rail = lv_obj_create(channel);
        lv_obj_set_pos(rail, x, UI_MIXER_FADER_Y);
        lv_obj_set_size(rail, UI_MIXER_METER_W, UI_MIXER_FADER_H);
        lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(rail, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(rail, (lv_opa_t)15, 0);
        lv_obj_set_style_radius(rail, 8, 0);
        lv_obj_set_style_border_width(rail, 0, 0);

        lv_obj_t *clip = lv_obj_create(channel);
        mixer->meter_clips[slot][side] = clip;
        lv_obj_set_pos(clip, x, UI_MIXER_FADER_Y + UI_MIXER_FADER_H);
        lv_obj_set_size(clip, UI_MIXER_METER_W, 10);
        lv_obj_clear_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(clip, 0, 0);
        lv_obj_set_style_radius(clip, 8, 0);
        lv_obj_set_style_clip_corner(clip, true, 0);
        lv_obj_set_style_pad_all(clip, 0, 0);
        lv_obj_add_flag(clip, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *grad = lv_obj_create(clip);
        mixer->meter_grads[slot][side] = grad;
        lv_obj_set_pos(grad, 0, -UI_MIXER_FADER_H);
        lv_obj_set_size(grad, UI_MIXER_METER_W, UI_MIXER_FADER_H);
        lv_obj_clear_flag(grad, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(grad, 4, 0);
        lv_obj_set_style_clip_corner(grad, true, 0);
        lv_obj_set_style_border_width(grad, 0, 0);
        lv_obj_set_style_bg_color(grad, lv_color_hex(0xf7d038), 0);
        lv_obj_set_style_bg_grad_color(grad, lv_color_hex(0x27cf5a), 0);
        lv_obj_set_style_bg_grad_dir(grad, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(grad, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(grad, 0, 0);

        /* The peak-hold mark: detached segment floating at the held level, in the
         * meter's loud-end yellow so it reads as "this is where the bar peaked". */
        lv_obj_t *mark = lv_obj_create(channel);
        mixer->peak_marks[slot][side] = mark;
        lv_obj_set_pos(mark, x, UI_MIXER_FADER_Y);
        lv_obj_set_size(mark, UI_MIXER_METER_W, UI_MIXER_PEAK_MARK_H);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(mark, lv_color_hex(0xf7d038), 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(mark, 2, 0);
        lv_obj_set_style_border_width(mark, 0, 0);
        lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
    }

    /* The source scale is dBFS; the wider master gets the percent scale from the mock. */
    const int source_ticks[] = {0, -15, -30, -45, -60};
    /* MASTER shares the exact horizontal guide lines used by the dB channels. A
     * linear 0..100 fader reads 91/68/45/23/0 at those lines; couch-readable values
     * are rounded to the nearest five. The unlabeled +6 dB headroom maps to 100%. */
    const int master_ticks[] = {90, 70, 45, 25, 0};
    for (int tick_index = 0; tick_index < 5; tick_index++) {
        int tick_value = slot == NATIVE_UI_MIXER_MASTER ? master_ticks[tick_index] : source_ticks[tick_index];
        int y = ui_mixer_db_to_y(source_ticks[tick_index]);
        lv_obj_t *tick = lv_label_create(channel);
        lv_obj_set_style_text_font(tick, &lv_font_ibm_plex_mono_regular_16, 0);
        lv_obj_set_style_text_color(tick, lv_color_hex(0xe9eef8), 0);
        lv_obj_set_style_text_opa(tick, (lv_opa_t)102, 0);
        lv_obj_set_style_text_align(tick, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(tick, 34);
        lv_label_set_text_fmt(tick, "%d", tick_value);
        lv_obj_set_pos(tick, pair_cx - 83, y - 8);
    }

    /* White pill knob with a colour-key stripe; the MASTER uses a neutral slate stripe. */
    lv_obj_t *knob = lv_obj_create(channel);
    mixer->knobs[slot] = knob;
    lv_obj_set_size(knob, UI_MIXER_KNOB_W, UI_MIXER_KNOB_H);
    lv_obj_set_pos(knob, pair_cx - UI_MIXER_KNOB_W / 2, ui_mixer_db_to_y(0) - UI_MIXER_KNOB_H / 2);
    lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(knob, lv_color_white(), 0);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_pad_all(knob, 0, 0);
    lv_obj_set_style_shadow_width(knob, 16, 0);
    lv_obj_set_style_shadow_opa(knob, LV_OPA_50, 0);
    lv_obj_set_style_shadow_color(knob, lv_color_black(), 0);
    lv_obj_t *stripe = lv_obj_create(knob);
    mixer->knob_stripes[slot] = stripe;
    lv_obj_set_size(stripe, 22, 5);
    lv_obj_clear_flag(stripe, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(stripe, lv_color_hex(slot == NATIVE_UI_MIXER_MASTER ? 0x5b6472 : color), 0);
    lv_obj_set_style_radius(stripe, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(stripe, 0, 0);
    lv_obj_center(stripe);

    if (slot != NATIVE_UI_MIXER_MASTER) {
        mixer->mute_buttons[slot] =
            ui_mixer_build_ms_plate(channel, "M", UI_MIXER_MS_MUTE_X, &mixer->mute_labels[slot]);
        mixer->duck_buttons[slot] =
            ui_mixer_build_ms_plate(channel, "D", UI_MIXER_MS_DUCK_X, &mixer->duck_labels[slot]);
        mixer->solo_buttons[slot] =
            ui_mixer_build_ms_plate(channel, "S", UI_MIXER_MS_SOLO_X, &mixer->solo_labels[slot]);
    }
}

NativeUiMixer *native_ui_mixer_create(SDL_Renderer *renderer) {
    if (!renderer) {
        clog(cLogLevelError, "cannot create mixer UI without an SDL renderer");
        return NULL;
    }
    NativeUiMixer *mixer = (NativeUiMixer *)calloc(1, sizeof(*mixer));
    if (!mixer) {
        clog(cLogLevelError, "failed to allocate mixer UI");
        return NULL;
    }
    mixer->renderer = renderer;
    mixer->disp = lv_disp_get_default();
    mixer->screen = lv_obj_create(NULL);
    if (!mixer->disp || !mixer->screen) {
        clog(cLogLevelError, "failed to create mixer LVGL screen (display=%s screen=%s)",
             mixer->disp ? "set" : "NULL", mixer->screen ? "set" : "NULL");
        if (mixer->screen) {
            lv_obj_del(mixer->screen);
        }
        free(mixer);
        return NULL;
    }
    /* The screen itself remains a hole: only the floating console paints over video. */
    lv_obj_set_style_bg_opa(mixer->screen, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(mixer->screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(mixer->screen);
    lv_obj_set_size(panel, UI_MIXER_PANEL_W, UI_MIXER_PANEL_H);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -UI_MIXER_PANEL_BOTTOM);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_MIXER_PANEL_COLOR), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(panel, UI_MIXER_PANEL_RADIUS, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_opa(panel, (lv_opa_t)46, 0);
    lv_obj_set_style_shadow_width(panel, 48, 0);
    lv_obj_set_style_shadow_ofs_y(panel, 12, 0);
    lv_obj_set_style_shadow_opa(panel, (lv_opa_t)102, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_black(), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    lv_obj_t *bank = lv_obj_create(panel);
    lv_obj_set_size(bank, UI_MIXER_BANK_W, UI_MIXER_BANK_H);
    lv_obj_align(bank, LV_ALIGN_TOP_MID, 0, UI_MIXER_PANEL_PAD_TOP);
    lv_obj_clear_flag(bank, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bank, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bank, 0, 0);
    lv_obj_set_style_radius(bank, 0, 0);
    lv_obj_set_style_pad_all(bank, 0, 0);

    lv_obj_t *title = lv_label_create(bank);
    lv_obj_set_style_text_font(title, &lv_font_ibm_plex_sans_semibold_40, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xeef1f7), 0);
    lv_obj_set_style_text_opa(title, LV_OPA_COVER, 0);
    lv_label_set_text(title, "Audio Mixer");
    lv_obj_set_pos(title, 0, 0);

    for (int slot = 0; slot < NATIVE_UI_MIXER_CHANNELS; slot++) {
        uint32_t color = slot == NATIVE_UI_MIXER_MASTER ? 0xf2f5fb : native_ui_slot_rgb(slot);
        ui_mixer_build_channel(mixer, bank, slot, color);
    }

    lv_obj_t *hints = lv_label_create(bank);
    lv_obj_set_style_text_font(hints, &lv_font_ibm_plex_sans_regular_20, 0);
    lv_obj_set_style_text_color(hints, lv_color_hex(0xe9eef8), 0);
    lv_obj_set_style_text_opa(hints, (lv_opa_t)102, 0);
    lv_label_set_text(hints,
                      "< >  Channel     ^ v  Level     OK  Channel actions     BACK  Close     Colour key jumps to its channel");
    lv_obj_set_pos(hints, 0, 640);
    clog(cLogLevelDebug, "created mixer UI");
    return mixer;
}

void native_ui_mixer_destroy(NativeUiMixer *mixer) {
    if (!mixer) {
        return;
    }
    if (mixer->screen) {
        lv_obj_del(mixer->screen);
    }
    clog(cLogLevelDebug, "destroyed mixer UI");
    free(mixer);
}

void native_ui_mixer_set_texture(NativeUiMixer *mixer, SDL_Texture *texture) {
    if (mixer) {
        mixer->texture = texture;
        /* A resized display gets a fresh, uncleared SDL target. Re-arm the one-shot
         * transparent clear so stale pixels cannot survive around the floating form. */
        mixer->full_refresh = true;
        clog(cLogLevelDebug, "mixer render texture %s", texture ? "updated" : "cleared");
    }
}

void native_ui_mixer_set_profiles(NativeUiMixer *mixer, const NativeSessionConfig *sessions) {
    if (!mixer || !sessions) {
        return;
    }
    for (int slot = 0; slot < NATIVE_SETTINGS_MAX_SESSIONS; slot++) {
        const char *name = sessions[slot].host[0] ? sessions[slot].host
                                                  : ui_mixer_channel_labels[slot];
        if (sessions[slot].name[0] &&
            native_ui_profile_name_valid(sessions[slot].name, sizeof(sessions[slot].name))) {
            name = sessions[slot].name;
        }
        lv_label_set_text(mixer->name_labels[slot], name);
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
        mixer->queue_shown[i] = UINT32_MAX;
        mixer->target_shown[i] = UINT32_MAX;
        for (int side = 0; side < 2; side++) {
            mixer->meter_db[i][side] = NATIVE_MIXER_METER_FLOOR_DB;
            mixer->peak_db[i][side] = NATIVE_MIXER_METER_FLOOR_DB;
            mixer->peak_since[i][side] = 0;
            mixer->peak_px[i][side] = -1;
            if (mixer->peak_clip[i][side]) {
                mixer->peak_clip[i][side] = false;
                lv_obj_set_style_bg_color(mixer->peak_marks[i][side], lv_color_hex(0xf7d038), 0);
            }
        }
    }
    mixer->meter_ticks = SDL_GetTicks();
    mixer->latency_ticks = 0; /* first render publishes immediately */
    mixer->selected = -2;
    mixer->mask = 0xffffffffu;
    for (int i = 0; i < NATIVE_UI_MIXER_CHANNELS; i++) {
        mixer->ms_state[i] = -1;
    }
    mixer->full_refresh = true;
    mixer->restore_screen = lv_scr_act();
    /* The shared display background stays transparent for both HUB and mixer so the
     * active hardware video plane remains the bottom layer. */
    mixer->disp->bg_opa = LV_OPA_TRANSP;
    lv_scr_load(mixer->screen);
    clog(cLogLevelDebug, "showing mixer UI");
}

void native_ui_mixer_hide(NativeUiMixer *mixer) {
    if (!mixer || !mixer->active) {
        return;
    }
    mixer->active = false;
    mixer->disp->bg_opa = LV_OPA_TRANSP;
    if (mixer->restore_screen) {
        lv_scr_load(mixer->restore_screen);
    }
    clog(cLogLevelDebug, "hiding mixer UI");
    /* No repaint here: during streaming the app repaints the punch frame on its next
     * tick, and back on the configurator the normal tick path re-renders the form. */
}

void native_ui_mixer_render(NativeUiMixer *mixer, const int32_t (*peaks)[2], const unsigned *queue_ms,
                            const unsigned *target_ms, const int8_t *gain_db, int master_pct, int selected,
                            unsigned connected_mask, int active_slot, unsigned duck_mask,
                            unsigned mute_mask, unsigned solo_mask, uint32_t now_ticks) {
    if (!mixer || !peaks || !queue_ms || !target_ms || !gain_db || !mixer->active) {
        return;
    }
    /* Latency readouts tick at 4 Hz: the queue depth breathes with every 10ms block,
     * and a calmer number is a readable one. */
    if (SDL_TICKS_PASSED(now_ticks, mixer->latency_ticks)) {
        mixer->latency_ticks = now_ticks + 250u;
        for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
            bool connected = (connected_mask & (1u << i)) != 0;
            unsigned shown_queue = connected ? queue_ms[i] : UINT32_MAX - 1u;
            unsigned shown_target = connected ? target_ms[i] : UINT32_MAX - 1u;
            if ((shown_queue == mixer->queue_shown[i] && shown_target == mixer->target_shown[i]) ||
                !mixer->latency_labels[i]) {
                continue;
            }
            mixer->queue_shown[i] = shown_queue;
            mixer->target_shown[i] = shown_target;
            if (connected) {
                lv_label_set_text_fmt(mixer->latency_labels[i], "%u/%u ms", queue_ms[i], target_ms[i]);
            } else {
                lv_label_set_text(mixer->latency_labels[i], "NO SIGNAL");
            }
        }
    }
    /* Meter ballistics: instant attack to the audio pipeline's post-fader block peak,
     * steady dB/s release. Wrap-safe tick math; a stalled loop just decays further.
     * The peak-hold marks release on their own (slower) slope once the hold expires. */
    float fall_db = (float)(now_ticks - mixer->meter_ticks) * (NATIVE_MIXER_METER_DECAY_DB_S / 1000.0f);
    float peak_fall_db = (float)(now_ticks - mixer->meter_ticks) * (UI_MIXER_PEAK_DECAY_DB_S / 1000.0f);
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

            /* Peak-hold mark: capture the loudest INSTANT chunk peak (not the decayed
             * bar), hold it for UI_MIXER_PEAK_HOLD_MS, then let it fall. */
            float peak = mixer->peak_db[i][side];
            if (target_db >= peak) {
                peak = target_db;
                mixer->peak_since[i][side] = now_ticks;
            } else if (SDL_TICKS_PASSED(now_ticks, mixer->peak_since[i][side] + UI_MIXER_PEAK_HOLD_MS)) {
                peak -= peak_fall_db;
            }
            if (peak < NATIVE_MIXER_METER_FLOOR_DB) {
                peak = NATIVE_MIXER_METER_FLOOR_DB;
            }
            mixer->peak_db[i][side] = peak;

            /* Clip detector: the mark burns red while the HELD peak sits above the
             * threshold (strict >: peaks come in pre-saturation, so anything past
             * full scale is a real clamp, while a legal rail-touching sample reads
             * exactly 0 dB). The red rides the hold+release, staying visible ~2s. */
            bool clipped = peak > UI_MIXER_CLIP_DB;
            if (clipped != mixer->peak_clip[i][side]) {
                mixer->peak_clip[i][side] = clipped;
                lv_obj_set_style_bg_color(mixer->peak_marks[i][side],
                                          lv_color_hex(clipped ? 0xff4136 : 0xf7d038), 0);
            }

            int hold_px = 0;
            if (peak > (float)NATIVE_MIXER_FADER_MIN_DB) {
                float f = (peak - (float)NATIVE_MIXER_FADER_MIN_DB) * (float)UI_MIXER_FADER_H /
                          (float)(NATIVE_MIXER_FADER_MAX_DB - NATIVE_MIXER_FADER_MIN_DB);
                hold_px = (int)f;
                if (hold_px > UI_MIXER_FADER_H) {
                    hold_px = UI_MIXER_FADER_H;
                }
                if (hold_px < 4) {
                    hold_px = 0; /* park the mark with the bar's sub-radius slivers */
                }
            }
            if (hold_px != mixer->peak_px[i][side]) {
                mixer->peak_px[i][side] = hold_px;
                lv_obj_t *mark = mixer->peak_marks[i][side];
                if (hold_px == 0) {
                    lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(mark, LV_OBJ_FLAG_HIDDEN);
                    /* The mark's BOTTOM sits at the held level, so it floats above the
                     * bar; clamped at the rail top for full-scale peaks. */
                    int mark_y = UI_MIXER_FADER_Y + UI_MIXER_FADER_H - hold_px - UI_MIXER_PEAK_MARK_H;
                    if (mark_y < UI_MIXER_FADER_Y) {
                        mark_y = UI_MIXER_FADER_Y;
                    }
                    lv_obj_set_y(mark, mark_y);
                }
            }

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
            if (i == NATIVE_UI_MIXER_MASTER) {
                if (value < 0) {
                    lv_label_set_text(mixer->value_labels[i], "--%");
                } else {
                    int shown_pct = ui_mixer_master_pct_clamped(value);
                    lv_label_set_text_fmt(mixer->value_labels[i], "%d%%", shown_pct);
                }
            } else if (value <= NATIVE_MIXER_FADER_MIN_DB) {
                lv_label_set_text(mixer->value_labels[i], "-inf dB");
            } else if (value > 0) {
                lv_label_set_text_fmt(mixer->value_labels[i], "+%d dB", value);
            } else {
                lv_label_set_text_fmt(mixer->value_labels[i], "%d dB", value);
            }
        }
    }
    if (selected != mixer->selected || connected_mask != mixer->mask) {
        mixer->selected = selected;
        mixer->mask = connected_mask;
        for (int i = 0; i < NATIVE_UI_MIXER_CHANNELS; i++) {
            bool connected = (connected_mask & (1u << i)) != 0;
            bool is_selected = i == selected;
            bool is_master = i == NATIVE_UI_MIXER_MASTER;
            lv_obj_set_style_bg_color(mixer->channels[i],
                                      lv_color_hex(is_master ? UI_MIXER_MASTER_COLOR : UI_MIXER_STRIP_COLOR), 0);
            lv_obj_set_style_bg_opa(mixer->channels[i], LV_OPA_COVER, 0);
            /* Selection is an outline, not a thicker border: changing border width
             * changes LVGL's content box and makes every child appear to jump forward. */
            lv_obj_set_style_border_width(mixer->channels[i], 1, 0);
            lv_obj_set_style_border_color(mixer->channels[i], lv_color_white(), 0);
            lv_obj_set_style_border_opa(mixer->channels[i],
                                        is_master ? (lv_opa_t)36 : (lv_opa_t)20, 0);
            lv_obj_set_style_outline_width(mixer->channels[i], is_selected ? 3 : 0, 0);
            lv_obj_set_style_outline_opa(mixer->channels[i], is_selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_opa(mixer->channels[i], connected ? LV_OPA_COVER : (lv_opa_t)140, 0);
            /* Disconnected slots keep their fader (the level applies on connect) but
             * dim the complete strip; their meters are empty anyway. */
            lv_obj_set_style_bg_opa(mixer->knobs[i], LV_OPA_COVER, 0);
            if (mixer->knob_stripes[i]) {
                lv_obj_set_style_bg_opa(mixer->knob_stripes[i], LV_OPA_COVER, 0);
            }
        }
    }
    /* M/D/S plates (every session channel): red mute, blue duck and yellow solo.
     * The active channel's D remains an inert ghost. Change-only. */
    for (int i = 0; i < NATIVE_SETTINGS_MAX_SESSIONS; i++) {
        if (!mixer->mute_buttons[i]) {
            continue;
        }
        bool connected = (connected_mask & (1u << i)) != 0;
        bool is_active = i == active_slot;
        bool mute_on = ((mute_mask >> i) & 1u) != 0;
        bool solo_on = ((solo_mask >> i) & 1u) != 0;
        bool duck_on = !is_active && ((duck_mask >> i) & 1u) != 0;
        int ms = (connected ? 1 : 0) | (mute_on ? 2 : 0) | (solo_on ? 4 : 0) | (duck_on ? 8 : 0) |
                 (is_active ? 16 : 0);
        if (ms == mixer->ms_state[i]) {
            continue;
        }
        mixer->ms_state[i] = ms;
        if (mute_on) {
            lv_obj_set_style_bg_color(mixer->mute_buttons[i], lv_color_hex(0xe35d55), 0);
            lv_obj_set_style_bg_opa(mixer->mute_buttons[i], (lv_opa_t)51, 0);
            lv_obj_set_style_border_color(mixer->mute_buttons[i], lv_color_hex(0xe35d55), 0);
            lv_obj_set_style_border_opa(mixer->mute_buttons[i], (lv_opa_t)140, 0);
            lv_obj_set_style_text_color(mixer->mute_labels[i], lv_color_hex(0xf0958f), 0);
            lv_obj_set_style_text_opa(mixer->mute_labels[i], connected ? LV_OPA_COVER : (lv_opa_t)102, 0);
        } else {
            lv_obj_set_style_bg_color(mixer->mute_buttons[i], lv_color_white(), 0);
            lv_obj_set_style_bg_opa(mixer->mute_buttons[i], (lv_opa_t)13, 0);
            lv_obj_set_style_border_color(mixer->mute_buttons[i], lv_color_white(), 0);
            lv_obj_set_style_border_opa(mixer->mute_buttons[i], (lv_opa_t)33, 0);
            lv_obj_set_style_text_color(mixer->mute_labels[i], lv_color_hex(0xeef1f7), 0);
            lv_obj_set_style_text_opa(mixer->mute_labels[i], connected ? (lv_opa_t)191 : (lv_opa_t)51, 0);
        }
        if (is_active) {
            /* You cannot duck yourself: the active channel's D is a plateless ghost. */
            lv_obj_set_style_bg_color(mixer->duck_buttons[i], lv_color_white(), 0);
            lv_obj_set_style_bg_opa(mixer->duck_buttons[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(mixer->duck_buttons[i], lv_color_white(), 0);
            lv_obj_set_style_border_opa(mixer->duck_buttons[i], (lv_opa_t)20, 0);
            lv_obj_set_style_text_color(mixer->duck_labels[i], lv_color_white(), 0);
            lv_obj_set_style_text_opa(mixer->duck_labels[i], (lv_opa_t)51, 0);
        } else if (duck_on) {
            lv_obj_set_style_bg_color(mixer->duck_buttons[i], lv_color_hex(0x5aa0e8), 0);
            lv_obj_set_style_bg_opa(mixer->duck_buttons[i], (lv_opa_t)51, 0);
            lv_obj_set_style_border_color(mixer->duck_buttons[i], lv_color_hex(0x5aa0e8), 0);
            lv_obj_set_style_border_opa(mixer->duck_buttons[i], (lv_opa_t)140, 0);
            lv_obj_set_style_text_color(mixer->duck_labels[i], lv_color_hex(0xa9cdf5), 0);
            lv_obj_set_style_text_opa(mixer->duck_labels[i], connected ? LV_OPA_COVER : (lv_opa_t)102, 0);
        } else {
            lv_obj_set_style_bg_color(mixer->duck_buttons[i], lv_color_white(), 0);
            lv_obj_set_style_bg_opa(mixer->duck_buttons[i], (lv_opa_t)13, 0);
            lv_obj_set_style_border_color(mixer->duck_buttons[i], lv_color_white(), 0);
            lv_obj_set_style_border_opa(mixer->duck_buttons[i], (lv_opa_t)33, 0);
            lv_obj_set_style_text_color(mixer->duck_labels[i], lv_color_hex(0xeef1f7), 0);
            lv_obj_set_style_text_opa(mixer->duck_labels[i], connected ? (lv_opa_t)191 : (lv_opa_t)51, 0);
        }
        if (solo_on) {
            lv_obj_set_style_bg_color(mixer->solo_buttons[i], lv_color_hex(0xe8c15a), 0);
            lv_obj_set_style_bg_opa(mixer->solo_buttons[i], (lv_opa_t)46, 0);
            lv_obj_set_style_border_color(mixer->solo_buttons[i], lv_color_hex(0xe8c15a), 0);
            lv_obj_set_style_border_opa(mixer->solo_buttons[i], (lv_opa_t)128, 0);
            lv_obj_set_style_text_color(mixer->solo_labels[i], lv_color_hex(0xecd08a), 0);
            lv_obj_set_style_text_opa(mixer->solo_labels[i], connected ? LV_OPA_COVER : (lv_opa_t)102, 0);
        } else {
            lv_obj_set_style_bg_color(mixer->solo_buttons[i], lv_color_white(), 0);
            lv_obj_set_style_bg_opa(mixer->solo_buttons[i], (lv_opa_t)13, 0);
            lv_obj_set_style_border_color(mixer->solo_buttons[i], lv_color_white(), 0);
            lv_obj_set_style_border_opa(mixer->solo_buttons[i], (lv_opa_t)33, 0);
            lv_obj_set_style_text_color(mixer->solo_labels[i], lv_color_hex(0xeef1f7), 0);
            lv_obj_set_style_text_opa(mixer->solo_labels[i], connected ? (lv_opa_t)191 : (lv_opa_t)51, 0);
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
        } else {
            clog_limited(cLogLevelWarning, 1, 5000, "mixer UI has no render texture");
        }
        lv_obj_invalidate(mixer->screen);
    }
    lv_refr_now(mixer->disp);
}

#endif /* HELLOLG_WITH_PRECONNECT_UI */
#endif /* HELLOLG_WITH_SDL */
