// Debug REPL over USB-Serial-JTAG (`idf.py monitor`, type `help`).
//
// UI remote control + self-tests live here too:
//   press/start/screen/nav drive the UI from serial; `test ui` walks every
//   screen through its scenarios and reports PASS/FAIL; `test scan` hammers
//   rescan (the reported flash-and-kicked-to-menu bug) and asserts the badge
//   stays on the play screen with no reboot.
#pragma once

#include <stdbool.h>
#include "hal_buttons.h"

void debug_init(void);
// OR serial-injected presses into the live button event (main loop calls
// this every poll; `press` sets them). Consumed after one merge.
void debug_merge(btn_event_t *ev);
// True while a `test` owns nav_tick (main loop skips its own ticks).
bool debug_hijacked(void);
