// Duel: 1v1 Pokemon battle over broadcast, entered from the lobby once
// a challenge is accepted. Deterministic lockstep — both badges run the
// same simulation and only exchange move indices (see ui_duel.c).
#pragma once

#include <stdint.h>
#include "hal_buttons.h"

// Set the opponent before entering (called by ui_play on accept).
void ui_duel_set_opponent(const uint8_t *mac, const char *name);
void ui_duel_enter(void);
void ui_duel_tick(uint32_t now_ms, const btn_event_t *ev);
// True if Home was consumed locally (e.g. dismissing the result).
bool ui_duel_home(const btn_event_t *ev);
