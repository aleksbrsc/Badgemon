// Reusable on-screen keyboard driven by the badge buttons.
//
// Any screen can open it: ui_keyboard_start(initial, on_done, ctx).
// While active, that screen must route its ticks to ui_keyboard_tick()
// and ignore its own input. Finish with START (save) or Home (cancel).
//
// Layout: Up/Down/Left/Right move, A picks a char, B deletes,
// START confirms, Home cancels.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "hal_buttons.h"

#define KEYBOARD_TEXT_MAX 16

typedef void (*keyboard_done_cb)(const char *text, void *ctx);  // text=NULL on cancel

void ui_keyboard_start(const char *initial, keyboard_done_cb on_done, void *ctx);
bool ui_keyboard_active(void);
void ui_keyboard_tick(uint32_t now_ms, const btn_event_t *ev);
