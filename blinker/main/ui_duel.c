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
#include "game.h"
#include "pokemon_data.h"
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

typedef enum {
  DS_SYNC,
  DS_LOG,
  DS_COMMAND,
  DS_SELECT,
  DS_WAIT,
  DS_OVER,
  DS_PARTY
} duel_state_t;

// ---- Battle state ---------------------------------------------------
static uint8_t opp_mac[6];
static char opp_name[LOBBY_NAME_MAX + 1];

static pokemon_t mons[2];  // mons[0] = player 0, mons[1] = player 1
static bool me_is_p0;
static int me_idx, opp_idx;
static species_id_t my_species;
static species_id_t opp_species;
static bool opp_synced;

static lv_obj_t *me_sprite;
static lv_obj_t *foe_sprite;
static lv_obj_t *me_ball;
static lv_obj_t *foe_ball;

// Per-strike battle sprite choreography (see sprite_battle_tick).
static bool strike_lunge_started;
static bool strike_damage_applied;

#define RELEASE_BALL_WAIT_MS 320
#define RELEASE_BRIGHT_MS 60
#define RELEASE_DIM_MS 100
#define RELEASE_FLIP_MS 65
#define RELEASE_FLIP_TOGGLES 4
#define SPRITE_BRIGHT_OPA LV_OPA_80
#define SPRITE_LUNGE_MS 140
#define SPRITE_LUNGE_DX (-8)
#define SPRITE_LUNGE_DY 8
#define SPRITE_TWITCH_MS 70
#define SPRITE_TWITCH_PX 5
#define SPRITE_FLASH_HALF_MS 250
#define SPRITE_FLASH_COUNT 3
#define SPRITE_FAINT_MS 800
#define SPRITE_FAINT_SLIDE 40

typedef enum {
  SANIM_NONE,
  SANIM_LUNGE,
  SANIM_HURT,
  SANIM_FAINT,
} sprite_anim_t;

static sprite_anim_t sanim;
static uint32_t sanim_start;
static int sanim_step;
static lv_obj_t *sanim_obj;
static int sanim_base_x, sanim_base_y;
static bool me_sprite_fainted;
static bool foe_sprite_fainted;
static int me_idle_frame;
static int foe_idle_frame;

typedef struct {
  bool active;
  bool done;
  bool queued;
  uint8_t step;
  uint32_t step_start;
  int flip_i;
} send_out_t;

static send_out_t send_me, send_foe;

static lv_obj_t *intro_top, *intro_bot;
static uint32_t intro_start;
static bool intro_done;

typedef struct {
  const lv_image_dsc_t *back0;
  const lv_image_dsc_t *back1;
  const lv_image_dsc_t *front0;
  const lv_image_dsc_t *front1;
  int back_x0, back_x1, back_y;
  int front_x0, front_x1, front_y;
  int poke_me_x, poke_me_y;
  int poke_foe_x, poke_foe_y;
} spr_layout_t;

static bool spr_layout_for(species_id_t sp, spr_layout_t *l) {
  if (!l) return false;
  memset(l, 0, sizeof(*l));
  switch (sp) {
    case SPECIES_CHARMANDER:
      l->back0 = &assets_vinyl_back_1;
      l->back1 = &assets_vinyl_back_2;
      l->front0 = &assets_vinyl_front_1;
      l->front1 = &assets_vinyl_front_2;
      l->back_x0 = DUEL_VINYL_BACK_X1;
      l->back_x1 = DUEL_VINYL_BACK_X2;
      l->back_y = DUEL_VINYL_BACK_Y;
      l->front_x0 = DUEL_VINYL_FOE_X1;
      l->front_x1 = DUEL_VINYL_FOE_X2;
      l->front_y = DUEL_VINYL_FOE_Y;
      l->poke_me_x = DUEL_VINYL_POKEBALL_ME_X;
      l->poke_me_y = DUEL_VINYL_POKEBALL_ME_Y;
      l->poke_foe_x = DUEL_VINYL_POKEBALL_FOE_X;
      l->poke_foe_y = DUEL_VINYL_POKEBALL_FOE_Y;
      return true;
    case SPECIES_SQUIRTLE:
      l->back0 = &assets_ginny_back_1;
      l->back1 = &assets_ginny_back_2;
      l->front0 = &assets_ginny_front_1;
      l->front1 = &assets_ginny_front_2;
      l->back_x0 = DUEL_GINNY_BACK_X;
      l->back_x1 = DUEL_GINNY_BACK_X;
      l->back_y = DUEL_GINNY_BACK_Y;
      l->front_x0 = DUEL_GINNY_FOE_X;
      l->front_x1 = DUEL_GINNY_FOE_X;
      l->front_y = DUEL_GINNY_FOE_Y;
      l->poke_me_x = DUEL_GINNY_POKEBALL_ME_X;
      l->poke_me_y = DUEL_GINNY_POKEBALL_ME_Y;
      l->poke_foe_x = DUEL_GINNY_POKEBALL_FOE_X;
      l->poke_foe_y = DUEL_GINNY_POKEBALL_FOE_Y;
      return true;
    case SPECIES_BULBASAUR:
      l->back0 = &assets_patchy_back_1;
      l->back1 = &assets_patchy_back_2;
      l->front0 = &assets_patchy_front_1;
      l->front1 = &assets_patchy_front_2;
      l->back_x0 = DUEL_PATCH_BACK_X;
      l->back_x1 = DUEL_PATCH_BACK_X;
      l->back_y = DUEL_PATCH_BACK_Y;
      l->front_x0 = DUEL_PATCH_FOE_X;
      l->front_x1 = DUEL_PATCH_FOE_X;
      l->front_y = DUEL_PATCH_FOE_Y;
      l->poke_me_x = DUEL_PATCH_POKEBALL_ME_X;
      l->poke_me_y = DUEL_PATCH_POKEBALL_ME_Y;
      l->poke_foe_x = DUEL_PATCH_POKEBALL_FOE_X;
      l->poke_foe_y = DUEL_PATCH_POKEBALL_FOE_Y;
      return true;
    default:
      return false;
  }
}

static bool species_has_sprite(species_id_t sp) {
  spr_layout_t tmp;
  return spr_layout_for(sp, &tmp);
}

static void spr_back_xy(species_id_t sp, int frame, int *x, int *y) {
  spr_layout_t l;
  if (!spr_layout_for(sp, &l)) return;
  *x = frame ? l.back_x1 : l.back_x0;
  *y = l.back_y;
}

static void spr_front_xy(species_id_t sp, int frame, int *x, int *y) {
  spr_layout_t l;
  if (!spr_layout_for(sp, &l)) return;
  *x = frame ? l.front_x1 : l.front_x0;
  *y = l.front_y;
}

static void sprite_set_opa(lv_obj_t *img, lv_opa_t opa) {
  if (!img) return;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_style_image_opa(img, opa, LV_PART_MAIN);
  lvgl_port_unlock();
}

static void sprite_set_bright(lv_obj_t *img, lv_opa_t recolor_opa) {
  if (!img) return;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_style_image_recolor(img, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_image_recolor_opa(img, recolor_opa, LV_PART_MAIN);
  lv_obj_set_style_image_opa(img, LV_OPA_COVER, LV_PART_MAIN);
  lvgl_port_unlock();
}

static void sprite_set_pos_off(lv_obj_t *img, int base_x, int base_y, int dx,
                              int dy) {
  if (!img) return;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_pos(img, base_x + dx, base_y + dy);
  lvgl_port_unlock();
}

static void me_sprite_layout(int frame, int dx, int dy) {
  spr_layout_t l;
  if (!me_sprite || me_sprite_fainted || !spr_layout_for(my_species, &l)) return;
  int x = frame ? l.back_x1 : l.back_x0;
  if (!lvgl_port_lock(0)) return;
  lv_image_set_src(me_sprite, frame ? l.back1 : l.back0);
  lv_obj_set_pos(me_sprite, x + dx, l.back_y + dy);
  lvgl_port_unlock();
}

static void foe_sprite_layout(int frame, int dx, int dy) {
  spr_layout_t l;
  if (!foe_sprite || foe_sprite_fainted || !spr_layout_for(opp_species, &l))
    return;
  int x = frame ? l.front_x1 : l.front_x0;
  if (!lvgl_port_lock(0)) return;
  lv_image_set_src(foe_sprite, frame ? l.front1 : l.front0);
  lv_obj_set_pos(foe_sprite, x + dx, l.front_y + dy);
  lvgl_port_unlock();
}

static lv_obj_t *sprite_for_player_idx(int idx) {
  return idx == me_idx ? me_sprite : foe_sprite;
}

static void sprite_refresh_visibility(void) {
  if (!lvgl_port_lock(0)) return;
  if (me_sprite) {
    bool show = species_has_sprite(my_species) && !me_sprite_fainted &&
                send_me.done;
    lv_obj_set_flag(me_sprite, LV_OBJ_FLAG_HIDDEN, !show);
  }
  if (foe_sprite) {
    bool show = opp_synced && species_has_sprite(opp_species) &&
                !foe_sprite_fainted && send_foe.done;
    lv_obj_set_flag(foe_sprite, LV_OBJ_FLAG_HIDDEN, !show);
  }
  lvgl_port_unlock();
  if (me_sprite && species_has_sprite(my_species) && !me_sprite_fainted &&
      send_me.done)
    me_sprite_layout(me_idle_frame, 0, 0);
  if (foe_sprite && opp_synced && species_has_sprite(opp_species) &&
      !foe_sprite_fainted && send_foe.done)
    foe_sprite_layout(foe_idle_frame, 0, 0);
}

static bool send_out_busy(void) { return send_me.active || send_foe.active; }

static bool sprite_anim_busy(void) {
  return sanim != SANIM_NONE || send_out_busy();
}

static void send_out_queue(send_out_t *so, species_id_t sp) {
  if (so->done || so->active || so->queued) return;
  if (!species_has_sprite(sp)) {
    so->done = true;
    return;
  }
  so->queued = true;
}

static void send_out_begin(send_out_t *so, lv_obj_t *ball, lv_obj_t *mon,
                           bool is_me, species_id_t sp) {
  spr_layout_t lay;
  if (!ball || !mon || so->done || so->active || !spr_layout_for(sp, &lay))
    return;
  so->active = true;
  so->queued = false;
  so->step = 0;
  so->flip_i = 0;
  so->step_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if (is_me)
    me_idle_frame = 0;
  else
    foe_idle_frame = 0;
  if (!lvgl_port_lock(0)) return;
  lv_image_set_src(ball, is_me ? &assets_pokeball : &assets_pokeball_foe);
  lv_obj_set_pos(ball, is_me ? lay.poke_me_x : lay.poke_foe_x,
                 is_me ? lay.poke_me_y : lay.poke_foe_y);
  lv_obj_clear_flag(ball, LV_OBJ_FLAG_HIDDEN);
  sprite_set_bright(ball, 0);
  lv_obj_set_flag(mon, LV_OBJ_FLAG_HIDDEN, true);
  lvgl_port_unlock();
}

static void send_out_finish(send_out_t *so, lv_obj_t *ball, lv_obj_t *mon,
                            bool is_me) {
  so->active = false;
  so->done = true;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_flag(ball, LV_OBJ_FLAG_HIDDEN, true);
  sprite_set_bright(ball, 0);
  sprite_set_bright(mon, 0);
  lvgl_port_unlock();
  if (is_me)
    me_idle_frame = 0;
  else
    foe_idle_frame = 0;
  if (is_me)
    me_sprite_layout(0, 0, 0);
  else
    foe_sprite_layout(0, 0, 0);
  sprite_refresh_visibility();
}

static void send_out_tick_one(send_out_t *so, lv_obj_t *ball, lv_obj_t *mon,
                              bool is_me, species_id_t sp, uint32_t now) {
  if (!so->active || !ball || !mon) return;
  uint32_t el = now - so->step_start;
  switch (so->step) {
    case 0:
      if (el >= RELEASE_BALL_WAIT_MS) {
        so->step = 1;
        so->step_start = now;
        if (!lvgl_port_lock(0)) return;
        lv_image_set_src(ball, is_me ? &assets_pokeball_open
                                     : &assets_pokeball_open_foe);
        lv_obj_clear_flag(mon, LV_OBJ_FLAG_HIDDEN);
        if (is_me) {
          me_idle_frame = 0;
          me_sprite_layout(0, 0, 0);
        } else {
          foe_idle_frame = 0;
          foe_sprite_layout(0, 0, 0);
        }
        sprite_set_bright(ball, SPRITE_BRIGHT_OPA);
        sprite_set_bright(mon, SPRITE_BRIGHT_OPA);
        lvgl_port_unlock();
      }
      break;
    case 1:
      if (el >= RELEASE_BRIGHT_MS) {
        so->step = 2;
        so->step_start = now;
      }
      break;
    case 2: {
      if (el >= RELEASE_DIM_MS) {
        sprite_set_bright(ball, 0);
        sprite_set_bright(mon, 0);
        so->step = 3;
        so->step_start = now;
        so->flip_i = 0;
        break;
      }
      lv_opa_t fade = (lv_opa_t)(SPRITE_BRIGHT_OPA *
                                 (int)(RELEASE_DIM_MS - el) / RELEASE_DIM_MS);
      sprite_set_bright(ball, fade);
      sprite_set_bright(mon, fade);
      break;
    }
    case 3:
      if (el >= RELEASE_FLIP_MS) {
        so->flip_i++;
        so->step_start = now;
        if (is_me) {
          me_idle_frame ^= 1;
          me_sprite_layout(me_idle_frame, 0, 0);
        } else {
          foe_idle_frame ^= 1;
          foe_sprite_layout(foe_idle_frame, 0, 0);
        }
        if (so->flip_i >= RELEASE_FLIP_TOGGLES) {
          send_out_finish(so, ball, mon, is_me);
        }
      }
      break;
  }
}

static void send_out_try_start(void) {
  if (!intro_done || !opp_synced) return;
  if (species_has_sprite(my_species) && send_me.queued && !send_me.active &&
      !send_me.done)
    send_out_begin(&send_me, me_ball, me_sprite, true, my_species);
  if (species_has_sprite(opp_species) && send_foe.queued && !send_foe.active &&
      !send_foe.done)
    send_out_begin(&send_foe, foe_ball, foe_sprite, false, opp_species);
}

static void send_out_tick(uint32_t now) {
  send_out_try_start();
  send_out_tick_one(&send_me, me_ball, me_sprite, true, my_species, now);
  send_out_tick_one(&send_foe, foe_ball, foe_sprite, false, opp_species, now);
}

static void sprite_anim_begin(sprite_anim_t kind, lv_obj_t *obj, int base_x,
                              int base_y) {
  sanim = kind;
  sanim_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
  sanim_step = 0;
  sanim_obj = obj;
  sanim_base_x = base_x;
  sanim_base_y = base_y;
}

static void sprite_lunge_foe_start(void) {
  int x, y;
  if (!foe_sprite || foe_sprite_fainted) return;
  spr_front_xy(opp_species, foe_idle_frame, &x, &y);
  sprite_anim_begin(SANIM_LUNGE, foe_sprite, x, y);
}

static void sprite_hurt_start(int def_idx) {
  lv_obj_t *obj = sprite_for_player_idx(def_idx);
  if (!obj) return;
  if (def_idx == me_idx && (me_sprite_fainted || !species_has_sprite(my_species)))
    return;
  if (def_idx == opp_idx &&
      (foe_sprite_fainted || !species_has_sprite(opp_species)))
    return;
  int x = 0, y = 0;
  if (def_idx == me_idx)
    spr_back_xy(my_species, me_idle_frame, &x, &y);
  else
    spr_front_xy(opp_species, foe_idle_frame, &x, &y);
  sprite_anim_begin(SANIM_HURT, obj, x, y);
}

static void sprite_faint_start(int def_idx) {
  lv_obj_t *obj = sprite_for_player_idx(def_idx);
  if (!obj) return;
  int x = 0, y = 0;
  if (def_idx == me_idx)
    spr_back_xy(my_species, me_idle_frame, &x, &y);
  else
    spr_front_xy(opp_species, foe_idle_frame, &x, &y);
  if (def_idx == me_idx)
    me_sprite_fainted = true;
  else
    foe_sprite_fainted = true;
  sprite_anim_begin(SANIM_FAINT, obj, x, y);
}

static void sprite_anim_finish(void) {
  sanim = SANIM_NONE;
  sanim_obj = NULL;
  sprite_refresh_visibility();
}

static void sprite_battle_tick(uint32_t now) {
  send_out_tick(now);
  if (sanim == SANIM_NONE) return;
  uint32_t el = now - sanim_start;
  if (sanim == SANIM_LUNGE) {
    if (el < SPRITE_LUNGE_MS) {
      sprite_set_pos_off(sanim_obj, sanim_base_x, sanim_base_y, SPRITE_LUNGE_DX,
                         SPRITE_LUNGE_DY);
    } else if (el < SPRITE_LUNGE_MS * 2) {
      sprite_set_pos_off(sanim_obj, sanim_base_x, sanim_base_y, 0, 0);
    } else {
      sprite_anim_finish();
    }
    return;
  }
  if (sanim == SANIM_HURT) {
    // Twitch left/right, then opacity flash 250 ms on/off x3.
    int twitch_steps = 4;
    uint32_t twitch_end = twitch_steps * SPRITE_TWITCH_MS;
    if (el < twitch_end) {
      int step = (int)(el / SPRITE_TWITCH_MS);
      int dx = (step % 2 == 0) ? -SPRITE_TWITCH_PX : SPRITE_TWITCH_PX;
      sprite_set_pos_off(sanim_obj, sanim_base_x, sanim_base_y, dx, 0);
      sprite_set_opa(sanim_obj, LV_OPA_COVER);
    } else {
      uint32_t flash_el = el - twitch_end;
      uint32_t cycle = flash_el / SPRITE_FLASH_HALF_MS;
      if (cycle >= (uint32_t)(SPRITE_FLASH_COUNT * 2)) {
        sprite_set_pos_off(sanim_obj, sanim_base_x, sanim_base_y, 0, 0);
        sprite_set_opa(sanim_obj, LV_OPA_COVER);
        sprite_anim_finish();
      } else {
        sprite_set_pos_off(sanim_obj, sanim_base_x, sanim_base_y, 0, 0);
        sprite_set_opa(sanim_obj,
                       (cycle % 2 == 0) ? LV_OPA_COVER : LV_OPA_TRANSP);
      }
    }
    return;
  }
  if (sanim == SANIM_FAINT) {
    if (el >= SPRITE_FAINT_MS) {
      sprite_set_opa(sanim_obj, LV_OPA_TRANSP);
      if (!lvgl_port_lock(0)) return;
      lv_obj_set_flag(sanim_obj, LV_OBJ_FLAG_HIDDEN, true);
      lvgl_port_unlock();
      sprite_anim_finish();
      return;
    }
    int slide = (sanim_obj == me_sprite) ? -SPRITE_FAINT_SLIDE : SPRITE_FAINT_SLIDE;
    int dx = (int)((long)slide * (long)el / (long)SPRITE_FAINT_MS);
    lv_opa_t opa = (lv_opa_t)(255 - (255 * (int)el / (int)SPRITE_FAINT_MS));
    sprite_set_pos_off(sanim_obj, sanim_base_x, sanim_base_y, dx, 0);
    sprite_set_opa(sanim_obj, opa);
  }
}

// Battle intro wipe (FireRed style):

static duel_state_t st;
static uint8_t turn;      // current turn number (wraps at 256; fine)
static int my_move;       // move index I committed this turn, -1 = none
static int opp_move;      // opponent's move for this turn, -1 = none
static int prev_turn;     // last resolved turn (-1 until first resolve)
static int prev_move;     // my move on prev_turn (for catch-up resends)
static int cmd_col, cmd_row;  // command menu cursor (2x2)
static int move_idx;          // move menu cursor (linear into 2-col grid)
static int last_move_idx;     // last move used this duel (re-highlight next turn)
static uint32_t last_send;

#define MOVE_PP_COLOR lv_color_hex(0xB8B8B8)
#define MOVE_SEL_COLOR lv_color_hex(0xFFE45E)
#define MOVE_EMPTY_COLOR lv_color_hex(0x707070)

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
  int power;          // move base power (0 = non-attacking)
} strike_t;
static strike_t strikes[2];
static int n_strikes, strike_i;
// Subphase: USED streams, USED_HOLD waits 2s, EFF streams the outcome,
// EFF_HOLD waits 2s, NEUTRAL_HOLD waits 2s with no line, POST_TURN is
// optional "grew to Lv.N!" after the last strike, SINGLE is a one-off
// line (errors) that falls back to the command menu.
typedef enum {
  SP_USED,
  SP_HURT,          // defender hit reaction (twitch + flash)
  SP_FAINT,         // slide off + fade when HP hits 0
  SP_EFF,
  SP_NEUTRAL_HOLD,
  SP_POST_TURN,
  SP_SINGLE
} strike_phase_t;
static strike_phase_t strike_phase;
static uint32_t neutral_until;
static bool over_pending, over_won;
// FireRed-style level-up line after the KO (exp applied in resolve_turn).
static char post_turn_msg[160];

// ---- LVGL objects ---------------------------------------------------
static lv_obj_t *foe_name_label;
static lv_obj_t *foe_level_label;
static lv_obj_t *foe_hp_label;
static lv_obj_t *foe_bar;
static lv_obj_t *me_name_label;
static lv_obj_t *me_level_label;
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
static lv_obj_t *move_pp_labels[MAX_MOVES];
static lv_obj_t *mv_cursor;

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
  lv_obj_set_width(hp_l, PARTY2_HP_W);
  lv_obj_set_style_text_align(hp_l, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
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
  // Bottom white dialog bar prompt (dark ink) + purple CANCEL tab
  // (white). No cursor is drawn anywhere on this screen.
  lv_obj_t *hint = lv_label_create(scr);
  lv_obj_set_style_text_font(hint, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(hint, PLATE_INK, LV_PART_MAIN);
  lv_obj_set_pos(hint, PARTY2_DLG_X, PARTY2_DLG_Y);
  lv_obj_set_width(hint, PARTY2_DLG_W);
  lv_label_set_text(hint, "Choose a Badgemon.");
  party_track(hint);
  lv_obj_t *cancel = lv_label_create(scr);
  lv_obj_set_style_text_font(cancel, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(cancel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_pos(cancel, PARTY2_CANCEL_X, PARTY2_CANCEL_Y);
  lv_obj_set_width(cancel, PARTY2_CANCEL_W);
  lv_obj_set_style_text_align(cancel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(cancel, "CANCEL");
  party_track(cancel);
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

// My HP number tweens down (or up) in step with the bar fill instead of
// snapping. HP_NUM_ANIM_MS matches the bar's anim_time (see make_hp_bar).
#define HP_NUM_ANIM_MS 600
static int hp_shown;         // number currently on screen
static int hp_num_from;      // tween start value
static int hp_num_to;        // tween target value
static uint32_t hp_num_start;  // tween start time

// LEDs flash the health colour brightly until this time after I take a
// hit (0 = not flashing).
static uint32_t dmg_flash_until;

static void set_me_hp_label(int hp, int max_hp) {
  if (!me_hp_label) return;
  char buf[24];
  snprintf(buf, sizeof(buf), "%d/%d", hp, max_hp);
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(me_hp_label, buf);
  lvgl_port_unlock();
}

// Advance the my-HP number toward its target each tick (matches bar).
static void hp_num_tick(uint32_t now) {
  if (!me_hp_label || hp_shown == hp_num_to) return;
  uint32_t el = now - hp_num_start;
  int v;
  if (el >= HP_NUM_ANIM_MS) {
    v = hp_num_to;
  } else {
    v = hp_num_from + (int)((long)(hp_num_to - hp_num_from) * (long)el /
                            HP_NUM_ANIM_MS);
  }
  if (v != hp_shown) {
    hp_shown = v;
    set_me_hp_label(hp_shown, mons[me_idx].max_health);
  }
}

static void set_name_only(lv_obj_t *label, const pokemon_t *p) {
  if (!label || !p) return;
  lv_label_set_text(label, p->name);
}

static void set_level_plate(lv_obj_t *label, const pokemon_t *p) {
  if (!label || !p) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "Lv.%d", p->level);
  lv_label_set_text(label, buf);
}

static void redraw_hp(void) {
  if (!foe_hp_label) return;  // screen not built yet (enter failed to lock)
  pokemon_t *mp = &mons[me_idx];
  if (!lvgl_port_lock(0)) return;
  // Foe HP numbers stay hidden (bar only); keep the label blank.
  lv_label_set_text(foe_hp_label, "");
  set_name_only(foe_name_label, &mons[opp_idx]);
  set_level_plate(foe_level_label, &mons[opp_idx]);
  set_name_only(me_name_label, mp);
  set_level_plate(me_level_label, mp);
  lvgl_port_unlock();
  lv_anim_enable_t anim = bars_init ? LV_ANIM_ON : LV_ANIM_OFF;
  // My HP number: snap on the first draw, otherwise tween to the new
  // value over HP_NUM_ANIM_MS so it counts down with the bar.
  if (!bars_init) {
    hp_shown = mp->health;
    hp_num_from = hp_num_to = mp->health;
    hp_num_start = duel_now;
    set_me_hp_label(hp_shown, mp->max_health);
  } else if (mp->health != hp_num_to) {
    hp_num_from = hp_shown;
    hp_num_to = mp->health;
    hp_num_start = duel_now;
  }
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
  for (int i = 0; i < MAX_MOVES; i++) {
    lv_obj_set_flag(move_labels[i], LV_OBJ_FLAG_HIDDEN, !moves);
    if (move_pp_labels[i])
      lv_obj_set_flag(move_pp_labels[i], LV_OBJ_FLAG_HIDDEN, !moves);
  }
  lv_obj_set_flag(mv_cursor, LV_OBJ_FLAG_HIDDEN, !moves);
  lvgl_port_unlock();
}

static int move_max_pp(species_id_t sp, int move_i) {
  const pokemon_t *t = species_template(sp);
  if (!t || move_i < 0 || move_i >= t->move_count) return 0;
  return t->moves[move_i].pp;
}

static void mon_restore_move_pp(pokemon_t *p, species_id_t sp) {
  const pokemon_t *t = species_template(sp);
  if (!p || !t) return;
  for (int i = 0; i < p->move_count && i < MAX_MOVES; i++)
    p->moves[i].pp = t->moves[i].pp;
}

static bool move_usable(int idx) {
  pokemon_t *mp = &mons[me_idx];
  return idx >= 0 && idx < mp->move_count && mp->moves[idx].pp > 0;
}

static void redraw_mv_cursor(void);

static int move_pick_default(void) {
  pokemon_t *mp = &mons[me_idx];
  if (last_move_idx >= 0 && last_move_idx < mp->move_count &&
      mp->moves[last_move_idx].pp > 0)
    return last_move_idx;
  for (int i = 0; i < mp->move_count; i++)
    if (mp->moves[i].pp > 0) return i;
  return 0;
}

static void move_list_redraw(void) {
  pokemon_t *mp = &mons[me_idx];
  if (!lvgl_port_lock(0)) return;
  for (int i = 0; i < MAX_MOVES; i++) {
    int mx = (i % 2) == 0 ? DUEL_MV_COL_X0 : DUEL_MV_COL_X1;
    int my = (i / 2) == 0 ? DUEL_MV_ROW_Y0 : DUEL_MV_ROW_Y1;
    if (i >= mp->move_count) {
      lv_obj_set_flag(move_labels[i], LV_OBJ_FLAG_HIDDEN, true);
      if (move_pp_labels[i])
        lv_obj_set_flag(move_pp_labels[i], LV_OBJ_FLAG_HIDDEN, true);
      continue;
    }
    lv_obj_set_flag(move_labels[i], LV_OBJ_FLAG_HIDDEN, false);
    lv_label_set_text(move_labels[i], mp->moves[i].name);
    bool empty = mp->moves[i].pp <= 0;
    lv_color_t name_c =
        empty ? MOVE_EMPTY_COLOR
              : (i == move_idx ? MOVE_SEL_COLOR : lv_color_white());
    lv_obj_set_style_text_color(move_labels[i], name_c, LV_PART_MAIN);
    if (move_pp_labels[i]) {
      lv_obj_set_flag(move_pp_labels[i], LV_OBJ_FLAG_HIDDEN, false);
      char pp[16];
      int max = move_max_pp(my_species, i);
      snprintf(pp, sizeof(pp), "%d/%d", mp->moves[i].pp, max);
      lv_label_set_text(move_pp_labels[i], pp);
      lv_obj_set_style_text_color(move_pp_labels[i], MOVE_PP_COLOR,
                                  LV_PART_MAIN);
      lv_obj_update_layout(move_labels[i]);
      int32_t name_w = lv_obj_get_width(move_labels[i]);
      lv_obj_set_pos(move_pp_labels[i], mx + name_w + DUEL_MV_PP_GAP, my);
    }
  }
  lvgl_port_unlock();
  redraw_mv_cursor();
}

// ---- LEDs -----------------------------------------------------------
// Map my HP fraction onto the same green/orange/red thresholds the HP
// bar uses, scaled to a modest LED brightness (AA power).
static void hp_led_rgb(int hp, int max_hp, uint8_t scale, uint8_t *r,
                       uint8_t *g, uint8_t *b) {
  uint32_t hex;
  if (max_hp <= 0 || hp * 4 < max_hp)
    hex = 0xD83828;
  else if (hp * 2 < max_hp)
    hex = 0xE8A020;
  else
    hex = 0x38B838;
  *r = (uint8_t)(((hex >> 16) & 0xFF) * scale / 255);
  *g = (uint8_t)(((hex >> 8) & 0xFF) * scale / 255);
  *b = (uint8_t)((hex & 0xFF) * scale / 255);
}

// 8-bit color wheel (pos 0..255) at a capped brightness, for the win
// rainbow chase.
static void led_wheel(uint8_t pos, uint8_t bright, uint8_t *r, uint8_t *g,
                      uint8_t *b) {
  uint8_t rr, gg, bb;
  if (pos < 85) {
    rr = pos * 3;
    gg = 255 - pos * 3;
    bb = 0;
  } else if (pos < 170) {
    pos -= 85;
    rr = 255 - pos * 3;
    gg = 0;
    bb = pos * 3;
  } else {
    pos -= 170;
    rr = 0;
    gg = pos * 3;
    bb = 255 - pos * 3;
  }
  *r = (uint8_t)(rr * bright / 255);
  *g = (uint8_t)(gg * bright / 255);
  *b = (uint8_t)(bb * bright / 255);
}

// Victory: three evenly-spaced LEDs circulate around the 6-LED ring,
// each a different rainbow hue, with the whole wheel drifting over time.
static void win_leds(uint32_t now) {
  int pos = (int)((now / 120) % HAL_LED_COUNT);
  uint8_t base = (uint8_t)((now / 12) & 0xFF);
  for (int i = 0; i < HAL_LED_COUNT; i++) hal_led_set_one(i, 0, 0, 0);
  for (int k = 0; k < 3; k++) {
    int idx = (pos + k * 2) % HAL_LED_COUNT;
    uint8_t r, g, b;
    led_wheel((uint8_t)(base + k * 85), 34, &r, &g, &b);
    hal_led_set_one(idx, r, g, b);
  }
  hal_led_show();
}

// Central duel LED policy (called every tick):
//   over      -> win rainbow chase / steady red on a loss
//   otherwise -> reflect my HP bar colour, flashing brightly for a
//                couple seconds right after I take damage (not while
//                waiting on the opponent's move).
static void duel_leds_update(uint32_t now) {
  if (st == DS_OVER) {
    if (over_won)
      win_leds(now);
    else
      hal_led_set_all(18, 0, 0);
    return;
  }
  uint8_t r, g, b;
  if (st == DS_WAIT) {
    hp_led_rgb(mons[me_idx].health, mons[me_idx].max_health, 22, &r, &g, &b);
    hal_led_set_all(r, g, b);
    return;
  }
  if (now < dmg_flash_until) {
    bool on = ((now / 120) % 2) == 0;
    hp_led_rgb(mons[me_idx].health, mons[me_idx].max_health, on ? 65 : 4, &r,
               &g, &b);
  } else {
    hp_led_rgb(mons[me_idx].health, mons[me_idx].max_health, 22, &r, &g, &b);
  }
  hal_led_set_all(r, g, b);
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
  int x = (cmd_col == 0 ? DUEL_CMD_COL_X0 : DUEL_CMD_COL_X1) - DUEL_CMD_CUR_DX;
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
  // LEDs are driven centrally by duel_leds_update() each tick.
}

static void redraw_mv_cursor(void) {
  if (!mv_cursor) return;
  int col = move_idx % 2, row = move_idx / 2;
  int x = (col == 0 ? DUEL_MV_COL_X0 : DUEL_MV_COL_X1) - DUEL_MV_CUR_DX;
  int y = (row == 0 ? DUEL_MV_ROW_Y0 : DUEL_MV_ROW_Y1) - DUEL_CUR_DY;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_pos(mv_cursor, x, y);
  lvgl_port_unlock();
}

// Fill the move list; cursor starts on last move used (if it still has PP).
static void to_moves(void) {
  st = DS_SELECT;
  move_idx = move_pick_default();
  show_group(false, false, true);
  move_list_redraw();
}

// ---- Networking -----------------------------------------------------
static void pack_setup_snap(const pokemon_t *p, species_id_t sp, uint8_t *snap) {
  snap[0] = (uint8_t)sp;
  snap[1] = (uint8_t)p->level;
  memcpy(snap + 2, &p->exp, 4);
  uint16_t hp = (uint16_t)p->health;
  memcpy(snap + 6, &hp, 2);
}

static bool apply_opp_setup(const uint8_t *snap) {
  if (!species_id_valid(snap[0])) return false;
  species_id_t sp = (species_id_t)snap[0];
  int level = snap[1];
  uint32_t exp;
  uint16_t hp;
  memcpy(&exp, snap + 2, 4);
  memcpy(&hp, snap + 6, 2);
  pokemon_from_species(&mons[opp_idx], sp);
  pokemon_apply_progress(&mons[opp_idx], level, (int)exp, (int)hp);
  opp_species = sp;
  send_out_queue(&send_foe, opp_species);
  sprite_refresh_visibility();
  return mons[opp_idx].move_count > 0;
}

static esp_err_t send_setup(void) {
  uint8_t buf[DUEL_SETUP_VALS_LEN];
  memcpy(buf, opp_mac, 6);
  pack_setup_snap(&mons[me_idx], my_species, buf + 6);
  esp_err_t err = net_send(PKT_DUEL_SETUP, buf, sizeof(buf), NULL);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "send setup failed: %s", esp_err_to_name(err));
  return err;
}

static void duel_sync_done(void) {
  opp_synced = true;
  st = DS_COMMAND;
  redraw_hp();
  send_out_queue(&send_me, my_species);
  send_out_queue(&send_foe, opp_species);
  sprite_refresh_visibility();
  to_command();
  intro_start_bars();
  ESP_LOGI(TAG, "sync done — your move");
}

static void strike_flow_reset(void) {
  strike_lunge_started = false;
  strike_damage_applied = false;
}

static void strike_go_eff(void) {
  if (strikes[strike_i].eff[0]) {
    log_stream_start(strikes[strike_i].eff, false);
    strike_phase = SP_EFF;
  } else {
    neutral_until = duel_now + RECAP_HOLD_MS;
    strike_phase = SP_NEUTRAL_HOLD;
  }
}

static void strike_after_hit(void) {
  int def = strikes[strike_i].def;
  if (mons[def].health <= 0) {
    sprite_faint_start(def);
    strike_phase = SP_FAINT;
  } else {
    strike_go_eff();
  }
}

static void duel_save_and_exit(void) {
  game_commit_active(&mons[me_idx]);
  nav_show(SCR_PLAY);
}

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
    if (!for_me(m.vals)) continue;
    if (memcmp(m.mac, opp_mac, 6)) continue;
    if (m.type == PKT_DUEL_SETUP && m.len >= DUEL_SETUP_VALS_LEN &&
        st == DS_SYNC && !opp_synced) {
      if (apply_opp_setup(m.vals + 6)) duel_sync_done();
      continue;
    }
    if (m.type != PKT_DUEL || m.len < 8) continue;
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
  // make_attack fills level/Attack/STAB from the attacker so the Gen 3
  // damage formula scales with level.
  return make_attack(&mons[player], move_idx);
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
  // Flash the LEDs for ~2.5s when my own mon takes damage.
  if (s->def == me_idx && s->dmg > 0) dmg_flash_until = duel_now + 2500;
  redraw_hp();
}

// Past the last strike: optional level-up line, then over or commands.
static void strike_finish(void) {
  if (post_turn_msg[0]) {
    log_stream_start(post_turn_msg, false);
    post_turn_msg[0] = '\0';
    strike_phase = SP_POST_TURN;
    return;
  }
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
  strikes[0].power = a0.power;
  strncpy(strikes[0].move, a0.name, sizeof(strikes[0].move) - 1);
  strikes[0].move[sizeof(strikes[0].move) - 1] = '\0';
  eff_text(strikes[0].eff, sizeof(strikes[0].eff), e0, mons[1].name,
           faint1);
  n_strikes = 1;
  bool faint0 = false;
  if (!faint1) {
    float e1 = attack_multiplier(&a1, &mons[0]);
    int d1 = calculate_damage(&a1, &mons[0]);
    faint0 = mons[0].health - d1 <= 0;
    strikes[1].atk = 1;
    strikes[1].def = 0;
    strikes[1].dmg = d1;
    strikes[1].power = a1.power;
    strncpy(strikes[1].move, a1.name, sizeof(strikes[1].move) - 1);
    strikes[1].move[sizeof(strikes[1].move) - 1] = '\0';
    eff_text(strikes[1].eff, sizeof(strikes[1].eff), e1, mons[0].name,
             faint0);
    n_strikes = 2;
  }

  post_turn_msg[0] = '\0';
  // Award experience on KO (Gen 3). Uses precomputed faints because HP
  // is not applied until each strike animates.
  int loser = faint1 ? 1 : (faint0 ? 0 : -1);
  if (loser >= 0) {
    int winner = 1 - loser;
    int levels = pokemon_gain_exp(&mons[winner], exp_yield(&mons[loser]));
    if (levels > 0) {
      snprintf(post_turn_msg, sizeof(post_turn_msg), "%s grew to Lv.%d!",
               mons[winner].name, mons[winner].level);
    }
  }

  // Advance bookkeeping (remember this move for catch-up resends).
  prev_turn = turn;
  prev_move = my_move;
  turn++;
  my_move = -1;
  opp_move = -1;

  if (mv0 >= 0 && mv0 < mons[0].move_count && mons[0].moves[mv0].pp > 0)
    mons[0].moves[mv0].pp--;
  if (n_strikes == 2 && mv1 >= 0 && mv1 < mons[1].move_count &&
      mons[1].moves[mv1].pp > 0)
    mons[1].moves[mv1].pp--;

  over_pending = faint1 || faint0;
  over_won = (opp_idx == 1) ? faint1 : faint0;
  ESP_LOGI(TAG, "turn resolved, streaming strikes (n=%d)", n_strikes);

  strike_i = 0;
  strike_phase = SP_USED;
  strike_flow_reset();
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
  snprintf(over, sizeof(over), "%s", won ? "YOU WIN!" : "You lost!");
  show_group(true, false, false);
  log_stream_start(over, false);
  // Win = rotating rainbow chase, loss = steady red (duel_leds_update).
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
  me_idx = me_is_p0 ? 0 : 1;
  opp_idx = 1 - me_idx;
  my_species = game_active_species();
  opp_synced = false;
  CHECK(game_load_active(&mons[me_idx]), "game_load_active failed");
  CHECK(mons[me_idx].move_count > 0 && mons[me_idx].move_count <= MAX_MOVES,
        "%s has bad move_count %d", mons[me_idx].name, mons[me_idx].move_count);
  mon_restore_move_pp(&mons[me_idx], my_species);

  turn = 0;
  my_move = -1;
  opp_move = -1;
  prev_turn = -1;
  prev_move = -1;
  cmd_col = 0;
  cmd_row = 0;
  move_idx = 0;
  last_move_idx = -1;
  last_send = 0;
  duel_now = 0;
  stream_full[0] = '\0';
  stream_len = stream_pos = 0;
  stream_last = stream_done_at = 0;
  n_strikes = strike_i = 0;
  strike_phase = SP_SINGLE;
  neutral_until = 0;
  over_pending = over_won = false;
  post_turn_msg[0] = '\0';
  bars_init = false;
  hp_shown = hp_num_from = hp_num_to = 0;
  hp_num_start = 0;
  dmg_flash_until = 0;
  opp_species = my_species;
  me_sprite_fainted = foe_sprite_fainted = false;
  me_idle_frame = foe_idle_frame = 0;
  sanim = SANIM_NONE;
  sanim_obj = NULL;
  memset(&send_me, 0, sizeof(send_me));
  memset(&send_foe, 0, sizeof(send_foe));
  strike_flow_reset();
  intro_top = intro_bot = NULL;
  intro_done = true;
  n_party_objs = 0;
  for (int i = 0; i < (int)(sizeof(party_objs) / sizeof(party_objs[0])); i++)
    party_objs[i] = NULL;
  st = DS_SYNC;

  ESP_LOGI(TAG, "enter vs '%s' as p%d (%s)", opp_name, me_is_p0 ? 0 : 1,
           mons[me_idx].name);

  // Drop any stale handles up front: if we fail to build the screen the
  // redraw_* guards must see NULL, not pointers to freed LVGL objects.
  foe_name_label = foe_level_label = foe_hp_label = foe_bar = NULL;
  me_name_label = me_level_label = me_hp_label = me_bar = NULL;
  me_sprite = foe_sprite = me_ball = foe_ball = NULL;
  cap_full_img = log_label = NULL;
  cap_half_img = prompt_label = NULL;
  speech_img = cmd_cursor = mv_cursor = NULL;
  intro_top = intro_bot = NULL;
  for (int c = 0; c < 2; c++)
    for (int r = 0; r < 2; r++) opt_labels[c][r] = NULL;
  for (int i = 0; i < MAX_MOVES; i++) {
    move_labels[i] = NULL;
    move_pp_labels[i] = NULL;
  }

  if (!lvgl_port_lock(0)) {
    ESP_LOGE(TAG, "could not lock LVGL on enter; screen not built");
    return;
  }
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();

  lv_obj_t *bg = lv_image_create(scr);
  lv_image_set_src(bg, &assets_duel_bg);
  lv_obj_set_pos(bg, 0, 0);

  me_sprite = lv_image_create(scr);
  lv_obj_add_flag(me_sprite, LV_OBJ_FLAG_HIDDEN);
  foe_sprite = lv_image_create(scr);
  lv_obj_add_flag(foe_sprite, LV_OBJ_FLAG_HIDDEN);
  me_ball = lv_image_create(scr);
  lv_image_set_src(me_ball, &assets_pokeball);
  lv_obj_add_flag(me_ball, LV_OBJ_FLAG_HIDDEN);
  foe_ball = lv_image_create(scr);
  lv_image_set_src(foe_ball, &assets_pokeball_foe);
  lv_obj_add_flag(foe_ball, LV_OBJ_FLAG_HIDDEN);
  me_sprite_layout(0, 0, 0);

  // Names are static for the duel; HP numbers + bars redraw every turn.
  // Foe HP numbers are hidden (bar only).
  foe_name_label = make_plate_label(scr, DUEL_FOE_NAME_X, DUEL_FOE_NAME_Y);
  lv_label_set_text(foe_name_label, "???");
  foe_level_label = make_plate_label(scr, DUEL_FOE_LEVEL_X, DUEL_FOE_LEVEL_Y);
  lv_obj_set_width(foe_level_label, DUEL_FOE_LEVEL_W);
  lv_obj_set_style_text_align(foe_level_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_label_set_text(foe_level_label, "");
  foe_hp_label = make_plate_label(scr, DUEL_FOE_HP_X, DUEL_FOE_HP_Y);
  lv_label_set_text(foe_hp_label, "");
  lv_obj_add_flag(foe_hp_label, LV_OBJ_FLAG_HIDDEN);
  foe_bar = make_hp_bar(scr, DUEL_FOE_BAR_X, DUEL_FOE_BAR_Y,
                        DUEL_FOE_BAR_W, DUEL_FOE_BAR_H);

  me_name_label = make_plate_label(scr, DUEL_ME_NAME_X, DUEL_ME_NAME_Y);
  set_name_only(me_name_label, &mons[me_idx]);
  me_level_label = make_plate_label(scr, DUEL_ME_LEVEL_X, DUEL_ME_LEVEL_Y);
  lv_obj_set_width(me_level_label, DUEL_ME_LEVEL_W);
  lv_obj_set_style_text_align(me_level_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  set_level_plate(me_level_label, &mons[me_idx]);
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
    int mx = (i % 2) == 0 ? DUEL_MV_COL_X0 : DUEL_MV_COL_X1;
    int my = (i / 2) == 0 ? DUEL_MV_ROW_Y0 : DUEL_MV_ROW_Y1;
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, BADGE_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_pos(l, mx, my);
    move_labels[i] = l;
    lv_obj_t *pp = lv_label_create(scr);
    lv_obj_set_style_text_font(pp, BADGE_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(pp, MOVE_PP_COLOR, LV_PART_MAIN);
    lv_obj_set_pos(pp, mx + DUEL_MV_PP_GAP, my);
    move_pp_labels[i] = pp;
  }
  mv_cursor = lv_image_create(scr);
  lv_image_set_src(mv_cursor, &assets_cursor_white_sm);
  lvgl_port_unlock();

  redraw_hp();
  intro_done = true;  // intro runs after sync; send-out waits for wipe
  show_group(true, false, false);
  log_stream_start("Syncing...", false);
  last_send = 0;
  send_setup();
}

void ui_duel_tick(uint32_t now, const btn_event_t *ev) {
  duel_now = now;
  drain_net();
  hp_num_tick(now);  // count the HP number down/up with the bar
  sprite_battle_tick(now);
  if (st == DS_SYNC) {
    if (!opp_synced && now - last_send > RESEND_MS) {
      last_send = now;
      send_setup();
    }
    return;
  }
  duel_leds_update(now);  // central LED policy (health colour / flashes)
  intro_update(now);
  if (!intro_done) return;  // wipe plays out before any input

  // Resolve as soon as both moves are in (from net or from my pick).
  if ((st == DS_SELECT || st == DS_WAIT) && my_move >= 0 && opp_move >= 0) {
    resolve_turn();
    return;
  }

  switch (st) {
    case DS_SYNC:
      break;
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
          if (!stream_busy()) {
            if (!strike_lunge_started) {
              strike_lunge_started = true;
              strike_t *s = &strikes[strike_i];
              if (s->atk == opp_idx && s->power > 0) sprite_lunge_foe_start();
            }
            if (!strike_damage_applied &&
                now - stream_done_at >= RECAP_HOLD_MS && !sprite_anim_busy()) {
              strike_damage_applied = true;
              strike_apply();
              if (strikes[strike_i].dmg > 0) {
                sprite_hurt_start(strikes[strike_i].def);
                strike_phase = SP_HURT;
              } else {
                strike_after_hit();
              }
            }
          }
          break;
        case SP_HURT:
          if (!sprite_anim_busy()) strike_after_hit();
          break;
        case SP_FAINT:
          if (!sprite_anim_busy()) strike_go_eff();
          break;
        case SP_EFF:
          // Outcome fully streamed: hold 2s, then continue.
          if (now - stream_done_at >= RECAP_HOLD_MS) adv = true;
          break;
        case SP_NEUTRAL_HOLD:
          if (now >= neutral_until) adv = true;
          break;
        case SP_POST_TURN:
          if (now - stream_done_at >= RECAP_HOLD_MS) {
            if (over_pending)
              show_over();
            else
              to_command();
          }
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
          strike_flow_reset();
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
        move_list_redraw();
      }
      if (ev->up || ev->down) {
        int col = move_idx % 2, row = (move_idx / 2) ^ 1;
        if (row * 2 + col < count) move_idx = row * 2 + col;
        move_list_redraw();
      }
      if (ev->a) {
        if (!move_usable(move_idx)) break;
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
        last_move_idx = move_idx;
        last_send = now;
        st = DS_WAIT;
        show_group(true, false, false);
        status_show("Waiting for your opponent's turn...", false);
        // Opponent may already have sent; resolve next tick via the
        // check at the top.
      }
      if (ev->b) to_command();  // back out of FIGHT
      break;
    }
    case DS_WAIT:
      stream_pump(now);
      // LEDs reflect my health colour here (duel_leds_update).
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
          duel_save_and_exit();
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
  duel_save_and_exit();
  return true;
}

static const char *state_name(duel_state_t s) {
  switch (s) {
    case DS_SYNC: return "sync";
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
  snprintf(out, cap,
           "duel vs='%s' st=%s turn=%u me=p%d(%s L%d) hp=%d/%d exp=%d/+%d "
           "opp=%s L%d %d/%d cur=%d,%d/%d",
           opp_name, state_name(st), turn, me_is_p0 ? 0 : 1, mons[me_idx].name,
           mons[me_idx].level, mons[me_idx].health, mons[me_idx].max_health,
           mons[me_idx].exp, exp_to_next_level(&mons[me_idx]),
           mons[opp_idx].name, mons[opp_idx].level, mons[opp_idx].health,
           mons[opp_idx].max_health, cmd_col, cmd_row, move_idx);
}
