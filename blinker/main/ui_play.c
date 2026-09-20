#include "ui_play.h"
#include "ui_duel.h"
#include "nav.h"
#include "hal_led.h"
#include "hal_display.h"
#include "lobby.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "play";

#define WAIT_TIMEOUT_MS 10000
#define RESULT_MS 2500

typedef enum { ST_BROWSE, ST_WAIT, ST_DIALOG, ST_RESULT } play_state_t;

static play_state_t st = ST_BROWSE;
static lobby_peer_t peers[LOBBY_MAX_PEERS];
static int n_peers = 0;
static int cursor = 0;  // 0 = refresh row, 1..n = peer rows
static lv_obj_t *list_label;
static lv_obj_t *status_label;
static uint8_t wait_mac[6];
static char wait_name[LOBBY_NAME_MAX + 1];
static uint32_t wait_since = 0;
static uint8_t dlg_mac[6];
static char dlg_name[LOBBY_NAME_MAX + 1];
static char result_text[64];
static uint32_t result_until = 0;
static int blink_div = 0;

static uint32_t now_ms(void) { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

static void status_show(const char *s) {
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(status_label, s);
  lvgl_port_unlock();
}

static void list_redraw(void) {
  if (!lvgl_port_lock(0)) return;
  char buf[192];
  int n = snprintf(buf, sizeof(buf), "%s refresh\n", cursor == 0 ? ">" : " ");
  for (int i = 0; i < n_peers && n < (int)sizeof(buf) - 24; i++)
    n += snprintf(buf + n, sizeof(buf) - n, "%s %s\n", cursor == i + 1 ? ">" : " ",
                  peers[i].name);
  if (!n_peers) n += snprintf(buf + n, sizeof(buf) - n, "(no players yet)");
  lv_label_set_text(list_label, buf);
  lvgl_port_unlock();
}

static void show_browse(void) {
  st = ST_BROWSE;
  cursor = 0;
  n_peers = lobby_list(peers, LOBBY_MAX_PEERS);
  list_redraw();
  status_show("A: refresh / challenge");
  hal_led_set_all(0, 0, 8);
}

void ui_play_enter(void) {
  ESP_LOGI(TAG, "enter");
  if (!lvgl_port_lock(0)) return;
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "play");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
  list_label = lv_label_create(scr);
  lv_obj_set_style_text_font(list_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(list_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(list_label, LV_ALIGN_TOP_LEFT, 12, 72);
  status_label = lv_label_create(scr);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
  lv_obj_set_width(status_label, 300);
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(status_label, LV_ALIGN_BOTTOM_LEFT, 8, -26);
  lv_obj_t *foot = lv_label_create(scr);
  lv_label_set_text(foot, "up/dn move  A ok  Home back");
  lv_obj_set_style_text_font(foot, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(foot, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -6);
  lvgl_port_unlock();
  show_browse();
  lobby_refresh();  // auto-scan on open
  status_show("scanning...");
}

static void to_wait(const lobby_peer_t *p) {
  memcpy(wait_mac, p->mac, 6);
  strncpy(wait_name, p->name, sizeof(wait_name) - 1);
  wait_name[sizeof(wait_name) - 1] = '\0';
  wait_since = now_ms();
  st = ST_WAIT;
  lobby_challenge(p->mac);
  char s[64];
  snprintf(s, sizeof(s), "challenged %s...", p->name);
  status_show(s);
}

static void to_dialog(const lobby_event_t *ev) {
  memcpy(dlg_mac, ev->mac, 6);
  strncpy(dlg_name, ev->name, sizeof(dlg_name) - 1);
  dlg_name[sizeof(dlg_name) - 1] = '\0';
  st = ST_DIALOG;
  if (!lvgl_port_lock(0)) return;
  char buf[96];
  snprintf(buf, sizeof(buf), "> %s\n  wants to play!\n\n  A = yes   B = no", ev->name);
  lv_label_set_text(list_label, buf);
  lv_label_set_text(status_label, "answer!");
  lvgl_port_unlock();
  hal_led_set_all(0, 24, 0);
}

static void to_result(const char *fmt, const char *name) {
  snprintf(result_text, sizeof(result_text), fmt, name);
  result_until = now_ms() + RESULT_MS;
  st = ST_RESULT;
  status_show(result_text);
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
        to_result("%s declined", evt.name);
        hal_led_set_all(24, 0, 0);
      }
    }
  }

  switch (st) {
    case ST_BROWSE: {
      int rows = n_peers + 1;
      if (ev->up) {
        cursor = (cursor + rows - 1) % rows;
        list_redraw();
      }
      if (ev->down) {
        cursor = (cursor + 1) % rows;
        list_redraw();
      }
      if (ev->a) {
        if (cursor == 0) {
          lobby_refresh();
          status_show("scanning...");
        } else if (cursor - 1 < n_peers) {
          to_wait(&peers[cursor - 1]);
        }
      }
      // Refresh the list view periodically (presence trickles in).
      static uint32_t last_list = 0;
      if (now_ms - last_list > 1000) {
        last_list = now_ms;
        int n = lobby_list(peers, LOBBY_MAX_PEERS);
        if (n != n_peers) {
          n_peers = n;
          if (cursor > n_peers) cursor = n_peers;
          list_redraw();
        }
      }
      if (++blink_div >= 25) {
        blink_div = 0;
        hal_led_set_all(0, 0, 8);
      }
      break;
    }
    case ST_WAIT:
      // Amber pulse while waiting.
      if (++blink_div >= 5) {
        blink_div = 0;
        static bool on = false;
        on = !on;
        hal_led_set_all(on ? 24 : 0, on ? 12 : 0, 0);
      }
      if (now_ms - wait_since > WAIT_TIMEOUT_MS) {
        to_result("%s: no answer", wait_name);
        hal_led_set_all(24, 0, 0);
      }
      break;
    case ST_DIALOG:
      if (ev->a) {
        lobby_respond(dlg_mac, true);
        // I accepted -> start the duel.
        ui_duel_set_opponent(dlg_mac, dlg_name);
        nav_show(SCR_DUEL);
        return;
      } else if (ev->b) {
        lobby_respond(dlg_mac, false);
        to_result("declined %s", dlg_name);
        show_browse();
      }
      break;
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
    lobby_respond(dlg_mac, false);
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
