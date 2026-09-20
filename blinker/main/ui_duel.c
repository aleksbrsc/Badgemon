// Duel: 1v1 battle entered from the lobby. See ui_duel.h for the model.
//
// Sync model — deterministic lockstep:
//   * Roles are fixed by MAC: the lower MAC is "player 0". Both badges
//     agree, so both know who has which Pokemon and who strikes first.
//   * Each turn a badge broadcasts only its chosen move index (+ turn
//     number). Once a badge holds BOTH move indices for the current
//     turn, it resolves the turn locally. The damage formula is pure
//     (power * type effectiveness, no RNG), so both badges compute the
//     exact same HP — no HP sync, no server/authority needed.
//   * Broadcast can drop packets, so a badge resends its pending move
//     periodically, and rebroadcasts the previous turn's move if it
//     sees the opponent is a turn behind.
#include "ui_duel.h"
#include "engine.h"
#include "net.h"
#include "lobby.h"  // LOBBY_NAME_MAX
#include "payload.h"
#include "nav.h"
#include "fault.h"
#include "hal_led.h"
#include "hal_display.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "duel";

#define RESEND_MS 400  // rebroadcast pending move while waiting

typedef enum { DS_SELECT, DS_WAIT, DS_OVER } duel_state_t;

// ---- Tiny hardcoded dex (spike). p0 = DEX[0], p1 = DEX[1]. ----------
// Fire vs Grass/Poison gives clear type-chart behaviour on screen:
// Charmander's Ember is 2x on Bulbasaur; Bulbasaur's Grass moves are
// 0.5x back into Charmander.
static const pokemon_t DEX[2] = {
    {.name = "Charmander", .health = 150, .max_health = 150,
     .type1 = TYPE_FIRE, .type2 = TYPE_NONE, .move_count = 4,
     .moves = {
         {"Ember", TYPE_FIRE, 40, 25},
         {"Scratch", TYPE_NORMAL, 40, 35},
         {"Slash", TYPE_NORMAL, 70, 20},
         {"Metal Claw", TYPE_STEEL, 50, 35},
     }},
    {.name = "Bulbasaur", .health = 150, .max_health = 150,
     .type1 = TYPE_GRASS, .type2 = TYPE_POISON, .move_count = 4,
     .moves = {
         {"Vine Whip", TYPE_GRASS, 45, 25},
         {"Tackle", TYPE_NORMAL, 40, 35},
         {"Razor Leaf", TYPE_GRASS, 55, 25},
         {"Sludge Bomb", TYPE_POISON, 65, 20},
     }},
};

// ---- Battle state ---------------------------------------------------
static uint8_t opp_mac[6];
static char opp_name[LOBBY_NAME_MAX + 1];

static pokemon_t mons[2];  // mons[0] = player 0, mons[1] = player 1
static bool me_is_p0;
static int me_idx, opp_idx;

static duel_state_t st;
static uint8_t turn;      // current turn number (wraps at 256; fine)
static int my_move;       // move index I committed this turn, -1 = none
static int opp_move;      // opponent's move for this turn, -1 = none
static int prev_turn;     // last resolved turn (-1 until first resolve)
static int prev_move;     // my move on prev_turn (for catch-up resends)
static int cursor;        // move menu selection
static uint32_t last_send;

// ---- LVGL objects ---------------------------------------------------
static lv_obj_t *title_label;
static lv_obj_t *hp_label;
static lv_obj_t *move_label;
static lv_obj_t *status_label;

static bool for_me(const uint8_t *t) { return !memcmp(t, net_mac(), 6); }

// ---- Rendering ------------------------------------------------------
static void redraw_hp(void) {
  if (!hp_label) return;  // labels not built yet (enter failed to lock)
  if (!lvgl_port_lock(0)) return;
  char buf[128];
  pokemon_t *mp = &mons[me_idx], *op = &mons[opp_idx];
  snprintf(buf, sizeof(buf), "%s  %d/%d\nYou: %s  %d/%d",
           op->name, op->health, op->max_health,
           mp->name, mp->health, mp->max_health);
  lv_label_set_text(hp_label, buf);
  lvgl_port_unlock();
}

static void redraw_moves(void) {
  if (!move_label) return;
  if (!lvgl_port_lock(0)) return;
  char buf[192];
  if (st == DS_SELECT) {
    int n = 0;
    pokemon_t *mp = &mons[me_idx];
    for (int i = 0; i < mp->move_count && n < (int)sizeof(buf) - 24; i++)
      n += snprintf(buf + n, sizeof(buf) - n, "%s %s\n",
                    cursor == i ? ">" : " ", mp->moves[i].name);
    lv_label_set_text(move_label, buf);
  } else if (st == DS_WAIT) {
    snprintf(buf, sizeof(buf), "waiting for\n%s...", opp_name);
    lv_label_set_text(move_label, buf);
  } else {
    lv_label_set_text(move_label, "");
  }
  lvgl_port_unlock();
}

static void status_show(const char *s) {
  if (!status_label) return;
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(status_label, s);
  lvgl_port_unlock();
}

// ---- Networking -----------------------------------------------------
static void send_move(uint8_t t, uint8_t mv) {
  uint8_t buf[8];
  memcpy(buf, opp_mac, 6);
  buf[6] = t;
  buf[7] = mv;
  net_send(PKT_DUEL, buf, sizeof(buf), NULL);
}

static void drain_net(void) {
  ping_msg_t m;
  while (net_recv(&m)) {
    if (m.type != PKT_DUEL || m.len < 8) continue;  // ignore lobby chatter
    if (!for_me(m.vals)) continue;                  // not addressed to me
    if (memcmp(m.mac, opp_mac, 6)) continue;        // not my opponent
    uint8_t t = m.vals[6], mv = m.vals[7];
    if (mv >= mons[opp_idx].move_count) {
      ESP_LOGW(TAG, "ignoring move idx %u from opponent (count=%d)", mv,
               mons[opp_idx].move_count);
      continue;  // don't trust the wire; drop instead of crashing
    }
    if (t == turn) {
      opp_move = mv;  // move for the turn we're on
    } else if ((int)t == prev_turn && prev_move >= 0) {
      // Opponent is a turn behind (missed my move). Rebroadcast it.
      send_move((uint8_t)prev_turn, (uint8_t)prev_move);
    }
  }
}

// ---- Turn resolution ------------------------------------------------
static const char *eff_word(float m) {
  if (m <= 0.0f) return "no effect";
  if (m < 1.0f) return "resisted";
  if (m > 1.0f) return "super!";
  return "hit";
}

// Deal damage to `def`; returns damage. (Local variant of apply_attack
// that works on a bare pokemon_t rather than a player_state_t.)
static int hit(pokemon_t *def, const incoming_attack_t *atk) {
  int d = calculate_damage(atk, def);
  def->health -= d;
  if (def->health < 0) def->health = 0;
  return d;
}

static incoming_attack_t attack_of(int player, int move_idx) {
  CHECK(player == 0 || player == 1, "bad player %d", player);
  CHECK(move_idx >= 0 && move_idx < mons[player].move_count,
        "move_idx %d out of range for %s (count=%d)", move_idx,
        mons[player].name, mons[player].move_count);
  move_t *mv = &mons[player].moves[move_idx];
  incoming_attack_t a = {mv->name, mv->type, mv->power};
  return a;
}

static void resolve_turn(void) {
  // Map my/opponent moves onto the fixed p0/p1 simulation so both
  // badges resolve identically regardless of perspective.
  int mv0 = me_is_p0 ? my_move : opp_move;
  int mv1 = me_is_p0 ? opp_move : my_move;
  incoming_attack_t a0 = attack_of(0, mv0);
  incoming_attack_t a1 = attack_of(1, mv1);

  // Player 0 strikes first (deterministic tiebreak by MAC order).
  char log[160];
  int n = 0;
  float e0 = attack_multiplier(&a0, &mons[1]);
  hit(&mons[1], &a0);
  n += snprintf(log + n, sizeof(log) - n, "%s: %s (%s)",
                mons[0].name, a0.name, eff_word(e0));
  if (!is_fainted(&mons[1])) {
    float e1 = attack_multiplier(&a1, &mons[0]);
    hit(&mons[0], &a1);
    n += snprintf(log + n, sizeof(log) - n, "\n%s: %s (%s)",
                  mons[1].name, a1.name, eff_word(e1));
  } else {
    n += snprintf(log + n, sizeof(log) - n, "\n%s fainted!", mons[1].name);
  }

  // Advance bookkeeping (remember this move for catch-up resends).
  prev_turn = turn;
  prev_move = my_move;
  turn++;
  my_move = -1;
  opp_move = -1;

  redraw_hp();

  if (is_fainted(&mons[me_idx]) || is_fainted(&mons[opp_idx])) {
    st = DS_OVER;
    bool won = is_fainted(&mons[opp_idx]);
    char over[192];
    snprintf(over, sizeof(over), "%s\n\n%s  (A/Home: lobby)",
             log, won ? "YOU WIN!" : "you lose...");
    status_show(over);
    redraw_moves();
    hal_led_set_all(won ? 0 : 40, won ? 40 : 0, 0);
    ESP_LOGI(TAG, "duel over: %s", won ? "win" : "lose");
    return;
  }

  st = DS_SELECT;
  cursor = 0;
  status_show(log);
  redraw_moves();
  hal_led_set_all(0, 0, 12);
}

// ---- Screen lifecycle ----------------------------------------------
void ui_duel_set_opponent(const uint8_t *mac, const char *name) {
  memcpy(opp_mac, mac, 6);
  strncpy(opp_name, name, sizeof(opp_name) - 1);
  opp_name[sizeof(opp_name) - 1] = '\0';
}

void ui_duel_enter(void) {
  // Deterministic roles: lower MAC is player 0.
  me_is_p0 = memcmp(net_mac(), opp_mac, 6) < 0;
  mons[0] = DEX[0];
  mons[1] = DEX[1];
  me_idx = me_is_p0 ? 0 : 1;
  opp_idx = 1 - me_idx;

  // Dex sanity: move_count drives menu modulo + array indexing.
  for (int i = 0; i < 2; i++)
    CHECK(mons[i].move_count > 0 && mons[i].move_count <= MAX_MOVES,
          "%s has bad move_count %d", mons[i].name, mons[i].move_count);

  turn = 0;
  my_move = -1;
  opp_move = -1;
  prev_turn = -1;
  prev_move = -1;
  cursor = 0;
  last_send = 0;
  st = DS_SELECT;

  ESP_LOGI(TAG, "enter vs '%s' as p%d (%s)", opp_name, me_is_p0 ? 0 : 1,
           mons[me_idx].name);

  // Drop any stale handles up front: if we fail to build the screen the
  // redraw_* guards must see NULL, not pointers to freed LVGL objects.
  title_label = hp_label = move_label = status_label = NULL;

  if (!lvgl_port_lock(0)) {
    ESP_LOGE(TAG, "could not lock LVGL on enter; screen not built");
    return;
  }
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();

  title_label = lv_label_create(scr);
  char t[48];
  snprintf(t, sizeof(t), "duel: %s", opp_name);
  lv_label_set_text(title_label, t);
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(title_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 6);

  hp_label = lv_label_create(scr);
  lv_obj_set_style_text_font(hp_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(hp_label, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
  lv_obj_align(hp_label, LV_ALIGN_TOP_LEFT, 10, 44);

  move_label = lv_label_create(scr);
  lv_obj_set_style_text_font(move_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(move_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(move_label, LV_ALIGN_LEFT_MID, 12, 24);

  status_label = lv_label_create(scr);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFE0A0), LV_PART_MAIN);
  lv_obj_set_width(status_label, 300);
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
  lv_obj_align(status_label, LV_ALIGN_BOTTOM_LEFT, 8, -8);
  lvgl_port_unlock();

  redraw_hp();
  redraw_moves();
  status_show("pick a move!");
  hal_led_set_all(0, 0, 12);
}

void ui_duel_tick(uint32_t now, const btn_event_t *ev) {
  drain_net();

  // Resolve as soon as both moves are in (from net or from my pick).
  if (st != DS_OVER && my_move >= 0 && opp_move >= 0) {
    resolve_turn();
    return;
  }

  switch (st) {
    case DS_SELECT: {
      int count = mons[me_idx].move_count;
      if (ev->up) {
        cursor = (cursor + count - 1) % count;
        redraw_moves();
      }
      if (ev->down) {
        cursor = (cursor + 1) % count;
        redraw_moves();
      }
      if (ev->a) {
        my_move = cursor;
        send_move(turn, (uint8_t)my_move);
        last_send = now;
        st = DS_WAIT;
        redraw_moves();
        status_show("locked in. waiting...");
        // Opponent may already have sent; resolve next tick via the
        // check at the top.
      }
      break;
    }
    case DS_WAIT:
      if (now - last_send > RESEND_MS) {
        last_send = now;
        send_move(turn, (uint8_t)my_move);  // survive dropped packets
      }
      break;
    case DS_OVER:
      if (ev->a) nav_show(SCR_PLAY);  // back to the lobby
      break;
  }
}

bool ui_duel_home(const btn_event_t *ev) {
  if (!ev->home) return false;
  ESP_LOGI(TAG, "duel exited via Home");
  nav_show(SCR_PLAY);  // forfeit / leave -> lobby
  return true;
}
