#include "game.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "game";

#define SAVE_MAGIC 0x42444731u /* "BDG1" */
#define SAVE_VERSION 1

// Bag is stored under its own NVS key so the party blob layout/version is
// untouched (no wiping existing saves). Fresh badges start well-stocked.
#define BALLS_KEY "balls_v1"
#define GAME_START_BALLS 20
#define MAX_BALLS 99

typedef struct {
  uint8_t species;
  uint8_t level;
  uint32_t exp;
} saved_mon_t;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint8_t team_count;
  uint8_t active;
  saved_mon_t mon[MAX_TEAM];
} game_save_t;

static game_save_t s_save;
static int s_balls = -1;  // lazy-loaded; -1 = "not read yet"

static void balls_load(void);
static void balls_save(void);

static bool save_valid(const game_save_t *s) {
  if (!s || s->magic != SAVE_MAGIC || s->version != SAVE_VERSION) return false;
  if (s->team_count == 0 || s->team_count > MAX_TEAM) return false;
  if (s->active >= s->team_count) return false;
  for (int i = 0; i < s->team_count; i++) {
    if (!species_id_valid(s->mon[i].species)) return false;
    if (s->mon[i].level < MIN_LEVEL || s->mon[i].level > MAX_LEVEL) return false;
  }
  return true;
}

static bool save_write(void) {
  nvs_handle_t h;
  if (nvs_open("badge", NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "open for write failed");
    return false;
  }
  esp_err_t err = nvs_set_blob(h, "party_v1", &s_save, sizeof(s_save));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "save party (%s)", esp_err_to_name(err));
  return err == ESP_OK;
}

static bool save_read(void) {
  nvs_handle_t h;
  if (nvs_open("badge", NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = sizeof(s_save);
  esp_err_t err = nvs_get_blob(h, "party_v1", &s_save, &len);
  nvs_close(h);
  if (err != ESP_OK || len != sizeof(s_save)) return false;
  return save_valid(&s_save);
}

static void new_game(void) {
  memset(&s_save, 0, sizeof(s_save));
  s_save.magic = SAVE_MAGIC;
  s_save.version = SAVE_VERSION;
  s_save.team_count = 1;
  s_save.active = 0;
  species_id_t sp = species_random_starter();
  pokemon_t p;
  pokemon_from_species(&p, sp);
  pokemon_init(&p, GAME_START_LEVEL);
  s_save.mon[0].species = (uint8_t)sp;
  s_save.mon[0].level = (uint8_t)p.level;
  s_save.mon[0].exp = (uint32_t)p.exp;
  save_write();
  ESP_LOGI(TAG, "new game: %s L%d", p.name, p.level);
}

void game_factory_reset(void) {
  new_game();
  s_balls = GAME_START_BALLS;
  balls_save();
  ESP_LOGI(TAG, "factory reset: new party + %d pokeballs", GAME_START_BALLS);
}

void game_init(void) {
  balls_load();
  if (!save_read()) {
    ESP_LOGI(TAG, "no valid save, creating party");
    new_game();
    if (s_balls < 0) s_balls = GAME_START_BALLS;
    balls_save();
  } else {
    saved_mon_t *m = &s_save.mon[s_save.active];
    ESP_LOGI(TAG, "loaded species=%u L%d exp=%u team=%u", m->species, m->level,
             (unsigned)m->exp, s_save.team_count);
  }
}

species_id_t game_active_species(void) {
  if (!save_valid(&s_save)) return SPECIES_CHARMANDER;
  return (species_id_t)s_save.mon[s_save.active].species;
}

bool game_load_active(pokemon_t *out) {
  if (!out || !save_valid(&s_save)) return false;
  saved_mon_t *m = &s_save.mon[s_save.active];
  pokemon_from_species(out, (species_id_t)m->species);
  pokemon_apply_progress(out, m->level, (int)m->exp, -1);
  return true;
}

bool game_commit_active(const pokemon_t *p) {
  if (!p || !save_valid(&s_save)) return false;
  saved_mon_t *m = &s_save.mon[s_save.active];
  m->level = (uint8_t)p->level;
  m->exp = (uint32_t)p->exp;
  return save_write();
}

// ---- Bag (Poke Balls) ----------------------------------------------
static void balls_load(void) {
  s_balls = GAME_START_BALLS;
  nvs_handle_t h;
  if (nvs_open("badge", NVS_READONLY, &h) != ESP_OK) return;
  uint8_t v;
  if (nvs_get_u8(h, BALLS_KEY, &v) == ESP_OK) s_balls = v;
  nvs_close(h);
}

static void balls_save(void) {
  int v = s_balls;
  if (v < 0) v = 0;
  if (v > MAX_BALLS) v = MAX_BALLS;
  nvs_handle_t h;
  if (nvs_open("badge", NVS_READWRITE, &h) != ESP_OK) return;
  if (nvs_set_u8(h, BALLS_KEY, (uint8_t)v) == ESP_OK) nvs_commit(h);
  nvs_close(h);
}

int game_pokeball_count(void) {
  if (s_balls < 0) balls_load();
  return s_balls;
}

void game_add_pokeball(int n) {
  if (s_balls < 0) balls_load();
  s_balls += n;
  if (s_balls < 0) s_balls = 0;
  if (s_balls > MAX_BALLS) s_balls = MAX_BALLS;
  balls_save();
  ESP_LOGI(TAG, "pokeballs now %d", s_balls);
}

bool game_use_pokeball(void) {
  if (s_balls < 0) balls_load();
  if (s_balls <= 0) return false;
  s_balls--;
  balls_save();
  return true;
}

// ---- Party helpers --------------------------------------------------
int game_first_level(void) {
  if (!save_valid(&s_save)) return GAME_START_LEVEL;
  return s_save.mon[0].level;
}

int game_team_count(void) {
  if (!save_valid(&s_save)) return 0;
  return s_save.team_count;
}

int game_active_index(void) {
  if (!save_valid(&s_save)) return 0;
  return s_save.active;
}

bool game_set_active_index(int idx) {
  if (!save_valid(&s_save)) return false;
  if (idx < 0 || idx >= s_save.team_count) return false;
  s_save.active = (uint8_t)idx;
  return save_write();
}

bool game_swap_party(int a, int b) {
  if (!save_valid(&s_save)) return false;
  if (a < 0 || b < 0 || a >= s_save.team_count || b >= s_save.team_count)
    return false;
  if (a == b) return true;
  saved_mon_t tmp = s_save.mon[a];
  s_save.mon[a] = s_save.mon[b];
  s_save.mon[b] = tmp;
  if (s_save.active == (uint8_t)a)
    s_save.active = (uint8_t)b;
  else if (s_save.active == (uint8_t)b)
    s_save.active = (uint8_t)a;
  return save_write();
}

bool game_load_slot(int idx, pokemon_t *out) {
  if (!out || !save_valid(&s_save)) return false;
  if (idx < 0 || idx >= s_save.team_count) return false;
  saved_mon_t *m = &s_save.mon[idx];
  pokemon_from_species(out, (species_id_t)m->species);
  pokemon_apply_progress(out, m->level, (int)m->exp, -1);
  return true;
}

bool game_add_mon(species_id_t sp, int level) {
  if (!save_valid(&s_save)) return false;
  if (s_save.team_count >= MAX_TEAM) return false;
  if (!species_id_valid((uint8_t)sp)) return false;
  if (level < MIN_LEVEL) level = MIN_LEVEL;
  if (level > MAX_LEVEL) level = MAX_LEVEL;
  pokemon_t p;
  pokemon_from_species(&p, sp);
  pokemon_init(&p, level);
  int i = s_save.team_count;
  s_save.mon[i].species = (uint8_t)sp;
  s_save.mon[i].level = (uint8_t)p.level;
  s_save.mon[i].exp = (uint32_t)p.exp;
  s_save.team_count++;
  ESP_LOGI(TAG, "party += %s L%d (team=%d)", p.name, p.level,
           s_save.team_count);
  return save_write();
}

void game_debug(char *out, int cap) {
  if (!out || cap < 1) return;
  if (!save_valid(&s_save)) {
    snprintf(out, cap, "game: invalid save");
    return;
  }
  saved_mon_t *m = &s_save.mon[s_save.active];
  const pokemon_t *t = species_template((species_id_t)m->species);
  snprintf(out, cap, "game: %s L%d exp=%u team=%d balls=%d", t ? t->name : "?",
           m->level, (unsigned)m->exp, s_save.team_count, game_pokeball_count());
}
