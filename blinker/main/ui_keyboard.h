// Reusable on-screen keyboard driven by the badge buttons.
//
// Any screen can open it: ui_keyboard_start(initial, max_len, on_done, ctx).
// While active, that screen must route its ticks to ui_keyboard_tick()
// and ignore its own input. Finish with START (save) or Home (cancel).
//
// Full-width 7-column grid at font 24. Up/Down/Left/Right move, A picks,
// B deletes. The aA key toggles one-shot caps for the next letter,
// sp inserts a space.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "hal_buttons.h"
#include "lvgl.h"

#define KEYBOARD_TEXT_MAX 48

typedef void (*keyboard_done_cb)(const char *text, void *ctx);  // text=NULL on cancel

void ui_keyboard_start(const char *initial, int max_len, keyboard_done_cb on_done, void *ctx);
// Embed into an existing screen: caller creates + styles the labels,
// keyboard only writes text/grid into them (same tick/done semantics).
void ui_keyboard_bind(lv_obj_t *text_label, lv_obj_t *grid_label, const char *initial,
                      int max_len, keyboard_done_cb on_done, void *ctx);
bool ui_keyboard_active(void);
void ui_keyboard_tick(uint32_t now_ms, const btn_event_t *ev);
void ui_keyboard_debug(char *out, int cap);  // active/text/cursor/shift snapshot
