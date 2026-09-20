#include "game.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "game";

#define SAVE_MAGIC 0x42444731u /* "BDG1" */
#define SAVE_VERSION 1

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

void game_init(void) {
  if (!save_read()) {
    ESP_LOGI(TAG, "no valid save, creating party");
    new_game();
  } else {
    saved_mon_t *m = &s_save.mon[s_save.active];
    ESP_LOGI(TAG, "loaded species=%u L%d exp=%u", m->species, m->level,
             (unsigned)m->exp);
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

void game_debug(char *out, int cap) {
  if (!out || cap < 1) return;
  if (!save_valid(&s_save)) {
    snprintf(out, cap, "game: invalid save");
    return;
  }
  saved_mon_t *m = &s_save.mon[s_save.active];
  const pokemon_t *t = species_template((species_id_t)m->species);
  snprintf(out, cap, "game: %s L%d exp=%u", t ? t->name : "?", m->level,
           (unsigned)m->exp);
}
