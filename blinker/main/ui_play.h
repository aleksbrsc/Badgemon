// Play: nearby-player lobby. Browse peers (A=refresh), challenge one,
// answer incoming challenges. Home backs out (consumed while a dialog
// or wait is active — see ui_play_home).
#pragma once

#include <stdint.h>
#include "hal_buttons.h"

void ui_play_enter(void);
void ui_play_tick(uint32_t now_ms, const btn_event_t *ev);
// True if Home was consumed locally (dialog answer / wait cancel).
bool ui_play_home(const btn_event_t *ev);
