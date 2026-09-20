#include "ui_settings.h"
#include "hal_led.h"
#include "store.h"
#include "ui_menu.h"
#include "ui_keyboard.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "settings";

static const char *options[] = {"edit name", "clear name"};

// Keyboard callback: text=NULL means cancelled via Home.
static void on_name_done(const char *text, void *ctx) {
  (void)ctx;
  if (text) {
    if (!store_set_name(text)) {
      ESP_LOGW(TAG, "save failed");
    }
  } else {
    ESP_LOGI(TAG, "edit cancelled");
  }
  ui_settings_enter();  // rebuild with new name
}

static void on_option_pick(int index, void *ctx) {
  (void)ctx;
  if (index == 0) {
    char cur[STORE_NAME_MAX + 1];
    store_get_name(cur, sizeof(cur));
    ui_keyboard_start(cur, STORE_NAME_MAX, on_name_done, NULL);
  } else {
    store_set_name("");
    ui_settings_enter();
  }
}

// No-op back: nav owns Home-back globally; passing non-NULL only makes
// the generic menu print the "Home back" hint.
static void on_back_noop(void *ctx) { (void)ctx; }

void ui_settings_enter(void) {
  ESP_LOGI(TAG, "enter");
  char cur[STORE_NAME_MAX + 1];
  store_get_name(cur, sizeof(cur));
  char sub[STORE_NAME_MAX + 8];
  snprintf(sub, sizeof(sub), "name: %s", cur[0] ? cur : "(unset)");
  // Back is Home, owned by nav — non-NULL back_cb only prints the hint.
  ui_menu_open("settings", sub, options, 2, on_option_pick, on_back_noop, NULL);
  hal_led_set_all(0, 0, 0);
}

void ui_settings_tick(uint32_t now_ms, const btn_event_t *ev) {
  if (ui_keyboard_active()) {
    ui_keyboard_tick(now_ms, ev);
    return;  // keyboard owns input until Start/Home
  }
  ui_menu_tick(now_ms, ev);
}
