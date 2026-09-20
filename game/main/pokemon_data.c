#include "pokemon_data.h"
#include "esp_random.h"

static const pokemon_t SPECIES[SPECIES_COUNT] = {
    {.name = "Vinyl",
     .growth = GROWTH_MEDIUM_SLOW,
     .base_hp = 39,
     .base_attack = 52,
     .base_defense = 43,
     .base_exp = 65,
     .type1 = TYPE_FIRE,
     .type2 = TYPE_NONE,
     .move_count = 4,
     .moves = {
         {"Ember", TYPE_FIRE, 40, 25},
         {"Scratch", TYPE_NORMAL, 40, 35},
         {"Slash", TYPE_NORMAL, 70, 20},
         {"Metal Claw", TYPE_STEEL, 50, 35},
     }},
    {.name = "Patch",
     .growth = GROWTH_MEDIUM_SLOW,
     .base_hp = 45,
     .base_attack = 49,
     .base_defense = 49,
     .base_exp = 64,
     .type1 = TYPE_GRASS,
     .type2 = TYPE_POISON,
     .move_count = 4,
     .moves = {
         {"Vine Whip", TYPE_GRASS, 45, 25},
         {"Tackle", TYPE_NORMAL, 40, 35},
         {"Razor Leaf", TYPE_GRASS, 55, 25},
         {"Sludge Bomb", TYPE_POISON, 65, 20},
     }},
    {.name = "Ginny",
     .growth = GROWTH_MEDIUM_SLOW,
     .base_hp = 44,
     .base_attack = 48,
     .base_defense = 65,
     .base_exp = 63,
     .type1 = TYPE_WATER,
     .type2 = TYPE_NONE,
     .move_count = 4,
     .moves = {
         {"Water Gun", TYPE_WATER, 40, 25},
         {"Scratch", TYPE_NORMAL, 40, 35},
         {"Bite", TYPE_DARK, 60, 25},
         {"Rapid Spin", TYPE_NORMAL, 20, 40},
     }},
    {.name = "Goose",
     .growth = GROWTH_MEDIUM_SLOW,
     .base_hp = 50,
     .base_attack = 55,
     .base_defense = 45,
     .base_exp = 66,
     .type1 = TYPE_WATER,
     .type2 = TYPE_FLYING,
     .move_count = 4,
     .moves = {
         {"Water Gun", TYPE_WATER, 40, 25},
         {"Peck", TYPE_FLYING, 35, 35},
         {"Bite", TYPE_DARK, 60, 25},
         {"Rapid Spin", TYPE_NORMAL, 20, 40},
     }},
};

species_id_t species_random_starter(void) {
  return (species_id_t)(esp_random() % STARTER_COUNT);
}

const pokemon_t *species_template(species_id_t id) {
  if (id < 0 || id >= SPECIES_COUNT) return &SPECIES[0];
  return &SPECIES[id];
}

void pokemon_from_species(pokemon_t *out, species_id_t id) {
  if (!out) return;
  *out = *species_template(id);
}

bool species_id_valid(uint8_t raw) { return raw < SPECIES_COUNT; }
