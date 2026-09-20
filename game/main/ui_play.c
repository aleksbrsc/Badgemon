#include "ui_play.h"
#include "ui_duel.h"
#include "nav.h"
#include "hal_led.h"
#include "hal_display.h"
#include "hal_accel.h"
#include "lobby.h"
#include "game.h"
#include "pokemon_data.h"
#include "ui_font.h"
#include "assets.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "play";

#define WAIT_TIMEOUT_MS 10000
#define RESULT_MS 2500
#define POLL_MS 1000
#define DOTS_MS 400
// Wild encounter: FAST motion held continuously for 3 s in the lobby.
#define WILD_HIGH_HOLD_MS 1000
#define WILD_COOLDOWN_MS 4000
#define PLAY_MOTION_Y 182        // above Waiting / TRAINER SPOTTED (PARTY_DLG_Y)
// Peer rows: plain text on the teal screen, 3 visible with a scroll
// window. Status shows the Waiting pulse; the challenge dialog uses
// the speech bubble frame.
#define PLAY_NAME_CHARS 12
#define ROW_INK lv_color_hex(0x1F353C)

typedef enum { ST_BROWSE, ST_WAIT, ST_DIALOG, ST_RESULT } play_state_t;

static play_state_t st = ST_BROWSE;
static lobby_peer_t peers[LOBBY_MAX_PEERS];
static int n_peers = 0;
static int cursor = 0;  // index into peers (no refresh row anymore)
static lv_obj_t *you_name_label;
static lv_obj_t *row_names[PLAY_ROWS_SHOWN];
static lv_obj_t *row_cursor;
static lv_obj_t *bubble_img;
static lv_obj_t *bubble_label;
static lv_obj_t *status_label;
static lv_obj_t *motion_label;
static uint32_t wild_high_since;
static uint8_t wait_mac[6];
static char wait_name[LOBBY_NAME_MAX + 1];
static uint32_t wait_since = 0;
static uint8_t dlg_mac[6];
static char dlg_name[LOBBY_NAME_MAX + 1];
static char result_text[64];
static uint32_t result_until = 0;
static int blink_div = 0;
static accel_shake_t wild_shake;  // lobby shake-to-encounter detector

static uint32_t now_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

// Dark info vs red error status on the baked dialog bar (errors also go
// to the log with context).
static void status_show(const char *s, bool is_err) {
  ESP_LOGI(TAG, "status: %s", s);
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_style_text_font(status_label, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_label_set_text(status_label, s);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(status_label,
                              is_err ? lv_color_hex(0xFF7070) : lv_color_white(),
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

static void you_refresh(void) {
  if (!lvgl_port_lock(0)) return;
  char me[LOBBY_NAME_MAX + 1];
  lobby_myname(me, sizeof(me));
  char t[32];
  snprintf(t, sizeof(t), "username: %s", me);
  lv_label_set_text(you_name_label, t);
  lv_obj_set_style_text_align(you_name_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lvgl_port_unlock();
}

// "Waiting." / "Waiting.." / "Waiting..." pulse. Direct label write
// (no ESP_LOG spam like status_show).
static void waiting_show(uint32_t now) {
  if (!status_label) return;
  int dots = (int)((now / DOTS_MS) % 3) + 1;
  char t[16];
  snprintf(t, sizeof(t), "Waiting%.*s", dots, "...");
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_style_text_font(status_label, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_label_set_text(status_label, t);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(status_label, lv_color_white(), LV_PART_MAIN);
  lvgl_port_unlock();
}

static void motion_bar_show(accel_motion_t level) {
  if (!motion_label) return;
  char buf[56];
  if (!hal_accel_ok()) {
    snprintf(buf, sizeof(buf), "[ accel offline ]");
  } else {
    snprintf(buf, sizeof(buf), "[ %s ] [ %s ] [ %s ]",
             level == ACCEL_MOTION_SLOW ? "SLOW" : "slow",
             level == ACCEL_MOTION_MEDIUM ? "MEDIUM" : "medium",
             level == ACCEL_MOTION_FAST ? "FAST" : "fast");
  }
  lv_color_t c = lv_color_hex(0x606060);
  if (level == ACCEL_MOTION_SLOW) c = lv_color_hex(0xA8A8A8);
  else if (level == ACCEL_MOTION_MEDIUM) c = lv_color_white();
  else if (level == ACCEL_MOTION_FAST) c = lv_color_hex(0xFFE45E);
  else if (!hal_accel_ok()) c = lv_color_hex(0xFF7070);
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(motion_label, buf);
  lv_obj_set_style_text_color(motion_label, c, LV_PART_MAIN);
  lvgl_port_unlock();
}

static void trainer_spotted_show(void) {
  if (!status_label) return;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_style_text_font(status_label, BADGE_FONT, LV_PART_MAIN);
  lv_label_set_text(status_label, "TRAINER SPOTTED!");
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(status_label, lv_color_white(), LV_PART_MAIN);
  lvgl_port_unlock();
}

static void list_redraw(void) {
  int rows = n_peers;
  int ws = cursor - 1;     // first visible absolute row (cursor centered)
  if (ws < 0) ws = 0;
  if (ws > rows - PLAY_ROWS_SHOWN) ws = rows - PLAY_ROWS_SHOWN;
  if (ws < 0) ws = 0;
  if (!lvgl_port_lock(0)) return;
  for (int v = 0; v < PLAY_ROWS_SHOWN; v++) {
    int r = ws + v;
    if (r >= rows) {
      lv_obj_set_flag(row_names[v], LV_OBJ_FLAG_HIDDEN, true);
      continue;
    }
    lv_obj_set_flag(row_names[v], LV_OBJ_FLAG_HIDDEN, false);
    char nm[PLAY_NAME_CHARS + 1];
    strncpy(nm, peers[r].name, PLAY_NAME_CHARS);
    nm[PLAY_NAME_CHARS] = '\0';
    lv_label_set_text(row_names[v], nm);
    lv_obj_set_style_text_color(row_names[v],
                                r == cursor ? lv_color_hex(0xFFE45E) : lv_color_white(),
                                LV_PART_MAIN);
  }
  // No peers: hide the cursor entirely.
  lv_obj_set_flag(row_cursor, LV_OBJ_FLAG_HIDDEN, rows == 0);
  lv_obj_set_pos(row_cursor, PLAY_CURSOR_X,
                 PLAY_ROWS_Y + (cursor - ws) * PLAY_ROW_PITCH + 5);
  lvgl_port_unlock();
}

static void show_dialog(bool on) {
  if (!lvgl_port_lock(0)) return;
  for (int v = 0; v < PLAY_ROWS_SHOWN; v++) {
    lv_obj_set_flag(row_names[v], LV_OBJ_FLAG_HIDDEN, on);
  }
  lv_obj_set_flag(row_cursor, LV_OBJ_FLAG_HIDDEN, on);
  lv_obj_set_flag(bubble_img, LV_OBJ_FLAG_HIDDEN, !on);
  lv_obj_set_flag(bubble_label, LV_OBJ_FLAG_HIDDEN, !on);
  if (motion_label)
    lv_obj_set_flag(motion_label, LV_OBJ_FLAG_HIDDEN, on);
  lvgl_port_unlock();
}

static void show_browse(void) {
  st = ST_BROWSE;
  cursor = 0;
  n_peers = lobby_list(peers, LOBBY_MAX_PEERS);
  show_dialog(false);
  list_redraw();
  hal_led_set_all(0, 0, 8);
}

void ui_play_enter(void) {
  ESP_LOGI(TAG, "enter");
  if (!lvgl_port_lock(0)) return;
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();

  // Text-only lobby on the plain teal screen (no party backdrop art,
  // no slot rows, no decorative HP bar).
  // Username row: my name as plain text.
  you_name_label = lv_label_create(scr);
  lv_obj_set_style_text_font(you_name_label, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(you_name_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_pos(you_name_label, 0, PARTY_YOU_NAME_Y);
  lv_obj_set_width(you_name_label, 320);
  lv_obj_set_style_text_align(you_name_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

  for (int v = 0; v < PLAY_ROWS_SHOWN; v++) {
    int y = PLAY_ROWS_Y + v * PLAY_ROW_PITCH;
    row_names[v] = lv_label_create(scr);
    lv_obj_set_style_text_font(row_names[v], BADGE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(row_names[v], lv_color_white(), LV_PART_MAIN);
    lv_obj_set_pos(row_names[v], PLAY_NAME_X, y + 8);
    lv_obj_set_width(row_names[v], PLAY_NAME_W);
  }
  row_cursor = lv_image_create(scr);
  lv_image_set_src(row_cursor, &assets_cursor_white);

  bubble_img = lv_image_create(scr);
  lv_image_set_src(bubble_img, &assets_bubble);
  lv_obj_set_pos(bubble_img, PLAY_BUBBLE_X, PLAY_BUBBLE_Y);
  bubble_label = lv_label_create(scr);
  lv_obj_set_style_text_font(bubble_label, BADGE_FONT, LV_PART_MAIN);
  lv_obj_set_style_text_color(bubble_label, ROW_INK, LV_PART_MAIN);
  lv_obj_set_pos(bubble_label, PLAY_BUBBLE_TEXT_X, PLAY_BUBBLE_TEXT_Y);
  lv_obj_set_width(bubble_label, PLAY_BUBBLE_TEXT_W);
  lv_label_set_long_mode(bubble_label, LV_LABEL_LONG_WRAP);

  motion_label = lv_label_create(scr);
  lv_obj_set_style_text_font(motion_label, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(motion_label, lv_color_hex(0x606060), LV_PART_MAIN);
  lv_obj_set_pos(motion_label, 0, PLAY_MOTION_Y);
  lv_obj_set_width(motion_label, 320);
  lv_obj_set_style_text_align(motion_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(motion_label, "[ slow ] [ medium ] [ fast ]");

  status_label = lv_label_create(scr);
  lv_obj_set_style_text_font(status_label, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(status_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_pos(status_label, 0, PARTY_DLG_Y);
  lv_obj_set_width(status_label, 320);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lvgl_port_unlock();
  wild_high_since = 0;
  memset(&wild_shake, 0, sizeof(wild_shake));
  show_browse();
  you_refresh();
  lobby_refresh();  // kick off discovery; tick polls every POLL_MS
}

static void to_wait(const lobby_peer_t *p) {
  esp_err_t err = lobby_challenge(p->mac);
  if (err != ESP_OK) {
    status_net_err("challenge", err);  // stay in browse so user can retry
    return;
  }
  memcpy(wait_mac, p->mac, 6);
  strncpy(wait_name, p->name, sizeof(wait_name) - 1);
  wait_name[sizeof(wait_name) - 1] = '\0';
  wait_since = now_ms();
  st = ST_WAIT;
  char s[64];
  snprintf(s, sizeof(s), "challenged %s...", p->name);
  status_show(s, false);
}

static void to_dialog(const lobby_event_t *ev) {
  memcpy(dlg_mac, ev->mac, 6);
  strncpy(dlg_name, ev->name, sizeof(dlg_name) - 1);
  dlg_name[sizeof(dlg_name) - 1] = '\0';
  st = ST_DIALOG;
  show_dialog(true);
  if (!lvgl_port_lock(0)) return;
  char buf[96];
  snprintf(buf, sizeof(buf), "%s\nwants to play!\nA = yes\nB = no", ev->name);
  lv_label_set_text(bubble_label, buf);
  lvgl_port_unlock();
  hal_led_set_all(0, 24, 0);
}

static void to_result(const char *fmt, const char *name, bool is_err) {
  snprintf(result_text, sizeof(result_text), fmt, name);
  result_until = now_ms() + RESULT_MS;
  st = ST_RESULT;
  status_show(result_text, is_err);
}

void ui_play_tick(uint32_t now_ms, const btn_event_t *ev) {
  lobby_tick();

  // Incoming events first.
  lobby_event_t evt;
  while (lobby_event(&evt)) {
    if (!evt.is_response) {
      if (st == ST_BROWSE) {
        to_dialog(&evt);
      } else {
        ESP_LOGI(TAG, "busy, ignoring challenge from '%s'", evt.name);
      }
    } else if (st == ST_WAIT && !memcmp(evt.mac, wait_mac, 6)) {
      if (evt.accept) {
        // Challenge accepted -> start the duel.
        ui_duel_set_opponent(evt.mac, evt.name);
        nav_show(SCR_DUEL);
        return;
      } else {
        to_result("%s declined", evt.name, false);
        hal_led_set_all(24, 0, 0);
      }
    }
  }

  switch (st) {
    case ST_BROWSE: {
      int act_mg = 0;
      accel_motion_t mot = hal_accel_motion(&act_mg);
      motion_bar_show(mot);

      bool wild_go = false;
      if (hal_accel_ok()) {
        if (mot >= ACCEL_MOTION_FAST) {
          if (wild_high_since == 0) wild_high_since = now_ms;
          else if (now_ms - wild_high_since >= WILD_HIGH_HOLD_MS) {
            if (wild_shake.last_fire == 0 ||
                now_ms - wild_shake.last_fire >= WILD_COOLDOWN_MS)
              wild_go = true;
          }
        } else {
          wild_high_since = 0;
        }
      } else {
        wild_high_since = 0;
      }
      if (wild_go) {
        wild_high_since = 0;
        wild_shake.last_fire = now_ms ? now_ms : 1;
        int base = game_first_level();
        int lvl = base + ((int)(esp_random() % 3) - 1);
        if (lvl < MIN_LEVEL) lvl = MIN_LEVEL;
        if (lvl > MAX_LEVEL) lvl = MAX_LEVEL;
        ESP_LOGI(TAG, "wild encounter! goose L%d act=%d", lvl, act_mg);
        ui_duel_set_wild(SPECIES_GOOSE, lvl);
        nav_show(SCR_DUEL);
        return;
      }
      if (n_peers > 0) {
        if (ev->up) {
          cursor = (cursor + n_peers - 1) % n_peers;
          list_redraw();
        }
        if (ev->down) {
          cursor = (cursor + 1) % n_peers;
          list_redraw();
        }
        if (ev->a && cursor < n_peers) {
          to_wait(&peers[cursor]);
        }
      }
      // Poll for peers every second (presence trickles in); the
      // status line pulses Waiting. / Waiting.. / Waiting...
      static uint32_t last_poll = 0;
      if (now_ms - last_poll > POLL_MS) {
        last_poll = now_ms;
        lobby_refresh();
        int n = lobby_list(peers, LOBBY_MAX_PEERS);
        if (n != n_peers) {
          ESP_LOGD(TAG, "peer count %d -> %d", n_peers, n);
          if (n > n_peers) trainer_spotted_show();
          n_peers = n;
          if (cursor >= n_peers) cursor = n_peers > 0 ? n_peers - 1 : 0;
          list_redraw();
        }
      }
      if (n_peers > 0)
        trainer_spotted_show();
      else
        waiting_show(now_ms);
      if (++blink_div >= 25) {
        blink_div = 0;
        hal_led_set_all(0, 0, 8);
      }
      break;
    }
    case ST_WAIT:
      if (now_ms - wait_since > WAIT_TIMEOUT_MS) {
        to_result("%s: no answer", wait_name, true);
        hal_led_set_all(24, 0, 0);
      }
      break;
    case ST_DIALOG: {
      if (ev->a) {
        esp_err_t err = lobby_respond(dlg_mac, true);
        if (err != ESP_OK) {
          show_dialog(false);
          status_net_err("accept", err);
          show_browse();
        } else {
          // I accepted -> start the duel.
          ui_duel_set_opponent(dlg_mac, dlg_name);
          nav_show(SCR_DUEL);
          return;
        }
      } else if (ev->b) {
        esp_err_t err = lobby_respond(dlg_mac, false);
        if (err != ESP_OK) status_net_err("decline", err);
        show_browse();
      }
      break;
    }
    case ST_RESULT:
      if (now_ms >= result_until) show_browse();
      break;
  }
}

// Home handling: consumed locally while modal, else nav goes to menu.
bool ui_play_home(const btn_event_t *ev) {
  if (!ev->home) return false;
  if (st == ST_DIALOG) {
    ESP_LOGI(TAG, "dialog dismissed via Home (decline)");
    esp_err_t err = lobby_respond(dlg_mac, false);
    if (err != ESP_OK) status_net_err("decline", err);
    show_browse();
    return true;
  }
  if (st == ST_WAIT) {
    ESP_LOGI(TAG, "wait cancelled");
    show_browse();
    return true;
  }
  if (st == ST_RESULT) {
    show_browse();
    return true;
  }
  return false;  // BROWSE: nav goes to menu
}

static const char *state_name(play_state_t s) {
  switch (s) {
    case ST_BROWSE: return "browse";
    case ST_WAIT: return "wait";
    case ST_DIALOG: return "dialog";
    case ST_RESULT: return "result";
    default: return "?";
  }
}

void ui_play_debug(char *out, int cap) {
  if (!out || cap < 1) return;
  int n = snprintf(out, cap, "play st=%s cursor=%d/%d peers=%d [", state_name(st), cursor,
                   n_peers, n_peers);
  for (int i = 0; i < n_peers && n < cap - 4; i++)
    n += snprintf(out + n, (size_t)(cap - n), "%s%s", i ? " " : "", peers[i].name);
  if (st == ST_WAIT && n < cap - 40)
    n += snprintf(out + n, (size_t)(cap - n), "] wait='%s'", wait_name);
  else if (st == ST_DIALOG && n < cap - 40)
    n += snprintf(out + n, (size_t)(cap - n), "] dlg='%s'", dlg_name);
  else if (n < cap - 2)
    n += snprintf(out + n, (size_t)(cap - n), "]");
  (void)n;
}
