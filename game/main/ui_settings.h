// Settings: view name, edit via keyboard (persisted), clear. B = back.
#pragma once

#include <stdint.h>
#include "hal_buttons.h"

void ui_settings_enter(void);
void ui_settings_tick(uint32_t now_ms, const btn_event_t *ev);
