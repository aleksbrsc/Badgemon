// Screen navigation: menu <-> play <-> duel. Settings is keyboard-owned.
// Screens go back via nav_show(SCR_MENU). Keyboard is owned by settings/menu.
#pragma once

#include <stdint.h>
#include "hal_buttons.h"

typedef enum { SCR_MENU, SCR_PLAY, SCR_MSG, SCR_SETTINGS, SCR_DUEL } screen_t;

void nav_show(screen_t s);
void nav_tick(uint32_t now_ms, const btn_event_t *ev);
// Debug introspection (serial REPL `screen` / `test` commands).
screen_t nav_current(void);
const char *nav_name(screen_t s);
