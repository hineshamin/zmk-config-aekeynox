/*
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/usb.h>

#include "peripheral_status.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct peripheral_status_state {
    bool connected;
};

static void clear_canvas(lv_obj_t *canvas) {
    lv_draw_rect_dsc_t background;
    init_rect_dsc(&background, LVGL_BACKGROUND);
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &background);
}

static uint8_t lock_mask(const struct status_state *state) {
    return ((state->indicators & BIT(1)) ? BIT(0) : 0) |
           ((state->indicators & BIT(0)) ? BIT(1) : 0) |
           ((state->indicators & BIT(2)) ? BIT(2) : 0);
}

static void draw_all(lv_obj_t *widget, lv_color_t top_buffer[], lv_color_t middle_buffer[],
                     lv_color_t bottom_buffer[], const struct status_state *state) {
    lv_obj_t *top = lv_obj_get_child(widget, 0);
    lv_obj_t *middle = lv_obj_get_child(widget, 1);
    lv_obj_t *bottom = lv_obj_get_child(widget, 2);
    clear_canvas(top);
    clear_canvas(middle);
    clear_canvas(bottom);

    draw_battery(top, state);
    lv_draw_label_dsc_t connection;
    init_label_dsc(&connection, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_canvas_draw_text(top, 0, 0, CANVAS_SIZE, &connection,
                        state->connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);
    lv_draw_label_dsc_t label;
    init_label_dsc(&label, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
    lv_canvas_draw_text(middle, 0, 5, CANVAS_SIZE, &label, "LOCKS");
    draw_indicator_row(middle, 28, "CNS ", lock_mask(state));

    lv_canvas_draw_text(bottom, 0, 5, CANVAS_SIZE, &label,
                        state->connected ? "SPLIT OK" : "NO LINK");
    lv_draw_label_dsc_t detail;
    init_label_dsc(&detail, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER);
    lv_canvas_draw_text(bottom, 0, 32, CANVAS_SIZE, &detail,
                        state->charging ? "USB POWER" : "BATTERY");

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

ZMK_DISPLAY_WIDGET_LISTENER(aekeynox_peripheral_battery, struct battery_status_state,
                            update_battery, get_battery)
ZMK_SUBSCRIPTION(aekeynox_peripheral_battery, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(aekeynox_peripheral_battery, zmk_usb_conn_state_changed);
#endif

static void update_connection(struct peripheral_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.connected = state.connected;
        redraw(widget);
    }
}

static struct peripheral_status_state get_connection(const zmk_event_t *eh) {
    return (struct peripheral_status_state){.connected = zmk_split_bt_peripheral_is_connected()};
}

ZMK_DISPLAY_WIDGET_LISTENER(aekeynox_peripheral_connection, struct peripheral_status_state,
                            update_connection, get_connection)
ZMK_SUBSCRIPTION(aekeynox_peripheral_connection, zmk_split_peripheral_status_changed);

static void update_indicators(zmk_hid_indicators_t indicators) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        widget->state.indicators = indicators;
        redraw(widget);
    }
}

static zmk_hid_indicators_t get_indicators(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *event = as_zmk_hid_indicators_changed(eh);
    return event != NULL ? event->indicators : 0;
}

ZMK_DISPLAY_WIDGET_LISTENER(aekeynox_peripheral_indicators, zmk_hid_indicators_t,
                            update_indicators, get_indicators)
ZMK_SUBSCRIPTION(aekeynox_peripheral_indicators, zmk_hid_indicators_changed);

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
    aekeynox_peripheral_battery_init();
    aekeynox_peripheral_connection_init();
    aekeynox_peripheral_indicators_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
