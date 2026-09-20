// Ping UI: digit composer keyboard + payload display.
// Up/Down = digit, Left/Right = cursor, A = send, B = clear, Home = demo.
#pragma once

#include <stdint.h>
#include "hal_buttons.h"

void ui_ping_enter(void);
void ui_ping_tick(uint32_t now_ms, const btn_event_t *ev);
