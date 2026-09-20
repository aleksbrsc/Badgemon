// Persistent party save (NVS). Single active mon for now.
#pragma once

#include "engine.h"
#include "pokemon_data.h"
#include <stddef.h>
#include <stdbool.h>

void game_init(void);
/* Boot with A held: new starter party + 20 Poke Balls (NVS). */
void game_factory_reset(void);

bool game_load_active(pokemon_t *out);
species_id_t game_active_species(void);

/* Persist level/exp from battle; stored mon is full HP out of battle. */
bool game_commit_active(const pokemon_t *p);

/* Bag: Poke Balls. Defaults to GAME_START_BALLS on a fresh badge. */
int game_pokeball_count(void);
void game_add_pokeball(int n);  /* delta; clamped to [0, 99] */
bool game_use_pokeball(void);   /* consume one; false if none left */

/* Level of the first party mon (slot 0), for wild-encounter scaling. */
int game_first_level(void);

/* Append a caught mon to the party. False if the team is full/invalid. */
bool game_add_mon(species_id_t sp, int level);

int game_team_count(void);
int game_active_index(void);
bool game_set_active_index(int idx);
bool game_swap_party(int a, int b);
bool game_load_slot(int idx, pokemon_t *out);

void game_debug(char *out, int cap);
