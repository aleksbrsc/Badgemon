// Persistent party save (NVS). Single active mon for now.
#pragma once

#include "engine.h"
#include "pokemon_data.h"
#include <stddef.h>
#include <stdbool.h>

void game_init(void);

bool game_load_active(pokemon_t *out);
species_id_t game_active_species(void);

/* Persist level/exp from battle; stored mon is full HP out of battle. */
bool game_commit_active(const pokemon_t *p);

void game_debug(char *out, int cap);
