#include "engine.h"

/* ------------------------------------------------------------------ */
/* Fire Red type chart                                                */
/* ------------------------------------------------------------------ */

/* type_chart[attacking][defending] -> damage multiplier.
 *   0.0 = no effect, 0.5 = not very effective,
 *   1.0 = normal,    2.0 = super effective.
 * Rows/cols follow the `pokemon_type_t` order in engine.h. */
static const float type_chart[TYPE_COUNT][TYPE_COUNT] = {
    /*              NOR  FIR  WAT  ELE  GRA  ICE  FIG  POI  GRO  FLY  PSY  BUG  ROC  GHO  DRA  DAR  STE */
    /* NORMAL   */ {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.5, 0.0, 1.0, 1.0, 0.5},
    /* FIRE     */ {1.0, 0.5, 0.5, 1.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 0.5, 1.0, 0.5, 1.0, 2.0},
    /* WATER    */ {1.0, 2.0, 0.5, 1.0, 0.5, 1.0, 1.0, 1.0, 2.0, 1.0, 1.0, 1.0, 2.0, 1.0, 0.5, 1.0, 1.0},
    /* ELECTRIC */ {1.0, 1.0, 2.0, 0.5, 0.5, 1.0, 1.0, 1.0, 0.0, 2.0, 1.0, 1.0, 1.0, 1.0, 0.5, 1.0, 1.0},
    /* GRASS    */ {1.0, 0.5, 2.0, 1.0, 0.5, 1.0, 1.0, 0.5, 2.0, 0.5, 1.0, 0.5, 2.0, 1.0, 0.5, 1.0, 0.5},
    /* ICE      */ {1.0, 0.5, 0.5, 1.0, 2.0, 0.5, 1.0, 1.0, 2.0, 2.0, 1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 0.5},
    /* FIGHTING */ {2.0, 1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 0.5, 1.0, 0.5, 0.5, 0.5, 2.0, 0.0, 1.0, 2.0, 2.0},
    /* POISON   */ {1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 1.0, 0.5, 0.5, 1.0, 1.0, 1.0, 0.5, 0.5, 1.0, 1.0, 0.0},
    /* GROUND   */ {1.0, 2.0, 1.0, 2.0, 0.5, 1.0, 1.0, 2.0, 1.0, 0.0, 1.0, 0.5, 2.0, 1.0, 1.0, 1.0, 2.0},
    /* FLYING   */ {1.0, 1.0, 1.0, 0.5, 2.0, 1.0, 2.0, 1.0, 1.0, 1.0, 1.0, 2.0, 0.5, 1.0, 1.0, 1.0, 0.5},
    /* PSYCHIC  */ {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 2.0, 1.0, 1.0, 0.5, 1.0, 1.0, 1.0, 1.0, 0.0, 0.5},
    /* BUG      */ {1.0, 0.5, 1.0, 1.0, 2.0, 1.0, 0.5, 0.5, 1.0, 0.5, 2.0, 1.0, 1.0, 0.5, 1.0, 2.0, 0.5},
    /* ROCK     */ {1.0, 2.0, 1.0, 1.0, 1.0, 2.0, 0.5, 1.0, 0.5, 2.0, 1.0, 2.0, 1.0, 1.0, 1.0, 1.0, 0.5},
    /* GHOST    */ {0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 1.0, 2.0, 1.0, 0.5, 0.5},
    /* DRAGON   */ {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 0.5},
    /* DARK     */ {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.5, 1.0, 1.0, 1.0, 2.0, 1.0, 1.0, 2.0, 1.0, 0.5, 0.5},
    /* STEEL    */ {1.0, 0.5, 0.5, 0.5, 1.0, 2.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 1.0, 1.0, 1.0, 0.5},
};

/* ------------------------------------------------------------------ */
/* Damage calculation                                                 */
/* ------------------------------------------------------------------ */

/* Multiplier for one attacking type vs one defending type. Returns 1.0
 * for TYPE_NONE / out-of-range so it's a no-op for single-typed mons. */
static float type_effectiveness(pokemon_type_t attack, pokemon_type_t defend)
{
    if (attack < 0 || attack >= TYPE_COUNT)
        return 1.0f;
    if (defend < 0 || defend >= TYPE_COUNT)
        return 1.0f; /* covers TYPE_NONE */
    return type_chart[attack][defend];
}

float attack_multiplier(const incoming_attack_t *attack, const pokemon_t *defender)
{
    float m = type_effectiveness(attack->type, defender->type1);
    m *= type_effectiveness(attack->type, defender->type2);
    return m;
}

int calculate_damage(const incoming_attack_t *attack, const pokemon_t *defender)
{
    float dmg = (float)attack->power * attack_multiplier(attack, defender);
    if (dmg < 0.0f)
        dmg = 0.0f;
    return (int)dmg; /* truncates toward zero, matching the games' floor */
}

int apply_attack(player_state_t *target, const incoming_attack_t *attack)
{
    pokemon_t *active = &target->pokemon[target->activePokemon];
    int dmg = calculate_damage(attack, active);
    active->health -= dmg;
    if (active->health < 0)
        active->health = 0;
    return dmg;
}

int is_fainted(const pokemon_t *p)
{
    return p->health <= 0;
}
