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
#define MIN_LEVEL 1
#define MAX_LEVEL 100

/* Gen 3 experience groups. Each maps to a different "total exp needed
 * to be at level L" curve (see exp_for_level in engine.c). The Kanto
 * starters (Charmander/Bulbasaur/Squirtle) are MEDIUM_SLOW. */
typedef enum GrowthRate
{
    GROWTH_ERRATIC = 0,
    GROWTH_FAST,
    GROWTH_MEDIUM_FAST,
    GROWTH_MEDIUM_SLOW,
    GROWTH_SLOW,
    GROWTH_FLUCTUATING,
} growth_rate_t;

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

    /* --- Progression (Fire Red / Gen 3 style) --------------------- */
    int level;            /* 1..MAX_LEVEL */
    int exp;              /* total accumulated experience points */
    growth_rate_t growth; /* which exp curve this species follows */

    /* Species base stats (the per-Pokemon constants from the dex).
     * Current stats below are derived from these + level. */
    int base_hp;
    int base_attack;
    int base_defense;
    int base_exp; /* exp yield when this Pokemon is defeated */

    /* --- Derived, level-dependent stats --------------------------- */
    int health;     /* current HP; clamped to [0, max_health] */
    int max_health; /* recomputed from base_hp + level on each level-up */
    int attack;     /* recomputed from base_attack + level */
    int defense;    /* recomputed from base_defense + level */

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

/* The incoming attack the calc consumes. Carries the attacker context
 * the Gen 3 damage formula needs (level + Attack stat + STAB) so the
 * calc stays a pure function of (attack, defender). */
typedef struct IncomingAttack
{
    char *name;
    pokemon_type_t type;
    int power;
    int level;  /* attacker's level */
    int attack; /* attacker's current Attack stat */
    int stab;   /* 1 if the move type matches an attacker type, else 0 */
} incoming_attack_t;

/* Combined type multiplier of `attack` against a (possibly dual-typed)
 * `defender`. 0.0 / 0.5 / 1.0 / 2.0 (or 4.0 / 0.25 for dual types). */
float attack_multiplier(const incoming_attack_t *attack, const pokemon_t *defender);

/* Damage `attack` deals to `defender` using the deterministic Gen 3
 * formula (level, Attack/Defense, power, STAB, type effectiveness; no
 * random spread or crits so both badges compute identical HP). */
int calculate_damage(const incoming_attack_t *attack, const pokemon_t *defender);

/* Build an incoming attack for `attacker`'s move at index `move_idx`,
 * filling in level/Attack/STAB from the attacker. */
incoming_attack_t make_attack(const pokemon_t *attacker, int move_idx);

/* Apply an attack to the player's ACTIVE Pokemon; returns damage dealt
 * and clamps HP at 0. */
int apply_attack(player_state_t *target, const incoming_attack_t *attack);

/* Convenience: is this Pokemon fainted? */
int is_fainted(const pokemon_t *p);

/* ---------------------------------------------------------------- */
/* Leveling / experience (Fire Red / Gen 3)                         */
/* ---------------------------------------------------------------- */

/* Total experience required to *be* at `level` for a given growth
 * curve. exp_for_level(g, 1) == 0. Clamped to [MIN_LEVEL, MAX_LEVEL]. */
int exp_for_level(growth_rate_t growth, int level);

/* Gen 3 stat formulas (no IVs/EVs/nature): HP is higher and has a
 * different constant term than the other stats. */
int hp_for_level(int base_hp, int level);
int stat_for_level(int base_stat, int level);

/* (Re)compute max_health/attack/defense from base stats + current
 * level. Does NOT touch current `health` (see pokemon_init / gain_exp). */
void pokemon_recalc_stats(pokemon_t *p);

/* Initialize a Pokemon from its species base stats at `level`: sets
 * exp to the floor of that level's curve, derives stats, and fills
 * current HP to full. Call after setting base stats/type/moves/growth. */
void pokemon_init(pokemon_t *p, int level);

/* Apply saved level/exp without resetting species data. `health < 0` full
 * heals; otherwise clamps to [0, max_health]. */
void pokemon_apply_progress(pokemon_t *p, int level, int exp, int health);

/* Experience remaining until `p` reaches its next level (0 at MAX_LEVEL). */
int exp_to_next_level(const pokemon_t *p);

/* Experience awarded for defeating `fainted` (Gen 3: base_exp * L / 7). */
int exp_yield(const pokemon_t *fainted);

/* Grant `amount` exp to `p`, applying any level-ups (recomputing stats
 * and adding each level's HP gain to current HP, like the games).
 * Caps at MAX_LEVEL. Returns the number of levels gained. */
int pokemon_gain_exp(pokemon_t *p, int amount);
