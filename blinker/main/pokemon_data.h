// Kanto starter species templates (Gen 3 stats/moves). More species later.
#pragma once

#include "engine.h"
#include <stdint.h>

typedef enum {
  SPECIES_CHARMANDER = 0,
  SPECIES_BULBASAUR,
  SPECIES_SQUIRTLE,
  SPECIES_COUNT
} species_id_t;

#define STARTER_COUNT 3
#define GAME_START_LEVEL 5

species_id_t species_random_starter(void);
const pokemon_t *species_template(species_id_t id);
void pokemon_from_species(pokemon_t *out, species_id_t id);
bool species_id_valid(uint8_t raw);
