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
    if (attack->power <= 0)
        return 0;

    /* Deterministic Gen 3 damage. All-integer core (flooring at each
     * division as the games do), then the deterministic multipliers
     * (STAB * type effectiveness). No random 0.85-1.0 spread and no
     * crits, so both badges in a lockstep duel compute identical HP.
     *
     *   base = ((2*L/5 + 2) * Power * Atk / Def) / 50 + 2
     * Attack/Defense default to 1 if a caller left them unset, so an
     * unpopulated incoming_attack_t degrades to ~Power-scaled damage
     * instead of dividing by zero. */
    int level = attack->level > 0 ? attack->level : 1;
    int atk = attack->attack > 0 ? attack->attack : 1;
    int def = defender->defense > 0 ? defender->defense : 1;

    long base = (2L * level) / 5 + 2;
    base = base * attack->power * atk / def;
    base = base / 50 + 2;

    float mod = attack_multiplier(attack, defender);
    if (attack->stab)
        mod *= 1.5f;

    float dmg = (float)base * mod;
    if (dmg < 0.0f)
        dmg = 0.0f;
    return (int)dmg; /* truncates toward zero, matching the games' floor */
}

incoming_attack_t make_attack(const pokemon_t *attacker, int move_idx)
{
    incoming_attack_t a = {0};
    if (!attacker || move_idx < 0 || move_idx >= attacker->move_count)
        return a;
    const move_t *mv = &attacker->moves[move_idx];
    a.name = mv->name;
    a.type = mv->type;
    a.power = mv->power;
    a.level = attacker->level;
    a.attack = attacker->attack;
    a.stab = (mv->type == attacker->type1 || mv->type == attacker->type2) ? 1 : 0;
    return a;
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

/* ------------------------------------------------------------------ */
/* Leveling / experience (Fire Red / Gen 3)                           */
/* ------------------------------------------------------------------ */

static int clamp_level(int level)
{
    if (level < MIN_LEVEL)
        return MIN_LEVEL;
    if (level > MAX_LEVEL)
        return MAX_LEVEL;
    return level;
}

int exp_for_level(growth_rate_t growth, int level)
{
    long n = clamp_level(level);
    if (n <= 1)
        return 0;

    long e;
    switch (growth)
    {
    case GROWTH_FAST: /* 4n^3 / 5 */
        e = (4 * n * n * n) / 5;
        break;
    case GROWTH_SLOW: /* 5n^3 / 4 */
        e = (5 * n * n * n) / 4;
        break;
    case GROWTH_MEDIUM_SLOW: /* 6n^3/5 - 15n^2 + 100n - 140 */
        e = (6 * n * n * n) / 5 - 15 * n * n + 100 * n - 140;
        break;
    case GROWTH_ERRATIC:
        if (n <= 50)
            e = (n * n * n * (100 - n)) / 50;
        else if (n <= 68)
            e = (n * n * n * (150 - n)) / 100;
        else if (n <= 98)
            e = (n * n * n * ((1911 - 10 * n) / 3)) / 500;
        else
            e = (n * n * n * (160 - n)) / 100;
        break;
    case GROWTH_FLUCTUATING:
        if (n <= 15)
            e = (n * n * n * ((n + 1) / 3 + 24)) / 50;
        else if (n <= 36)
            e = (n * n * n * (n + 14)) / 50;
        else
            e = (n * n * n * (n / 2 + 32)) / 50;
        break;
    case GROWTH_MEDIUM_FAST: /* n^3 */
    default:
        e = n * n * n;
        break;
    }
    if (e < 0)
        e = 0;
    return (int)e;
}

int hp_for_level(int base_hp, int level)
{
    /* Gen 3 HP (IV/EV = 0): floor(2*Base*Level/100) + Level + 10. */
    level = clamp_level(level);
    return (2 * base_hp * level) / 100 + level + 10;
}

int stat_for_level(int base_stat, int level)
{
    /* Gen 3 non-HP stat (IV/EV = 0, neutral nature):
     * floor(2*Base*Level/100) + 5. */
    level = clamp_level(level);
    return (2 * base_stat * level) / 100 + 5;
}

void pokemon_recalc_stats(pokemon_t *p)
{
    if (!p)
        return;
    p->level = clamp_level(p->level);
    p->max_health = hp_for_level(p->base_hp, p->level);
    p->attack = stat_for_level(p->base_attack, p->level);
    p->defense = stat_for_level(p->base_defense, p->level);
}

void pokemon_init(pokemon_t *p, int level)
{
    if (!p)
        return;
    p->level = clamp_level(level);
    p->exp = exp_for_level(p->growth, p->level);
    pokemon_recalc_stats(p);
    p->health = p->max_health; /* start fully healed */
}

void pokemon_apply_progress(pokemon_t *p, int level, int exp, int health)
{
    if (!p)
        return;
    p->level = clamp_level(level);
    p->exp = exp >= 0 ? exp : exp_for_level(p->growth, p->level);
    int cap = exp_for_level(p->growth, MAX_LEVEL);
    if (p->exp > cap)
        p->exp = cap;
    pokemon_recalc_stats(p);
    if (health < 0)
        p->health = p->max_health;
    else {
        if (health > p->max_health)
            health = p->max_health;
        if (health < 0)
            health = 0;
        p->health = health;
    }
}

int exp_to_next_level(const pokemon_t *p)
{
    if (!p || p->level >= MAX_LEVEL)
        return 0;
    int next = exp_for_level(p->growth, p->level + 1);
    int rem = next - p->exp;
    return rem > 0 ? rem : 0;
}

int exp_yield(const pokemon_t *fainted)
{
    if (!fainted)
        return 0;
    /* Gen 3 wild formula, single participant: base_exp * level / 7. */
    int gained = fainted->base_exp * fainted->level / 7;
    return gained > 0 ? gained : 1;
}

int pokemon_gain_exp(pokemon_t *p, int amount)
{
    if (!p || amount <= 0 || p->level >= MAX_LEVEL)
        return 0;

    p->exp += amount;

    int levels = 0;
    while (p->level < MAX_LEVEL &&
           p->exp >= exp_for_level(p->growth, p->level + 1))
    {
        int old_max = p->max_health;
        p->level++;
        pokemon_recalc_stats(p);
        /* Match the games: a level-up adds its HP gain to current HP. */
        p->health += p->max_health - old_max;
        levels++;
    }

    /* Don't let exp overflow past the level 100 cap. */
    int cap = exp_for_level(p->growth, MAX_LEVEL);
    if (p->level >= MAX_LEVEL && p->exp > cap)
        p->exp = cap;

    if (p->health > p->max_health)
        p->health = p->max_health;
    return levels;
}
