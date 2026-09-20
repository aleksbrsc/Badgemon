#include "ui_settings.h"
#include "hal_led.h"
#include "store.h"
#include "ui_menu.h"
#include "ui_keyboard.h"
#include "nav.h"
#include "esp_log.h"

static const char *TAG = "settings";

// Legacy settings screen: now goes straight to the name keyboard
// (no edit/clear submenu). Kept so SCR_SETTINGS still works.
static void on_name_done(const char *text, void *ctx) {
  (void)ctx;
  if (text) {
    if (!store_set_name(text)) ESP_LOGW(TAG, "save failed");
  } else {
    ESP_LOGI(TAG, "edit cancelled");
  }
  nav_show(SCR_MENU);
}

void ui_settings_enter(void) {
  ESP_LOGI(TAG, "enter -> keyboard directly");
  char cur[STORE_NAME_MAX + 1];
  store_get_name(cur, sizeof(cur));
  ui_keyboard_start(cur, STORE_NAME_MAX, on_name_done, NULL);
  hal_led_set_all(0, 0, 0);
}

void ui_settings_tick(uint32_t now_ms, const btn_event_t *ev) {
  if (ui_keyboard_active()) {
    ui_keyboard_tick(now_ms, ev);
    return;
  }
  // Keyboard closed without callback (shouldn't happen) -> back to menu.
  (void)now_ms;
  (void)ev;
  nav_show(SCR_MENU);
}
