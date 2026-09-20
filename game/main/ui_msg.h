// Message: compose text with the keyboard, send as payload bytes,
// display received text. A = write, Home = back.
#pragma once

#include <stdint.h>
#include "hal_buttons.h"

void ui_msg_enter(void);
void ui_msg_tick(uint32_t now_ms, const btn_event_t *ev);

// Shared formatter: "AB:CD: text" if all-printable, else "AB:CD: n n n".
void msg_format_vals(char *buf, int cap, const uint8_t *mac, const uint8_t *vals, int len);
