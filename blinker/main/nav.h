// Screen navigation: menu <-> ping <-> settings.
// Screens go back via nav_show(SCR_MENU). Keyboard is owned by settings.
#pragma once

#include <stdint.h>
#include "hal_buttons.h"

typedef enum { SCR_MENU, SCR_PING, SCR_SETTINGS } screen_t;

void nav_show(screen_t s);
void nav_tick(uint32_t now_ms, const btn_event_t *ev);
