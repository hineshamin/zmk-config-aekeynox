/*
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <dt-bindings/zmk/modifiers.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aekeynox/events/caps_word_state_changed.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/hid.h>
#include <zmk/hid_indicators.h>
#include <zmk/keymap.h>
#include <zmk/rgb_underglow.h>
#include <zmk/usb.h>

#include "status.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

struct modifier_status_state {
    uint8_t modifiers;
};

struct rgb_status_state {
    bool on;
    struct zmk_led_hsb color;
    uint8_t effect;
};

static const char *const symbols_left[3][5] = {
    {"^", "<", ">", "$", "%"},
    {"{", "(", ")", "}", "="},
    {"~", "[", "]", "_", "#"},
};

static const char *const symbols_right[3][5] = {
    {"@", "&", "*", "'", "`"},
    {"\\", "+", "-", "/", "\""},
    {"|", "!", ";", ":", "?"},
};

static const char *const navnum_left[3][5] = {
    {"ESC", "HOM", "UP", "END", "PGU"},
    {"ALL", "LFT", "DN", "RGT", "PGD"},
    {"UND", "CUT", "CPY", "PST", "RDO"},
};

static const char *const navnum_right[3][5] = {
    {"LCK", "7", "8", "9", "/"},
    {"-", "4", "5", "6", "0"},
    {",", "1", "2", "3", "."},
};

static const char *const fnmedia_right[3][5] = {
    {"--", "NXT", "V+", "B+", "SCR"},
    {"--", "PLY", "MUT", "LCK", "PRT"},
    {"--", "PRV", "V-", "B-", "INS"},
};

static const char *const device_right[3][6] = {
    {"B1", "B2", "B3", "B4", "B5", "CLR"},
    {"H-", "H+", "S-", "S+", "ON", "ZMK"},
    {"E-", "E+", "B-", "B+", "--", "--"},
};

static bool layer_is(const struct status_state *state, const char *name) {
    return state->layer_label != NULL && strcmp(state->layer_label, name) == 0;
}

static void clear_canvas(lv_obj_t *canvas) {
    lv_draw_rect_dsc_t background;
    init_rect_dsc(&background, LVGL_BACKGROUND);
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &background);
}

static void draw_grid(lv_obj_t *canvas, const char *const *labels, int rows, int cols) {
    lv_draw_label_dsc_t label;
    init_label_dsc(&label, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER);

    int cell_width = CANVAS_SIZE / cols;
    int row_height = 18;
    int y = 7;
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            lv_canvas_draw_text(canvas, col * cell_width, y + row * row_height, cell_width, &label,
                                labels[row * cols + col]);
        }
    }
}

static void draw_layer_name(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t label;
    init_label_dsc(&label, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
    char fallback[12];
    const char *name = state->layer_label;
    if (name == NULL || name[0] == '\0') {
        snprintf(fallback, sizeof(fallback), "LAYER %u", state->layer_index);
        name = fallback;
    }
    lv_canvas_draw_text(canvas, 0, 5, CANVAS_SIZE, &label, name);
}

static void draw_output(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t label;
    init_label_dsc(&label, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    const char *icon = "";
    if (state->selected_endpoint.transport == ZMK_TRANSPORT_USB) {
        icon = LV_SYMBOL_USB;
    } else if (state->selected_endpoint.transport == ZMK_TRANSPORT_BLE) {
        icon = state->active_profile_bonded
                   ? (state->active_profile_connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE)
                   : LV_SYMBOL_SETTINGS;
    }
    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label, icon);
}

static uint8_t lock_mask(const struct status_state *state) {
    return (state->caps_word ? BIT(0) : 0) |
           ((state->indicators & BIT(1)) ? BIT(1) : 0) |
           ((state->indicators & BIT(0)) ? BIT(2) : 0) |
           ((state->indicators & BIT(2)) ? BIT(3) : 0);
}

static uint8_t modifier_mask(const struct status_state *state) {
    return ((state->modifiers & (MOD_LGUI | MOD_RGUI)) ? BIT(0) : 0) |
           ((state->modifiers & (MOD_LALT | MOD_RALT)) ? BIT(1) : 0) |
           ((state->modifiers & (MOD_LCTL | MOD_RCTL)) ? BIT(2) : 0) |
           ((state->modifiers & (MOD_LSFT | MOD_RSFT)) ? BIT(3) : 0);
}

static void draw_rgb(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_label_dsc_t title;
    lv_draw_label_dsc_t values;
    init_label_dsc(&title, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
    init_label_dsc(&values, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER);
    char line[24];
    lv_canvas_draw_text(canvas, 0, 2, CANVAS_SIZE, &title, state->rgb_on ? "RGB ON" : "RGB OFF");
    snprintf(line, sizeof(line), "E%u H%u", state->rgb_effect, state->rgb_hue);
    lv_canvas_draw_text(canvas, 0, 27, CANVAS_SIZE, &values, line);
    snprintf(line, sizeof(line), "S%u B%u", state->rgb_saturation, state->rgb_brightness);
    lv_canvas_draw_text(canvas, 0, 43, CANVAS_SIZE, &values, line);
}

static void draw_profiles(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_arc_dsc_t outline;
    lv_draw_arc_dsc_t selected;
    lv_draw_label_dsc_t foreground;
    lv_draw_label_dsc_t background;
    init_arc_dsc(&outline, LVGL_FOREGROUND, 2);
    init_arc_dsc(&selected, LVGL_FOREGROUND, 9);
    init_label_dsc(&foreground, LVGL_FOREGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);
    init_label_dsc(&background, LVGL_BACKGROUND, &lv_font_montserrat_18, LV_TEXT_ALIGN_CENTER);
    const int positions[5][2] = {{13, 13}, {55, 13}, {34, 34}, {13, 55}, {55, 55}};

    for (int i = 0; i < 5; i++) {
        bool active = i == state->active_profile_index;
        lv_canvas_draw_arc(canvas, positions[i][0], positions[i][1], 13, 0, 360, &outline);
        if (active) {
            lv_canvas_draw_arc(canvas, positions[i][0], positions[i][1], 9, 0, 359, &selected);
        }
        char number[2] = {(char)('1' + i), '\0'};
        lv_canvas_draw_text(canvas, positions[i][0] - 8, positions[i][1] - 10, 16,
                            active ? &background : &foreground, number);
    }
}

static void draw_all(lv_obj_t *widget, lv_color_t top_buffer[], lv_color_t middle_buffer[],
                     lv_color_t bottom_buffer[], const struct status_state *state) {
    lv_obj_t *top = lv_obj_get_child(widget, 0);
    lv_obj_t *middle = lv_obj_get_child(widget, 1);
    lv_obj_t *bottom = lv_obj_get_child(widget, 2);
    clear_canvas(top);
    clear_canvas(middle);
    clear_canvas(bottom);

    if (layer_is(state, "Symbols")) {
        draw_grid(top, &symbols_right[0][0], 3, 5);
        draw_grid(bottom, &symbols_left[0][0], 3, 5);
        draw_layer_name(middle, state);
        draw_battery(middle, state);
    } else if (layer_is(state, "NavNum")) {
        draw_grid(top, &navnum_right[0][0], 3, 5);
        draw_grid(bottom, &navnum_left[0][0], 3, 5);
        draw_layer_name(middle, state);
        draw_battery(middle, state);
    } else if (layer_is(state, "FnMedia")) {
        draw_grid(top, &fnmedia_right[0][0], 3, 5);
        draw_layer_name(middle, state);
        draw_battery(middle, state);
        draw_layer_name(bottom, state);
    } else if (layer_is(state, "Device")) {
        draw_grid(top, &device_right[0][0], 3, 6);
        draw_rgb(middle, state);
        draw_layer_name(bottom, state);
    } else {
        draw_battery(top, state);
        draw_output(top, state);
        draw_indicator_row(top, 28, "WCNS", lock_mask(state));
        draw_indicator_row(top, 47, "GACS", modifier_mask(state));
        draw_profiles(middle, state);
        draw_layer_name(bottom, state);
    }

    rotate_canvas(top, top_buffer);
    rotate_canvas(middle, middle_buffer);
    rotate_canvas(bottom, bottom_buffer);
}

static void redraw(struct zmk_widget_status *widget) {
    draw_all(widget->obj, widget->cbuf, widget->cbuf2, widget->cbuf3, &widget->state);
}

static void update_battery(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.battery = state.level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        widget->state.charging = state.usb_present;
#endif
        redraw(widget);
    }
}

static struct battery_status_state get_battery(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *event = as_zmk_battery_state_changed(eh);
    return (struct battery_status_state){
        .level = event != NULL ? event->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(aekeynox_battery, struct battery_status_state, update_battery,
                            get_battery)
ZMK_SUBSCRIPTION(aekeynox_battery, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(aekeynox_battery, zmk_usb_conn_state_changed);
#endif

static void update_output(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.selected_endpoint = state.selected_endpoint;
        widget->state.active_profile_index = state.active_profile_index;
        widget->state.active_profile_connected = state.active_profile_connected;
        widget->state.active_profile_bonded = state.active_profile_bonded;
        redraw(widget);
    }
}

static struct output_status_state get_output(const zmk_event_t *eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(aekeynox_output, struct output_status_state, update_output, get_output)
ZMK_SUBSCRIPTION(aekeynox_output, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(aekeynox_output, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(aekeynox_output, zmk_ble_active_profile_changed);
#endif

static void update_layer(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.layer_index = state.index;
        widget->state.layer_label = state.label;
        redraw(widget);
    }
}

static struct layer_status_state get_layer(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index,
        .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(aekeynox_layer, struct layer_status_state, update_layer, get_layer)
ZMK_SUBSCRIPTION(aekeynox_layer, zmk_layer_state_changed);

static void update_modifiers(struct modifier_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.modifiers = state.modifiers;
        redraw(widget);
    }
}

static struct modifier_status_state get_modifiers(const zmk_event_t *eh) {
    return (struct modifier_status_state){.modifiers = zmk_hid_get_explicit_mods()};
}

ZMK_DISPLAY_WIDGET_LISTENER(aekeynox_modifiers, struct modifier_status_state, update_modifiers,
                            get_modifiers)
ZMK_SUBSCRIPTION(aekeynox_modifiers, zmk_modifiers_state_changed);

static void update_indicators(zmk_hid_indicators_t indicators) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.indicators = indicators;
        redraw(widget);
    }
}

static zmk_hid_indicators_t get_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *event = as_zmk_hid_indicators_changed(eh);
    return event != NULL ? event->indicators : zmk_hid_indicators_get_current_profile();
}

ZMK_DISPLAY_WIDGET_LISTENER(aekeynox_indicators, zmk_hid_indicators_t, update_indicators,
                            get_indicators)
ZMK_SUBSCRIPTION(aekeynox_indicators, zmk_hid_indicators_changed);

static void update_caps_word(bool active) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.caps_word = active;
        redraw(widget);
    }
}

static bool get_caps_word(const zmk_event_t *eh) {
    const struct aekeynox_caps_word_state_changed *event =
        as_aekeynox_caps_word_state_changed(eh);
    return event != NULL && event->active;
}

ZMK_DISPLAY_WIDGET_LISTENER(aekeynox_caps_word, bool, update_caps_word, get_caps_word)
ZMK_SUBSCRIPTION(aekeynox_caps_word, aekeynox_caps_word_state_changed);

static void update_rgb(struct rgb_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.rgb_on = state.on;
        widget->state.rgb_hue = state.color.h;
        widget->state.rgb_saturation = state.color.s;
        widget->state.rgb_brightness = state.color.b;
        widget->state.rgb_effect = state.effect;
        redraw(widget);
    }
}

static struct rgb_status_state get_rgb(const zmk_event_t *eh) {
    bool on = false;
    zmk_rgb_underglow_get_state(&on);
    return (struct rgb_status_state){
        .on = on,
        .color = zmk_rgb_underglow_calc_brt(0),
        .effect = zmk_rgb_underglow_calc_effect(0),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(aekeynox_rgb, struct rgb_status_state, update_rgb, get_rgb)
ZMK_SUBSCRIPTION(aekeynox_rgb, zmk_position_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(aekeynox_rgb, zmk_usb_conn_state_changed);
#endif

#ifdef CONFIG_SHARP_MIP_ROTATE_180
static const int top_pos = 0;
static const int middle_pos = 68;
static const int bottom_pos = 136;
#else
static const int top_pos = 92;
static const int middle_pos = 24;
static const int bottom_pos = -44;
#endif

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, top_pos, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, middle_pos, 0);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, bottom_pos, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);
    aekeynox_battery_init();
    aekeynox_output_init();
    aekeynox_layer_init();
    aekeynox_modifiers_init();
    aekeynox_indicators_init();
    aekeynox_caps_word_init();
    aekeynox_rgb_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
