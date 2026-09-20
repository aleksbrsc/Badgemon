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
// bottom strip is a caption bubble (battle log) that swaps for a
// caption-half + speech bubble command menu on the player's turn:
//   FIGHT  BAG / BADGEMON  RUN, cursor on the active option. FIGHT
// opens the move list (2x2, same full-width caption rect —
// speech-bubble-full art is not in assets yet).
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
#include <ctype.h>

static const char *TAG = "duel";

#define RESEND_MS 400  // rebroadcast pending move while waiting
// Caption text streams one char per tick slice, then each recap line
// holds before auto-advancing (no A press).
#define STREAM_CH_MS 30
#define RECAP_HOLD_MS 2000
// Plate text is dark-on-cream; caption text is light-on-navy;
// speech-bubble options are dark-on-white.
#define PLATE_INK lv_color_hex(0x1F353C)
#define CMD_INK lv_color_hex(0x1F353C)

// Command menu options: (col,row). FIGHT works; the rest are stubs
// until their screens exist (party screen owns BADGEMON later).
// Layout: FIGHT BAG / BADGEMON RUN (BAG top-right, BADGEMON bottom-left
// so each quip fires on the right cell).
static const char *CMD_OPTS[2][2] = {
    {"FIGHT", "BADGEMON"},
    {"BAG", "RUN"},
};

typedef enum { DS_LOG, DS_COMMAND, DS_SELECT, DS_WAIT, DS_OVER } duel_state_t;

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
static int cmd_col, cmd_row;  // command menu cursor (2x2)
static int move_idx;          // move menu cursor (linear into 2-col grid)
static uint32_t last_send;
static int blink_div = 0;

// Caption text streamer + two-line turn recap. resolve_turn() queues
// one line per attacker; DS_LOG streams line 0, holds RECAP_HOLD_MS,
// streams line 1, holds again, then auto-continues (command or over).
static uint32_t duel_now;
static char stream_full[192];
static int stream_len, stream_pos;
static uint32_t stream_last, stream_done_at;
static char recap_msgs[2][160];
static int recap_n, recap_i;
static bool recap_over_pending, recap_won;

// ---- LVGL objects ---------------------------------------------------
static lv_obj_t *foe_name_label;
static lv_obj_t *foe_hp_label;
static lv_obj_t *foe_bar;
static lv_obj_t *me_name_label;
static lv_obj_t *me_hp_label;
static lv_obj_t *me_bar;
// Bottom strip groups (toggled per state): log (full caption), command
// (half caption + speech + options), moves (full caption + move names).
static lv_obj_t *cap_full_img;
static lv_obj_t *log_label;
static lv_obj_t *cap_half_img;
static lv_obj_t *prompt_label;
static lv_obj_t *speech_img;
static lv_obj_t *opt_labels[2][2];
static lv_obj_t *cmd_cursor;
static lv_obj_t *move_labels[MAX_MOVES];
static lv_obj_t *mv_cursor;

static bool for_me(const uint8_t *t) { return !memcmp(t, net_mac(), 6); }

// Uppercase a mon name for the "What will X do?" prompt (names are
// alpha-only, so byte-wise toupper is safe).
static void name_upper(const char *in, char *out, int cap) {
  int i = 0;
  while (in[i] && i < cap - 1) {
    out[i] = (char)toupper((unsigned char)in[i]);
    i++;
  }
  out[i] = '\0';
}

// Caption log with typewriter streaming: new text starts empty and
// fills one char per STREAM_CH_MS in ui_duel_tick. A press while
// streaming completes the line instantly.
static void log_stream_start(const char *s, bool is_err) {
  if (!s) s = "";
  strncpy(stream_full, s, sizeof(stream_full) - 1);
  stream_full[sizeof(stream_full) - 1] = '\0';
  stream_len = (int)strlen(stream_full);
  stream_pos = 0;
  stream_last = duel_now;
  stream_done_at = 0;
  if (stream_len == 0) stream_done_at = duel_now;
  if (!log_label) return;
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(log_label, stream_len == 0 ? "" : " ");
  lv_obj_set_style_text_color(log_label,
                              is_err ? lv_color_hex(0xFF6060) : lv_color_hex(0xB0FFB0),
                              LV_PART_MAIN);
  lvgl_port_unlock();
}

// Green info vs red error log (errors also go to the log with context).
static void status_show(const char *s, bool is_err) {
  ESP_LOGI(TAG, "status: %s", s);
  recap_n = 0;  // single line, no recap sequence
  recap_over_pending = false;
  log_stream_start(s, is_err);
}

static void stream_finish_now(void) {
  if (!log_label || stream_pos >= stream_len) return;
  stream_pos = stream_len;
  stream_done_at = duel_now;
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(log_label, stream_full);
  lvgl_port_unlock();
}

static void stream_pump(uint32_t now) {
  if (!log_label || stream_pos >= stream_len) return;
  if (now - stream_last < STREAM_CH_MS) return;
  stream_last = now;
  stream_pos++;
  if (stream_pos >= stream_len) {
    stream_done_at = now;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(log_label, stream_full);
    lvgl_port_unlock();
    return;
  }
  char tmp[192];
  memcpy(tmp, stream_full, (size_t)stream_pos);
  tmp[stream_pos] = '\0';
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(log_label, tmp);
  lvgl_port_unlock();
}

static bool stream_busy(void) { return stream_pos < stream_len; }

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
  lv_bar_set_value(bar, hp, LV_ANIM_ON);
  lv_obj_set_style_bg_color(bar, hp_color(hp, max_hp), LV_PART_INDICATOR);
  lvgl_port_unlock();
}

static void redraw_hp(void) {
  if (!foe_hp_label) return;  // screen not built yet (enter failed to lock)
  if (!lvgl_port_lock(0)) return;
  char buf[24];
  pokemon_t *mp = &mons[me_idx], *op = &mons[opp_idx];
  // Foe HP numbers stay hidden (bar only); keep the label blank.
  lv_label_set_text(foe_hp_label, "");
  snprintf(buf, sizeof(buf), "%d/%d", mp->health, mp->max_health);
  lv_label_set_text(me_hp_label, buf);
  lvgl_port_unlock();
  bar_set(foe_bar, op->health, op->max_health);
  bar_set(me_bar, mp->health, mp->max_health);
}

// Show exactly one bottom-strip group.
static void show_group(bool log, bool cmd, bool moves) {
  if (!cap_full_img) return;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_flag(cap_full_img, LV_OBJ_FLAG_HIDDEN, !log && !moves);
  lv_obj_set_flag(log_label, LV_OBJ_FLAG_HIDDEN, !log);
  lv_obj_set_flag(cap_half_img, LV_OBJ_FLAG_HIDDEN, !cmd);
  lv_obj_set_flag(prompt_label, LV_OBJ_FLAG_HIDDEN, !cmd);
  lv_obj_set_flag(speech_img, LV_OBJ_FLAG_HIDDEN, !cmd);
  lv_obj_set_flag(cmd_cursor, LV_OBJ_FLAG_HIDDEN, !cmd);
  for (int c = 0; c < 2; c++)
    for (int r = 0; r < 2; r++)
      lv_obj_set_flag(opt_labels[c][r], LV_OBJ_FLAG_HIDDEN, !cmd);
  for (int i = 0; i < MAX_MOVES; i++)
    lv_obj_set_flag(move_labels[i], LV_OBJ_FLAG_HIDDEN, !moves);
  lv_obj_set_flag(mv_cursor, LV_OBJ_FLAG_HIDDEN, !moves);
  lvgl_port_unlock();
}

// "What will X do?" prompt (also restores it after a stub quip).
static void command_prompt(void) {
  if (!prompt_label) return;
  char up[24];
  name_upper(mons[me_idx].name, up, sizeof(up));
  char buf[48];
  snprintf(buf, sizeof(buf), "What will\n%s do?", up);
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(prompt_label, buf);
  lvgl_port_unlock();
}

static void redraw_cmd_cursor(void) {
  if (!cmd_cursor) return;
  int x = (cmd_col == 0 ? DUEL_CMD_COL_X0 : DUEL_CMD_COL_X1) - DUEL_CUR_DX;
  int y = (cmd_row == 0 ? DUEL_CMD_ROW_Y0 : DUEL_CMD_ROW_Y1) - DUEL_CUR_DY;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_pos(cmd_cursor, x, y);
  lvgl_port_unlock();
}

static void to_command(void) {
  st = DS_COMMAND;
  cmd_col = 0;
  cmd_row = 0;
  command_prompt();
  show_group(false, true, false);
  redraw_cmd_cursor();
  hal_led_set_all(0, 0, 12);
}

static void redraw_mv_cursor(void) {
  if (!mv_cursor) return;
  int col = move_idx % 2, row = move_idx / 2;
  int x = (col == 0 ? DUEL_MV_COL_X0 : DUEL_MV_COL_X1) - DUEL_CUR_DX;
  int y = (row == 0 ? DUEL_MV_ROW_Y0 : DUEL_MV_ROW_Y1) - DUEL_CUR_DY;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_pos(mv_cursor, x, y);
  lvgl_port_unlock();
}

// Fill the move list from my active mon and park the cursor on top.
static void to_moves(void) {
  st = DS_SELECT;
  move_idx = 0;
  pokemon_t *mp = &mons[me_idx];
  if (!lvgl_port_lock(0)) return;
  for (int i = 0; i < MAX_MOVES; i++) {
    if (i < mp->move_count) {
      lv_label_set_text(move_labels[i], mp->moves[i].name);
      lv_obj_set_flag(move_labels[i], LV_OBJ_FLAG_HIDDEN, false);
    } else {
      lv_obj_set_flag(move_labels[i], LV_OBJ_FLAG_HIDDEN, true);
    }
  }
  lvgl_port_unlock();
  show_group(false, false, true);
  redraw_mv_cursor();
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
  // Queue one recap line per attacker; DS_LOG streams line 0, holds,
  // then streams line 1 and auto-continues (no A press).
  float e0 = attack_multiplier(&a0, &mons[1]);
  hit(&mons[1], &a0);
  snprintf(recap_msgs[0], sizeof(recap_msgs[0]), "%s: %s (%s)",
           mons[0].name, a0.name, eff_word(e0));
  if (!is_fainted(&mons[1])) {
    float e1 = attack_multiplier(&a1, &mons[0]);
    hit(&mons[0], &a1);
    snprintf(recap_msgs[1], sizeof(recap_msgs[1]), "%s: %s (%s)",
             mons[1].name, a1.name, eff_word(e1));
  } else {
    snprintf(recap_msgs[1], sizeof(recap_msgs[1]), "%s fainted!",
             mons[1].name);
  }

  // Advance bookkeeping (remember this move for catch-up resends).
  prev_turn = turn;
  prev_move = my_move;
  turn++;
  my_move = -1;
  opp_move = -1;

  redraw_hp();  // bars animate while the recap streams

  recap_n = 2;
  recap_i = 0;
  recap_over_pending =
      is_fainted(&mons[me_idx]) || is_fainted(&mons[opp_idx]);
  recap_won = is_fainted(&mons[opp_idx]);
  if (recap_over_pending)
    ESP_LOGI(TAG, "turn resolved, streaming recap then over");
  else
    ESP_LOGI(TAG, "turn resolved, streaming recap");

  st = DS_LOG;
  show_group(true, false, false);
  log_stream_start(recap_msgs[0], false);
}

// Show the pending battle-over line after the recap finishes.
static void show_over(void) {
  st = DS_OVER;
  bool won = recap_won;
  recap_over_pending = false;
  recap_n = 0;
  char over[64];
  snprintf(over, sizeof(over), "%s", won ? "YOU WIN!" : "you lose...");
  show_group(true, false, false);
  log_stream_start(over, !won);
  hal_led_set_all(won ? 0 : 40, won ? 40 : 0, 0);
  ESP_LOGI(TAG, "duel over: %s", won ? "win" : "lose");
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
  lv_obj_set_style_anim_time(bar, 600, LV_PART_MAIN);
  lv_obj_set_style_anim_time(bar, 600, LV_PART_INDICATOR);
  return bar;
}

static lv_obj_t *make_plate_label(lv_obj_t *scr, int x, int y) {
  lv_obj_t *l = lv_label_create(scr);
  lv_obj_set_style_text_font(l, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, PLATE_INK, LV_PART_MAIN);
  lv_obj_set_pos(l, x, y);
  return l;
}

static lv_obj_t *make_caption_text(lv_obj_t *scr, int x, int y, int w) {
  lv_obj_t *l = lv_label_create(scr);
  lv_obj_set_style_text_font(l, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_width(l, w);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
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
  cmd_col = 0;
  cmd_row = 0;
  move_idx = 0;
  last_send = 0;
  blink_div = 0;
  duel_now = 0;
  stream_full[0] = '\0';
  stream_len = stream_pos = 0;
  stream_last = stream_done_at = 0;
  recap_n = recap_i = 0;
  recap_over_pending = recap_won = false;
  st = DS_COMMAND;

  ESP_LOGI(TAG, "enter vs '%s' as p%d (%s)", opp_name, me_is_p0 ? 0 : 1,
           mons[me_idx].name);

  // Drop any stale handles up front: if we fail to build the screen the
  // redraw_* guards must see NULL, not pointers to freed LVGL objects.
  foe_name_label = foe_hp_label = foe_bar = NULL;
  me_name_label = me_hp_label = me_bar = NULL;
  cap_full_img = log_label = NULL;
  cap_half_img = prompt_label = NULL;
  speech_img = cmd_cursor = mv_cursor = NULL;
  for (int c = 0; c < 2; c++)
    for (int r = 0; r < 2; r++) opt_labels[c][r] = NULL;
  for (int i = 0; i < MAX_MOVES; i++) move_labels[i] = NULL;

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
  // Foe HP numbers are hidden (bar only).
  foe_name_label = make_plate_label(scr, DUEL_FOE_NAME_X, DUEL_FOE_NAME_Y);
  lv_label_set_text(foe_name_label, mons[opp_idx].name);
  foe_hp_label = make_plate_label(scr, DUEL_FOE_HP_X, DUEL_FOE_HP_Y);
  lv_label_set_text(foe_hp_label, "");
  lv_obj_add_flag(foe_hp_label, LV_OBJ_FLAG_HIDDEN);
  foe_bar = make_hp_bar(scr, DUEL_FOE_BAR_X, DUEL_FOE_BAR_Y,
                        DUEL_FOE_BAR_W, DUEL_FOE_BAR_H);

  me_name_label = make_plate_label(scr, DUEL_ME_NAME_X, DUEL_ME_NAME_Y);
  lv_label_set_text(me_name_label, mons[me_idx].name);
  me_hp_label = make_plate_label(scr, DUEL_ME_HP_X, DUEL_ME_HP_Y);
  lv_obj_set_width(me_hp_label, DUEL_ME_HP_W);
  lv_obj_set_style_text_align(me_hp_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  me_bar = make_hp_bar(scr, DUEL_ME_BAR_X, DUEL_ME_BAR_Y,
                       DUEL_ME_BAR_W, DUEL_ME_BAR_H);

  // Log group: full caption bubble.
  cap_full_img = lv_image_create(scr);
  lv_image_set_src(cap_full_img, &assets_caption);
  lv_obj_set_pos(cap_full_img, DUEL_CAP_X, DUEL_CAP_Y);
  log_label = make_caption_text(scr, DUEL_LOG_X, DUEL_LOG_Y, DUEL_LOG_W);
  lv_obj_set_style_text_color(log_label, lv_color_hex(0xB0FFB0), LV_PART_MAIN);

  // Command group: half caption (prompt) + speech (options).
  cap_half_img = lv_image_create(scr);
  lv_image_set_src(cap_half_img, &assets_caption_half);
  lv_obj_set_pos(cap_half_img, DUEL_CAP_X, DUEL_CAP_Y);
  prompt_label = make_caption_text(scr, DUEL_LOG_X, DUEL_LOG_Y, DUEL_PROMPT_W);
  speech_img = lv_image_create(scr);
  lv_image_set_src(speech_img, &assets_speech_half);
  lv_obj_set_pos(speech_img, DUEL_SPEECH_X, DUEL_SPEECH_Y);
  for (int c = 0; c < 2; c++) {
    for (int r = 0; r < 2; r++) {
      lv_obj_t *l = lv_label_create(scr);
      lv_obj_set_style_text_font(l, BADGE_FONT_SMALL, LV_PART_MAIN);
      lv_obj_set_style_text_color(l, CMD_INK, LV_PART_MAIN);
      lv_obj_set_pos(l, c == 0 ? DUEL_CMD_COL_X0 : DUEL_CMD_COL_X1,
                     r == 0 ? DUEL_CMD_ROW_Y0 : DUEL_CMD_ROW_Y1);
      lv_label_set_text(l, CMD_OPTS[c][r]);
      opt_labels[c][r] = l;
    }
  }
  cmd_cursor = lv_image_create(scr);
  lv_image_set_src(cmd_cursor, &assets_cursor_sm);

  // Move group: move names inside the full caption rect.
  for (int i = 0; i < MAX_MOVES; i++) {
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, BADGE_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_pos(l, (i % 2) == 0 ? DUEL_MV_COL_X0 : DUEL_MV_COL_X1,
                   (i / 2) == 0 ? DUEL_MV_ROW_Y0 : DUEL_MV_ROW_Y1);
    move_labels[i] = l;
  }
  mv_cursor = lv_image_create(scr);
  lv_image_set_src(mv_cursor, &assets_cursor_sm);
  lvgl_port_unlock();

  redraw_hp();
  to_command();
  ESP_LOGI(TAG, "your move — command menu");
}

void ui_duel_tick(uint32_t now, const btn_event_t *ev) {
  duel_now = now;
  drain_net();

  // Resolve as soon as both moves are in (from net or from my pick).
  if ((st == DS_SELECT || st == DS_WAIT) && my_move >= 0 && opp_move >= 0) {
    resolve_turn();
    return;
  }

  switch (st) {
    case DS_LOG:
      stream_pump(now);
      if (stream_busy()) {
        if (ev->a) stream_finish_now();  // skip the typewriter
        break;
      }
      if (recap_n > 0 && recap_i + 1 < recap_n) {
        // First line fully streamed: hold, then stream the next line.
        if (now - stream_done_at >= RECAP_HOLD_MS) {
          recap_i++;
          log_stream_start(recap_msgs[recap_i], false);
        }
      } else if (recap_over_pending) {
        if (now - stream_done_at >= RECAP_HOLD_MS) show_over();
      } else if (recap_n > 0) {
        // Full recap streamed: hold, then back to the command menu.
        if (now - stream_done_at >= RECAP_HOLD_MS) to_command();
      } else {
        // Single-line log (e.g. send error): brief hold, then command.
        if (ev->a || now - stream_done_at >= RECAP_HOLD_MS) to_command();
      }
      break;
    case DS_COMMAND: {
      bool moved = false;
      if (ev->left || ev->right) { cmd_col ^= 1; moved = true; }
      if (ev->up || ev->down) { cmd_row ^= 1; moved = true; }
      if (moved) {
        command_prompt();  // clear any stub quip
        redraw_cmd_cursor();
      }
      if (ev->a) {
        if (cmd_col == 0 && cmd_row == 0) {
          to_moves();  // FIGHT -> move list
        } else {
          // Stubs until their screens exist (BADGEMON owns party later).
          const char *quip = "BAG is empty!";
          if (cmd_col == 0) quip = "Party soon!";
          else if (cmd_row == 1) quip = "Can't escape!";
          if (!lvgl_port_lock(0)) break;
          lv_label_set_text(prompt_label, quip);
          lvgl_port_unlock();
        }
      }
      break;
    }
    case DS_SELECT: {
      int count = mons[me_idx].move_count;
      if (ev->left || ev->right) {
        int col = (move_idx % 2) ^ 1, row = move_idx / 2;
        if (row * 2 + col < count) move_idx = row * 2 + col;
        redraw_mv_cursor();
      }
      if (ev->up || ev->down) {
        int col = move_idx % 2, row = (move_idx / 2) ^ 1;
        if (row * 2 + col < count) move_idx = row * 2 + col;
        redraw_mv_cursor();
      }
      if (ev->a) {
        // Lock in: send first so a radio failure keeps us choosing
        // (retryable) instead of stranding us in WAIT.
        esp_err_t err = send_move(turn, (uint8_t)move_idx);
        if (err != ESP_OK) {
          st = DS_LOG;
          show_group(true, false, false);
          status_net_err("move", err);
          break;
        }
        my_move = move_idx;
        last_send = now;
        st = DS_WAIT;
        show_group(true, false, false);
        status_show("waiting for foe...", false);
        // Opponent may already have sent; resolve next tick via the
        // check at the top.
      }
      if (ev->b) to_command();  // back out of FIGHT
      break;
    }
    case DS_WAIT:
      stream_pump(now);
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
      stream_pump(now);
      if (ev->a) {
        if (stream_busy())
          stream_finish_now();  // first A completes the line
        else
          nav_show(SCR_PLAY);  // back to the lobby
      }
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
    case DS_LOG: return "log";
    case DS_COMMAND: return "command";
    case DS_SELECT: return "moves";
    case DS_WAIT: return "wait";
    case DS_OVER: return "over";
    default: return "?";
  }
}

void ui_duel_debug(char *out, int cap) {
  if (!out || cap < 1) return;
  snprintf(out, cap, "duel vs='%s' st=%s turn=%u me=p%d(%s) hp=%d/%d opp=%d/%d cur=%d,%d/%d",
           opp_name, state_name(st), turn, me_is_p0 ? 0 : 1, mons[me_idx].name,
           mons[me_idx].health, mons[me_idx].max_health,
           mons[opp_idx].health, mons[opp_idx].max_health, cmd_col, cmd_row, move_idx);
}
