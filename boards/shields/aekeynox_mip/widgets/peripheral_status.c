/*
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/usb.h>

#include "peripheral_status.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

LV_IMG_DECLARE(mountain);

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct peripheral_status_state {
    bool connected;
};

static void draw_top(lv_obj_t *widget, lv_color_t buffer[], const struct status_state *state) {
    lv_obj_t *top = lv_obj_get_child(widget, 0);
    lv_draw_rect_dsc_t background;
    init_rect_dsc(&background, LVGL_BACKGROUND);
    lv_canvas_draw_rect(top, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &background);

    draw_battery(top, state);
    lv_draw_label_dsc_t connection;
    init_label_dsc(&connection, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_canvas_draw_text(top, 0, 0, CANVAS_SIZE, &connection,
                        state->connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);

    rotate_canvas(top, buffer);
}

static void redraw(struct zmk_widget_status *widget) {
    draw_top(widget->obj, widget->cbuf, &widget->state);
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

#ifdef CONFIG_SHARP_MIP_ROTATE_180
static const int art_pos = 20;
static const int top_pos = 0;
#else
static const int art_pos = 0;
static const int top_pos = 92;
#endif

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, top_pos, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *art = lv_img_create(widget->obj);
    lv_img_set_src(art, &mountain);
    lv_obj_align(art, LV_ALIGN_TOP_LEFT, art_pos, 0);

    sys_slist_append(&widgets, &widget->node);
    aekeynox_peripheral_battery_init();
    aekeynox_peripheral_connection_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
