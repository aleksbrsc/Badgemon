// Reusable vertical menu. Any screen opens it with a title, an optional
// subtitle, items, and callbacks — e.g. sub-menus, pickers, settings:
//
//   static const char *items[] = {"foo", "bar"};
//   ui_menu_open("title", "subtitle", items, 2, on_pick, on_back, NULL);
//
// Up/Down move, A selects, Home goes back (ignored when on_back=NULL,
// i.e. at the root). Re-open to refresh (e.g. changed subtitle).
#pragma once

#include <stdint.h>
#include "hal_buttons.h"

typedef void (*menu_select_cb)(int index, void *ctx);
typedef void (*menu_back_cb)(void *ctx);

void ui_menu_open(const char *title, const char *subtitle, const char *items[], int n_items,
                  menu_select_cb on_select, menu_back_cb on_back, void *ctx);
void ui_menu_tick(uint32_t now_ms, const btn_event_t *ev);
