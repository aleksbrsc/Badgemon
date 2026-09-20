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
//
// Scene: assets_duel_bg (baked plates + dialog strip, see assets.h).
// Dynamic HP bars/text are overlaid exactly on the baked plates; the
// move list + cursor live on the dark bottom strip.
#include "ui_duel.h"
#include "engine.h"
#include "net.h"
#include "lobby.h"  // LOBBY_NAME_MAX
#include "payload.h"
#include "nav.h"
#include "fault.h"
#include "hal_led.h"
#include "hal_display.h"
#include "ui_font.h"
#include "assets.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "duel";

#define RESEND_MS 400  // rebroadcast pending move while waiting
// Plate text is dark-on-cream; log text is light-on-dark-strip.
#define PLATE_INK lv_color_hex(0x1F353C)

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
static int blink_div = 0;

// ---- LVGL objects ---------------------------------------------------
static lv_obj_t *foe_name_label;
static lv_obj_t *foe_hp_label;
static lv_obj_t *foe_bar;
static lv_obj_t *me_name_label;
static lv_obj_t *me_hp_label;
static lv_obj_t *me_bar;
static lv_obj_t *move_btnm;
static lv_obj_t *move_cursor;
static lv_obj_t *log_label;

// Matrix rows: one per move (SELECT) or a single waiting row.
// Text copied here (buttonmatrix keeps the pointer, so it must outlive
// temp buffers — same pattern as ui_play.c).
static char move_text[MAX_MOVES][24];
static const char *move_map[MAX_MOVES * 2 + 1];

static bool for_me(const uint8_t *t) { return !memcmp(t, net_mac(), 6); }

// Green info vs red error log (errors also go to the log with context).
static void status_show(const char *s, bool is_err) {
  ESP_LOGI(TAG, "status: %s", s);
  if (!log_label) return;
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(log_label, s);
  lv_obj_set_style_text_color(log_label,
                              is_err ? lv_color_hex(0xFF6060) : lv_color_hex(0xB0FFB0),
                              LV_PART_MAIN);
  lvgl_port_unlock();
}

// Surface a failed send on-screen: "what failed: ESP_ERR_...".
static void status_net_err(const char *what, esp_err_t err) {
  char s[64];
  snprintf(s, sizeof(s), "%s failed: %s", what, esp_err_to_name(err));
  ESP_LOGW(TAG, "%s", s);
  status_show(s, true);
}

// HP fill color: green -> yellow -> red as the mon weakens.
static lv_color_t hp_color(int hp, int max_hp) {
  if (max_hp <= 0) return lv_color_hex(0xD83828);
  int q = (hp * 4) / max_hp;  // 4 = full, 0 = empty
  if (q >= 3) return lv_color_hex(0x38B838);
  if (q >= 2) return lv_color_hex(0xD8B828);
  return lv_color_hex(0xD83828);
}

static void bar_set(lv_obj_t *bar, int hp, int max_hp) {
  if (!bar) return;
  if (!lvgl_port_lock(0)) return;
  lv_bar_set_range(bar, 0, max_hp > 0 ? max_hp : 1);
  lv_bar_set_value(bar, hp, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, hp_color(hp, max_hp), LV_PART_INDICATOR);
  lvgl_port_unlock();
}

static void redraw_hp(void) {
  if (!foe_hp_label) return;  // screen not built yet (enter failed to lock)
  if (!lvgl_port_lock(0)) return;
  char buf[24];
  pokemon_t *mp = &mons[me_idx], *op = &mons[opp_idx];
  snprintf(buf, sizeof(buf), "%d/%d", op->health, op->max_health);
  lv_label_set_text(foe_hp_label, buf);
  snprintf(buf, sizeof(buf), "%d/%d", mp->health, mp->max_health);
  lv_label_set_text(me_hp_label, buf);
  lvgl_port_unlock();
  bar_set(foe_bar, op->health, op->max_health);
  bar_set(me_bar, mp->health, mp->max_health);
}

static void redraw_moves(void) {
  if (!move_btnm) return;
  int rows = 0;
  if (st == DS_SELECT) {
    pokemon_t *mp = &mons[me_idx];
    rows = mp->move_count;
    for (int i = 0; i < rows; i++) {
      snprintf(move_text[i], sizeof(move_text[i]), "%s", mp->moves[i].name);
      move_map[i * 2] = move_text[i];
      move_map[i * 2 + 1] = "\n";
    }
  } else if (st == DS_WAIT) {
    // Single row while waiting (cursor parked on it).
    snprintf(move_text[0], sizeof(move_text[0]), "waiting...");
    move_map[0] = move_text[0];
    move_map[1] = "\n";
    rows = 1;
  } else {
    // OVER: hide the move list + cursor; the result lives in the log.
    if (!lvgl_port_lock(0)) return;
    lv_obj_set_flag(move_btnm, LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(move_cursor, LV_OBJ_FLAG_HIDDEN, true);
    lvgl_port_unlock();
    return;
  }
  if (rows < 1) rows = 1;
  move_map[(rows - 1) * 2 + 1] = "";
  int sel = (st == DS_SELECT) ? cursor : 0;
  if (sel > rows - 1) sel = rows - 1;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_flag(move_btnm, LV_OBJ_FLAG_HIDDEN, false);
  lv_obj_set_flag(move_cursor, LV_OBJ_FLAG_HIDDEN, false);
  lv_buttonmatrix_set_map(move_btnm, move_map);
  lv_buttonmatrix_set_selected_button(move_btnm, (uint16_t)sel);
  lv_obj_set_height(move_btnm, rows * DUEL_MOVE_ROW_H + (rows - 1) * 2);
  lv_obj_set_pos(move_cursor, DUEL_MOVES_X - 12,
                 DUEL_STRIP_Y + sel * (DUEL_MOVE_ROW_H + 2));
  lvgl_port_unlock();
}

// ---- Networking -----------------------------------------------------
static esp_err_t send_move(uint8_t t, uint8_t mv) {
  uint8_t buf[8];
  memcpy(buf, opp_mac, 6);
  buf[6] = t;
  buf[7] = mv;
  esp_err_t err = net_send(PKT_DUEL, buf, sizeof(buf), NULL);
  if (err != ESP_OK) ESP_LOGW(TAG, "send move t=%u mv=%u failed: %s", t, mv,
                              esp_err_to_name(err));
  return err;
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
    n += snprintf(log + n, sizeof(log) - n, " %s: %s (%s)",
                  mons[1].name, a1.name, eff_word(e1));
  } else {
    n += snprintf(log + n, sizeof(log) - n, " %s fainted!", mons[1].name);
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
    snprintf(over, sizeof(over), "%s %s", log, won ? "YOU WIN!" : "you lose...");
    status_show(over, !won);
    redraw_moves();
    hal_led_set_all(won ? 0 : 40, won ? 40 : 0, 0);
    ESP_LOGI(TAG, "duel over: %s", won ? "win" : "lose");
    return;
  }

  st = DS_SELECT;
  cursor = 0;
  status_show(log, false);
  redraw_moves();
  hal_led_set_all(0, 0, 12);
}

// ---- Screen lifecycle ----------------------------------------------
void ui_duel_set_opponent(const uint8_t *mac, const char *name) {
  memcpy(opp_mac, mac, 6);
  strncpy(opp_name, name, sizeof(opp_name) - 1);
  opp_name[sizeof(opp_name) - 1] = '\0';
}

// Invisible fill bar over a baked HP bar: transparent track, colored
// indicator, no border — the art provides the frame.
static lv_obj_t *make_hp_bar(lv_obj_t *scr, int x, int y, int w, int h) {
  lv_obj_t *bar = lv_bar_create(scr);
  lv_obj_set_pos(bar, x, y);
  lv_obj_set_size(bar, w, h);
  lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(bar, 0, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
  return bar;
}

static lv_obj_t *make_plate_label(lv_obj_t *scr, int x, int y) {
  lv_obj_t *l = lv_label_create(scr);
  lv_obj_set_style_text_font(l, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, PLATE_INK, LV_PART_MAIN);
  lv_obj_set_pos(l, x, y);
  return l;
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
  blink_div = 0;
  st = DS_SELECT;

  ESP_LOGI(TAG, "enter vs '%s' as p%d (%s)", opp_name, me_is_p0 ? 0 : 1,
           mons[me_idx].name);

  // Drop any stale handles up front: if we fail to build the screen the
  // redraw_* guards must see NULL, not pointers to freed LVGL objects.
  foe_name_label = foe_hp_label = foe_bar = NULL;
  me_name_label = me_hp_label = me_bar = NULL;
  move_btnm = move_cursor = log_label = NULL;

  if (!lvgl_port_lock(0)) {
    ESP_LOGE(TAG, "could not lock LVGL on enter; screen not built");
    return;
  }
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();

  lv_obj_t *bg = lv_image_create(scr);
  lv_image_set_src(bg, &assets_duel_bg);
  lv_obj_set_pos(bg, 0, 0);

  // Names are static for the duel; HP numbers + bars redraw every turn.
  foe_name_label = make_plate_label(scr, DUEL_FOE_NAME_X, DUEL_FOE_NAME_Y);
  lv_label_set_text(foe_name_label, mons[opp_idx].name);
  foe_hp_label = make_plate_label(scr, DUEL_FOE_HP_X, DUEL_FOE_HP_Y);
  foe_bar = make_hp_bar(scr, DUEL_FOE_BAR_X, DUEL_FOE_BAR_Y,
                        DUEL_FOE_BAR_W, DUEL_FOE_BAR_H);

  me_name_label = make_plate_label(scr, DUEL_ME_NAME_X, DUEL_ME_NAME_Y);
  lv_label_set_text(me_name_label, mons[me_idx].name);
  me_hp_label = make_plate_label(scr, DUEL_ME_HP_X, DUEL_ME_HP_Y);
  me_bar = make_hp_bar(scr, DUEL_ME_BAR_X, DUEL_ME_BAR_Y,
                       DUEL_ME_BAR_W, DUEL_ME_BAR_H);

  move_btnm = lv_buttonmatrix_create(scr);
  lv_buttonmatrix_set_one_checked(move_btnm, false);
  lv_obj_add_state(move_btnm, LV_STATE_FOCUS_KEY);  // cursor highlight
  lv_obj_set_size(move_btnm, DUEL_MOVES_W, DUEL_MOVE_ROW_H);
  lv_obj_set_pos(move_btnm, DUEL_MOVES_X, DUEL_STRIP_Y);
  lv_obj_set_style_text_font(move_btnm, badge_font(), LV_PART_ITEMS);
  lv_obj_set_style_border_width(move_btnm, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(move_btnm, lv_color_hex(0x084841), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(move_btnm, lv_color_hex(0x0B6B5E), LV_PART_ITEMS);
  lv_obj_set_style_text_color(move_btnm, lv_color_white(), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(move_btnm, lv_color_hex(0xE8442E),
                            LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_text_color(move_btnm, lv_color_white(), LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_radius(move_btnm, 4, LV_PART_ITEMS);
  lv_obj_set_style_pad_gap(move_btnm, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(move_btnm, 2, LV_PART_MAIN);

  move_cursor = lv_image_create(scr);
  lv_image_set_src(move_cursor, &assets_cursor_sm);

  log_label = lv_label_create(scr);
  lv_obj_set_style_text_font(log_label, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(log_label, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
  lv_obj_set_pos(log_label, DUEL_LOG_X, DUEL_LOG_Y);
  lv_obj_set_width(log_label, DUEL_LOG_W);
  lv_label_set_long_mode(log_label, LV_LABEL_LONG_WRAP);
  lvgl_port_unlock();

  redraw_hp();
  redraw_moves();
  char intro[48];
  snprintf(intro, sizeof(intro), "vs %s: pick a move!", opp_name);
  status_show(intro, false);
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
        // Lock in: send first so a radio failure keeps us in SELECT
        // (retryable) instead of stranding us in WAIT.
        esp_err_t err = send_move(turn, (uint8_t)cursor);
        if (err != ESP_OK) {
          status_net_err("move", err);
          break;
        }
        my_move = cursor;
        last_send = now;
        st = DS_WAIT;
        redraw_moves();
        status_show("locked in. waiting...", false);
        // Opponent may already have sent; resolve next tick via the
        // check at the top.
      }
      break;
    }
    case DS_WAIT:
      // Amber pulse while waiting (same language as the lobby wait).
      if (++blink_div >= 5) {
        blink_div = 0;
        static bool on = false;
        on = !on;
        hal_led_set_all(on ? 24 : 0, on ? 12 : 0, 0);
      }
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

static const char *state_name(duel_state_t s) {
  switch (s) {
    case DS_SELECT: return "select";
    case DS_WAIT: return "wait";
    case DS_OVER: return "over";
    default: return "?";
  }
}

void ui_duel_debug(char *out, int cap) {
  if (!out || cap < 1) return;
  snprintf(out, cap, "duel vs='%s' st=%s turn=%u me=p%d(%s) hp=%d/%d opp=%d/%d cur=%d",
           opp_name, state_name(st), turn, me_is_p0 ? 0 : 1, mons[me_idx].name,
           mons[me_idx].health, mons[me_idx].max_health,
           mons[opp_idx].health, mons[opp_idx].max_health, cursor);
}
