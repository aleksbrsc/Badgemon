// Battle engine: Pokemon/move types + Fire Red (Gen 3) damage math.
//
// Convention note (for the Go devs): this header declares the *types*
// and *function prototypes*. The actual code lives in engine.c. Other
// .c files `#include "engine.h"` to use it without seeing the guts.
#pragma once

#include <stddef.h>

/* Gen 3 (Fire Red / Leaf Green) has 17 types. No Fairy type yet.
 * The order here is the index used into the type chart in engine.c, so
 * DO NOT reorder without regenerating that table. */
typedef enum PokemonType
{
    TYPE_NORMAL = 0,
    TYPE_FIRE,
    TYPE_WATER,
    TYPE_ELECTRIC,
    TYPE_GRASS,
    TYPE_ICE,
    TYPE_FIGHTING,
    TYPE_POISON,
    TYPE_GROUND,
    TYPE_FLYING,
    TYPE_PSYCHIC,
    TYPE_BUG,
    TYPE_ROCK,
    TYPE_GHOST,
    TYPE_DRAGON,
    TYPE_DARK,
    TYPE_STEEL,
    TYPE_COUNT,    /* number of real types; handy for bounds checks */
    TYPE_NONE = -1 /* sentinel for "this Pokemon only has one type" */
} pokemon_type_t;

/* Limits. Fixed-size arrays (not C flexible array members) so these
 * structs can safely live inside other arrays. */
#define MAX_MOVES 4
#define MAX_TEAM 6

typedef struct Move
{
    char *name;
    pokemon_type_t type;
    int power; /* base power of the move, e.g. Tackle = 40 */
    int pp;
} move_t;

typedef struct Pokemon
{
    char *name;
    int health;     /* current HP; grows with the level-up system */
    int max_health; /* so we can clamp healing / draw HP bars */
    /* Dual typing: one or two types. Set type2 = TYPE_NONE for a
     * single-typed Pokemon. Effectiveness multiplies across both. */
    pokemon_type_t type1;
    pokemon_type_t type2;
    move_t moves[MAX_MOVES];
    int move_count;
} pokemon_t;

typedef struct PlayerState
{
    char *name;
    int activePokemon; /* index into pokemon[] */
    pokemon_t pokemon[MAX_TEAM];
    int team_count;
} player_state_t;

/* The incoming attack the calc consumes: which move, its type, power. */
typedef struct IncomingAttack
{
    char *name;
    pokemon_type_t type;
    int power;
} incoming_attack_t;

/* Combined type multiplier of `attack` against a (possibly dual-typed)
 * `defender`. 0.0 / 0.5 / 1.0 / 2.0 (or 4.0 / 0.25 for dual types). */
float attack_multiplier(const incoming_attack_t *attack, const pokemon_t *defender);

/* Damage `attack` deals to `defender`: power * type_effectiveness. */
int calculate_damage(const incoming_attack_t *attack, const pokemon_t *defender);

/* Apply an attack to the player's ACTIVE Pokemon; returns damage dealt
 * and clamps HP at 0. */
int apply_attack(player_state_t *target, const incoming_attack_t *attack);

/* Convenience: is this Pokemon fainted? */
int is_fainted(const pokemon_t *p);
