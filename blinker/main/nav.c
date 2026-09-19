#include "nav.h"
#include "ui_menu.h"
#include "ui_ping.h"
#include "ui_settings.h"
#include "ui_keyboard.h"
#include "store.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "nav";
static screen_t current = SCR_MENU;

// Main menu built on the reusable ui_menu: re-opened on every show so
// the greeting is always fresh.
static const char *main_items[] = {"ping", "settings"};

static void on_main_pick(int index, void *ctx) {
  (void)ctx;
  nav_show(index == 0 ? SCR_PING : SCR_SETTINGS);
}

static void menu_enter_main(void) {
  char greet[STORE_NAME_MAX + 8];
  char name[STORE_NAME_MAX + 1];
  store_get_name(name, sizeof(name));
  if (name[0])
    snprintf(greet, sizeof(greet), "hi %s", name);
  else
    snprintf(greet, sizeof(greet), "set name in settings");
  ui_menu_open("badge", greet, main_items, 2, on_main_pick, NULL, NULL);
}

void nav_show(screen_t s) {
  ESP_LOGI(TAG, "show %d", (int)s);
  current = s;
  switch (s) {
    case SCR_MENU: menu_enter_main(); break;
    case SCR_PING: ui_ping_enter(); break;
    case SCR_SETTINGS: ui_settings_enter(); break;
  }
}

void nav_tick(uint32_t now_ms, const btn_event_t *ev) {
  switch (current) {
    case SCR_MENU:
      ui_menu_tick(now_ms, ev);
      break;
    case SCR_PING:
      ui_ping_tick(now_ms, ev);
      if (ev->home) nav_show(SCR_MENU);  // Back is always Home
      break;
    case SCR_SETTINGS: {
      // Keyboard owns Home while active (cancel) — don't also go back.
      bool kb = ui_keyboard_active();
      ui_settings_tick(now_ms, ev);
      if (ev->home && !kb) nav_show(SCR_MENU);
      break;
    }
  }
}
