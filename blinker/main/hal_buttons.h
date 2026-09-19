// Buttons: HC165 shift register (DATA=7 LOAD=20 CLK=21) + START (GPIO9).
// Shift order: A,B,Home,Down,Left,Right,Up,Aux1 — all active-low.
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
  bool a, b, home, down, left, right, up;
} btn_event_t;

void hal_buttons_init(void);
uint8_t hal_buttons_read(void);  // raw bits, bit0=A .. bit6=Up, 1=pressed
bool hal_buttons_poll(btn_event_t *ev);  // edge detect; true if any press edge
bool hal_buttons_start(void);            // START button currently pressed
