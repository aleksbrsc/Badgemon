#include "nav.h"
#include "ui_menu.h"
#include "ui_play.h"
#include "ui_duel.h"
#include "ui_settings.h"
#include "ui_keyboard.h"
#include "store.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "nav";
static screen_t current = SCR_MENU;

// Main menu built on the reusable ui_menu: re-opened on every show so
// the greeting is always fresh. (ping/message screens still exist in
// the build but are hidden from the menu.)
static const char *main_items[] = {"play", "edit name"};

static void on_name_done(const char *text, void *ctx) {
  (void)ctx;
  if (text) store_set_name(text);
  nav_show(SCR_MENU);
}

static void on_main_pick(int index, void *ctx) {
  (void)ctx;
  if (index == 0) {
    nav_show(SCR_PLAY);
    return;
  }
  char cur[STORE_NAME_MAX + 1];
  store_get_name(cur, sizeof(cur));
  ui_keyboard_start(cur, STORE_NAME_MAX, on_name_done, NULL);
}

static void menu_enter_main(void) {
  ui_menu_open("", "", main_items, 2, on_main_pick, NULL, NULL);
}

void nav_show(screen_t s) {
  ESP_LOGI(TAG, "show %d (%s)", (int)s, nav_name(s));
  current = s;
  switch (s) {
    case SCR_MENU: menu_enter_main(); break;
    case SCR_PLAY: ui_play_enter(); break;
    case SCR_DUEL: ui_duel_enter(); break;
    case SCR_MSG: break;  // hidden (kept in build)
    case SCR_SETTINGS: ui_settings_enter(); break;
  }
}

void nav_tick(uint32_t now_ms, const btn_event_t *ev) {
  switch (current) {
    case SCR_MENU:
      if (ui_keyboard_active()) {
        ui_keyboard_tick(now_ms, ev);
        break;  // keyboard owns input until Start/Home
      }
      ui_menu_tick(now_ms, ev);
      break;
    case SCR_PLAY:
      ui_play_tick(now_ms, ev);
      if (ev->home && !ui_play_home(ev)) nav_show(SCR_MENU);
      break;
    case SCR_DUEL:
      ui_duel_tick(now_ms, ev);
      // Home always handled locally (leaves to lobby), never to menu.
      if (ev->home) ui_duel_home(ev);
      break;
    case SCR_MSG:
      break;  // hidden
    case SCR_SETTINGS: {
      // Keyboard owns Home while active (cancel) — don't also go back.
      bool kb = ui_keyboard_active();
      ui_settings_tick(now_ms, ev);
      if (ev->home && !kb) nav_show(SCR_MENU);
      break;
    }
  }
}

screen_t nav_current(void) { return current; }

const char *nav_name(screen_t s) {
  switch (s) {
    case SCR_MENU: return "menu";
    case SCR_PLAY: return "play";
    case SCR_DUEL: return "duel";
    case SCR_MSG: return "msg";
    case SCR_SETTINGS: return "settings";
    default: return "?";
  }
}
