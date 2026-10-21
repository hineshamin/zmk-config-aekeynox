/*
 * Copyright (c) 2026 Aekeynox contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/event_manager.h>

struct aekeynox_caps_word_state_changed {
    bool active;
};

ZMK_EVENT_DECLARE(aekeynox_caps_word_state_changed);
