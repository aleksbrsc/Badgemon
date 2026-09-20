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
//   FIGHT BAG / BADGEMON RUN, cursor on the active option. FIGHT
// opens the move list (2x2, same full-width caption rect);
// BADGEMON opens the party overlay; BAG/RUN are stubs. Turn recaps
// stream "<mon> used <move>!", hold, drain the bars, then stream the
// outcome ("It's super effective!" / "<mon> has fainted!" / ...).
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

// Forward declarations (party/intro blocks run before these defs).
static lv_color_t hp_color(int hp, int max_hp);
static void show_over(void);
static void to_command(void);

// Command menu options: (col,row). FIGHT works; the rest are stubs
// until their screens exist (party screen owns BADGEMON later).
// Layout: FIGHT BAG / BADGEMON RUN (BAG top-right, BADGEMON bottom-left
// so each quip fires on the right cell).
static const char *CMD_OPTS[2][2] = {
    {"FIGHT", "BADGEMON"},
    {"BAG", "RUN"},
};

typedef enum { DS_LOG, DS_COMMAND, DS_SELECT, DS_WAIT, DS_OVER, DS_PARTY } duel_state_t;

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

// Caption text streamer + strike-phase turn recap. resolve_turn()
// queues one strike per attacker; DS_LOG streams "<atk> used <move>!",
// holds, applies the damage (bars animate), then streams the outcome
// ("It's super effective!" / "It has fainted!" ...) and holds again
// before the next strike. Fully automatic, no A press.
static uint32_t duel_now;
static char stream_full[192];
static int stream_len, stream_pos;
static uint32_t stream_last, stream_done_at;
// One strike per attacker that gets to move (1 if the first KOs).
typedef struct {
  int atk, def;       // indices into mons[]
  char move[24];      // move name ("used" line)
  char eff[48];       // outcome line, "" when neutral (skipped)
  int dmg;            // precomputed, applied after the "used" hold
} strike_t;
static strike_t strikes[2];
static int n_strikes, strike_i;
// Subphase: USED streams, USED_HOLD waits 2s, EFF streams the outcome,
// EFF_HOLD waits 2s, NEUTRAL_HOLD waits 2s with no line, SINGLE is a
// one-off line (errors) that falls back to the command menu.
typedef enum { SP_USED, SP_EFF, SP_NEUTRAL_HOLD, SP_SINGLE } strike_phase_t;
static strike_phase_t strike_phase;
static uint32_t neutral_until;
static bool over_pending, over_won;

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

// Battle intro wipe (FireRed style): two full-width black bars meet at
// the middle of the screen, then slide apart over INTRO_MS.
static lv_obj_t *intro_top, *intro_bot;
static uint32_t intro_start;
static bool intro_done;

// Party overlay (opened with BADGEMON): fullscreen party art + current
// mon plate + one slot row per extra mon (none yet) + back hint.
// Objects are created on open and deleted on close.
static lv_obj_t *party_objs[8];
static int n_party_objs;

static void party_close(void) {
  if (!lvgl_port_lock(0)) return;
  for (int i = 0; i < n_party_objs; i++) {
    if (party_objs[i]) lv_obj_del(party_objs[i]);
    party_objs[i] = NULL;
  }
  n_party_objs = 0;
  lvgl_port_unlock();
}

static void party_track(lv_obj_t *o) {
  if (n_party_objs < (int)(sizeof(party_objs) / sizeof(party_objs[0])))
    party_objs[n_party_objs++] = o;
}

static void party_open(void) {
  if (!lvgl_port_lock(0)) return;
  lv_obj_t *scr = lv_scr_act();
  lv_obj_t *bg = lv_image_create(scr);
  lv_image_set_src(bg, &assets_party_screen);
  lv_obj_set_pos(bg, 0, 0);
  party_track(bg);
  // Current mon in the baked top-left panel.
  lv_obj_t *nm = lv_label_create(scr);
  lv_obj_set_style_text_font(nm, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(nm, PLATE_INK, LV_PART_MAIN);
  lv_obj_set_pos(nm, PARTY2_NAME_X, PARTY2_NAME_Y);
  lv_label_set_text(nm, mons[me_idx].name);
  party_track(nm);
  char hp[24];
  snprintf(hp, sizeof(hp), "%d/%d", mons[me_idx].health,
           mons[me_idx].max_health);
  lv_obj_t *hp_l = lv_label_create(scr);
  lv_obj_set_style_text_font(hp_l, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(hp_l, PLATE_INK, LV_PART_MAIN);
  lv_obj_set_pos(hp_l, PARTY2_HP_X, PARTY2_HP_Y);
  lv_label_set_text(hp_l, hp);
  party_track(hp_l);
  lv_obj_t *bar = lv_bar_create(scr);
  lv_obj_set_pos(bar, PARTY2_BAR_X, PARTY2_BAR_Y);
  lv_obj_set_size(bar, PARTY2_BAR_W, PARTY2_BAR_H);
  lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(bar, 0, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
  lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_bar_set_range(bar, 0, mons[me_idx].max_health > 0
                            ? mons[me_idx].max_health
                            : 1);
  lv_bar_set_value(bar, mons[me_idx].health, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar,
                            hp_color(mons[me_idx].health,
                                     mons[me_idx].max_health),
                            LV_PART_INDICATOR);
  party_track(bar);
  // Extra mons (none yet): one slot row each at (138, 8 + i*pitch).
  // The loop stays empty until the team model grows past one mon.
  for (int i = 0; i < 0; i++) {
    lv_obj_t *slot = lv_image_create(scr);
    lv_image_set_src(slot, &assets_slot);
    lv_obj_set_pos(slot, PARTY2_SLOT_X, PARTY2_SLOT_Y + i * PARTY2_SLOT_PITCH);
    party_track(slot);
  }
  lv_obj_t *hint = lv_label_create(scr);
  lv_obj_set_style_text_font(hint, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, PLATE_INK, LV_PART_MAIN);
  lv_obj_set_pos(hint, PARTY2_DLG_X, PARTY2_DLG_Y);
  lv_obj_set_width(hint, PARTY2_DLG_W);
  lv_label_set_text(hint, "B: back");
  party_track(hint);
  lvgl_port_unlock();
  st = DS_PARTY;
  ESP_LOGI(TAG, "party open");
}

// Slide the intro bars apart; when done, delete them.
static void intro_update(uint32_t now) {
  if (intro_done || !intro_top || !intro_bot) return;
  uint32_t t = now - intro_start;
  if (t >= INTRO_MS) {
    if (!lvgl_port_lock(0)) return;
    lv_obj_del(intro_top);
    lv_obj_del(intro_bot);
    intro_top = intro_bot = NULL;
    lvgl_port_unlock();
    intro_done = true;
    return;
  }
  int off = (int)((t * INTRO_BAR_H) / INTRO_MS);
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_pos(intro_top, 0, -off);
  lv_obj_set_pos(intro_bot, 0, INTRO_BAR_H + off);
  lvgl_port_unlock();
}

static void intro_start_bars(void) {
  if (!lvgl_port_lock(0)) return;
  lv_obj_t *scr = lv_scr_act();
  intro_top = lv_obj_create(scr);
  lv_obj_set_pos(intro_top, 0, 0);
  lv_obj_set_size(intro_top, HAL_LCD_W, INTRO_BAR_H);
  lv_obj_set_style_bg_color(intro_top, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(intro_top, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(intro_top, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(intro_top, 0, LV_PART_MAIN);
  intro_bot = lv_obj_create(scr);
  lv_obj_set_pos(intro_bot, 0, INTRO_BAR_H);
  lv_obj_set_size(intro_bot, HAL_LCD_W, INTRO_BAR_H);
  lv_obj_set_style_bg_color(intro_bot, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(intro_bot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(intro_bot, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(intro_bot, 0, LV_PART_MAIN);
  lvgl_port_unlock();
  intro_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
  intro_done = false;
}

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
                              is_err ? lv_color_hex(0xFF6060) : lv_color_white(),
                              LV_PART_MAIN);
  lvgl_port_unlock();
}

// Plain info vs red error log (errors also go to the log with context).
static void status_show(const char *s, bool is_err) {
  ESP_LOGI(TAG, "status: %s", s);
  n_strikes = 0;  // single line, no strike sequence
  strike_phase = SP_SINGLE;
  over_pending = false;
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

// HP fill color: green at half+, orange-yellow under half, red under
// a quarter.
static lv_color_t hp_color(int hp, int max_hp) {
  if (max_hp <= 0) return lv_color_hex(0xD83828);
  if (hp * 4 < max_hp) return lv_color_hex(0xD83828);
  if (hp * 2 < max_hp) return lv_color_hex(0xE8A020);
  return lv_color_hex(0x38B838);
}

static void bar_set(lv_obj_t *bar, int hp, int max_hp, lv_anim_enable_t anim) {
  if (!bar) return;
  if (!lvgl_port_lock(0)) return;
  lv_bar_set_range(bar, 0, max_hp > 0 ? max_hp : 1);
  lv_bar_set_value(bar, hp, anim);
  lv_obj_set_style_bg_color(bar, hp_color(hp, max_hp), LV_PART_INDICATOR);
  lvgl_port_unlock();
}

// First draw of the duel is instant (bars start full); HP changes
// mid-battle animate.
static bool bars_init;

static void redraw_hp(void) {
  if (!foe_hp_label) return;  // screen not built yet (enter failed to lock)
  if (!lvgl_port_lock(0)) return;
  char buf[24];
  pokemon_t *mp = &mons[me_idx];
  // Foe HP numbers stay hidden (bar only); keep the label blank.
  lv_label_set_text(foe_hp_label, "");
  snprintf(buf, sizeof(buf), "%d/%d", mp->health, mp->max_health);
  lv_label_set_text(me_hp_label, buf);
  lvgl_port_unlock();
  lv_anim_enable_t anim = bars_init ? LV_ANIM_ON : LV_ANIM_OFF;
  bars_init = true;
  bar_set(foe_bar, mons[opp_idx].health, mons[opp_idx].max_health, anim);
  bar_set(me_bar, mp->health, mp->max_health, anim);
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
// Outcome line for a strike: faint / effectiveness. "" when neutral
// (no line, just the hold).
static void eff_text(char *out, int cap, float e, const char *def_name,
                     bool faint) {
  if (faint)
    snprintf(out, cap, "%s has fainted!", def_name);
  else if (e <= 0.0f)
    snprintf(out, cap, "It has no effect!");
  else if (e > 1.0f)
    snprintf(out, cap, "It's super effective!");
  else if (e < 1.0f)
    snprintf(out, cap, "It's not very effective...");
  else if (cap > 0)
    out[0] = '\0';
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

// "<attacker> used <move>!" for the current strike.
static void strike_used_text(char *out, int cap) {
  strike_t *s = &strikes[strike_i];
  snprintf(out, cap, "%s used %s!", mons[s->atk].name, s->move);
}

// Apply the current strike's damage and redraw (bars animate).
static void strike_apply(void) {
  strike_t *s = &strikes[strike_i];
  pokemon_t *def = &mons[s->def];
  def->health -= s->dmg;
  if (def->health < 0) def->health = 0;
  redraw_hp();
}

// Past the last strike: battle over (faint) or back to commands.
static void strike_finish(void) {
  if (over_pending) {
    show_over();
    return;
  }
  to_command();
}

static void resolve_turn(void) {
  // Map my/opponent moves onto the fixed p0/p1 simulation so both
  // badges resolve identically regardless of perspective.
  int mv0 = me_is_p0 ? my_move : opp_move;
  int mv1 = me_is_p0 ? opp_move : my_move;
  incoming_attack_t a0 = attack_of(0, mv0);
  incoming_attack_t a1 = attack_of(1, mv1);

  // Player 0 strikes first (deterministic tiebreak by MAC order).
  // Damage is precomputed but NOT applied yet: each strike streams
  // "<atk> used <move>!", holds 2s, applies damage (bars drain), then
  // streams the outcome and holds 2s more. The second strike is
  // skipped when the first KOs.
  float e0 = attack_multiplier(&a0, &mons[1]);
  int d0 = calculate_damage(&a0, &mons[1]);
  bool faint1 = mons[1].health - d0 <= 0;
  strikes[0].atk = 0;
  strikes[0].def = 1;
  strikes[0].dmg = d0;
  strncpy(strikes[0].move, a0.name, sizeof(strikes[0].move) - 1);
  strikes[0].move[sizeof(strikes[0].move) - 1] = '\0';
  eff_text(strikes[0].eff, sizeof(strikes[0].eff), e0, mons[1].name,
           faint1);
  n_strikes = 1;
  if (!faint1) {
    float e1 = attack_multiplier(&a1, &mons[0]);
    int d1 = calculate_damage(&a1, &mons[0]);
    bool faint0 = mons[0].health - d1 <= 0;
    strikes[1].atk = 1;
    strikes[1].def = 0;
    strikes[1].dmg = d1;
    strncpy(strikes[1].move, a1.name, sizeof(strikes[1].move) - 1);
    strikes[1].move[sizeof(strikes[1].move) - 1] = '\0';
    eff_text(strikes[1].eff, sizeof(strikes[1].eff), e1, mons[0].name,
             faint0);
    n_strikes = 2;
  }

  // Advance bookkeeping (remember this move for catch-up resends).
  prev_turn = turn;
  prev_move = my_move;
  turn++;
  my_move = -1;
  opp_move = -1;

  bool faint0 = n_strikes > 1 &&
                  mons[0].health - strikes[1].dmg <= 0;
  over_pending = faint1 || faint0;
  over_won = (opp_idx == 1) ? faint1 : faint0;
  ESP_LOGI(TAG, "turn resolved, streaming strikes (n=%d)", n_strikes);

  strike_i = 0;
  strike_phase = SP_USED;
  st = DS_LOG;
  show_group(true, false, false);
  char used[64];
  strike_used_text(used, sizeof(used));
  log_stream_start(used, false);
}

// Show the pending battle-over line after the strikes finish.
static void show_over(void) {
  st = DS_OVER;
  bool won = over_won;
  over_pending = false;
  n_strikes = 0;
  strike_phase = SP_SINGLE;
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
  n_strikes = strike_i = 0;
  strike_phase = SP_SINGLE;
  neutral_until = 0;
  over_pending = over_won = false;
  bars_init = false;
  intro_top = intro_bot = NULL;
  intro_done = true;
  n_party_objs = 0;
  for (int i = 0; i < (int)(sizeof(party_objs) / sizeof(party_objs[0])); i++)
    party_objs[i] = NULL;
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
  intro_top = intro_bot = NULL;
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
  lv_obj_set_style_text_color(log_label, lv_color_white(), LV_PART_MAIN);

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
  intro_start_bars();  // FireRed-style black-bar wipe over the scene
  ESP_LOGI(TAG, "your move — command menu");
}

void ui_duel_tick(uint32_t now, const btn_event_t *ev) {
  duel_now = now;
  drain_net();
  intro_update(now);
  if (!intro_done) return;  // wipe plays out before any input

  // Resolve as soon as both moves are in (from net or from my pick).
  if ((st == DS_SELECT || st == DS_WAIT) && my_move >= 0 && opp_move >= 0) {
    resolve_turn();
    return;
  }

  switch (st) {
    case DS_LOG: {
      stream_pump(now);
      if (stream_busy()) {
        if (ev->a) stream_finish_now();  // skip the typewriter
        break;
      }
      // Advance to the next strike (or finish the turn).
      bool adv = false;
      switch (strike_phase) {
        case SP_USED:
          // "<atk> used <move>!" fully streamed: hold 2s, then the
          // health-bar change takes effect.
          if (now - stream_done_at >= RECAP_HOLD_MS) {
            strike_apply();
            if (strikes[strike_i].eff[0]) {
              log_stream_start(strikes[strike_i].eff, false);
              strike_phase = SP_EFF;
            } else {
              // Neutral hit: no outcome line, just hold 2s more.
              neutral_until = now + RECAP_HOLD_MS;
              strike_phase = SP_NEUTRAL_HOLD;
            }
          }
          break;
        case SP_EFF:
          // Outcome fully streamed: hold 2s, then continue.
          if (now - stream_done_at >= RECAP_HOLD_MS) adv = true;
          break;
        case SP_NEUTRAL_HOLD:
          if (now >= neutral_until) adv = true;
          break;
        case SP_SINGLE:
          // One-off line (e.g. send error): brief hold, then command.
          if (ev->a || now - stream_done_at >= RECAP_HOLD_MS)
            to_command();
          break;
      }
      if (adv) {
        strike_i++;
        if (strike_i < n_strikes) {
          char used[64];
          strike_used_text(used, sizeof(used));
          log_stream_start(used, false);
          strike_phase = SP_USED;
        } else {
          strike_finish();
        }
      }
      break;
    }
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
        } else if (cmd_col == 0 && cmd_row == 1) {
          party_open();  // BADGEMON -> party screen
        } else {
          // Stubs until their screens exist.
          const char *quip = "BAG is empty!";
          if (cmd_row == 1) quip = "Can't escape!";
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
        status_show("Waiting for foe...", false);
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
    case DS_PARTY:
      if (ev->a || ev->b) {
        party_close();
        to_command();  // back to the command menu
      }
      break;
  }
}

bool ui_duel_home(const btn_event_t *ev) {
  if (!ev->home) return false;
  if (st == DS_PARTY) {
    ESP_LOGI(TAG, "party closed via Home");
    party_close();
    to_command();
    return true;
  }
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
    case DS_PARTY: return "party";
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
