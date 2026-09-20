/*
 * ping badge: single ESP-NOW ping app with button keyboard composer.
 * Controls: Up/Down digit, Left/Right cursor, A send, B clear, H demo.
 * Payload format: payload.h. See README.md.
 *
 * Pins/values from custom-firmware-hal.md (source of truth):
 *   LCD  MOSI=10 CLK=1 CS=2 DC=0 RST=4 (SPI2, 40MHz, mode 0, RGB565 320x240)
 *   BTNS HC165 DATA=7 LOAD=20 CLK=21, order A,B,Home,Down,Left,Right,Up,Aux1
 *        (active-low, A shifts first). START=GPIO9 (separate).
 *   LEDS WS2812 x6 on GPIO3, GRB order (dim for AA power)
 *   I2C  SDA=5 SCL=6 (accel 0x19, NFC 0x26) — debug console probes.
 *   WiFi STA channel 1, ESP-NOW broadcast.
 */
#include "hal_buttons.h"
#include "hal_led.h"
#include "hal_display.h"
#include "hal_i2c.h"
#include "net.h"
#include "store.h"
#include "game.h"
#include "nav.h"
#include "debug.h"
#include "fault.h"
#include "assets.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static const char *reset_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "?";
  }
}

// Boot splash: fade in over 2s, hold 2.5s, fade out over 1s (out is
// faster so the tail does not feel sluggish).
#define SPLASH_STEP_MS 40
#define SPLASH_FADE_IN_MS 2000
#define SPLASH_FADE_OUT_MS 1000
#define SPLASH_HOLD_MS 2500

static void splash_set_opa(lv_obj_t *img, lv_opa_t opa) {
  if (!img) return;
  if (!lvgl_port_lock(0)) return;
  lv_obj_set_style_image_opa(img, opa, LV_PART_MAIN);
  lvgl_port_unlock();
}

static void boot_loading_screen(void) {
  if (!lvgl_port_lock(0)) {
    ESP_LOGW(TAG, "splash: could not lock LVGL, skipping");
    return;
  }
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_t *img = lv_image_create(scr);
  lv_image_set_src(img, &assets_loading);
  lv_obj_set_pos(img, 0, 0);
  lv_obj_set_style_image_opa(img, LV_OPA_TRANSP, LV_PART_MAIN);
  lvgl_port_unlock();
  hal_led_set_all(0, 0, 0);

  for (int t = 0; t <= SPLASH_FADE_IN_MS; t += SPLASH_STEP_MS) {
    splash_set_opa(img, (lv_opa_t)(255 * t / SPLASH_FADE_IN_MS));
    vTaskDelay(pdMS_TO_TICKS(SPLASH_STEP_MS));
  }
  splash_set_opa(img, LV_OPA_COVER);
  vTaskDelay(pdMS_TO_TICKS(SPLASH_HOLD_MS));
  for (int t = 0; t <= SPLASH_FADE_OUT_MS; t += SPLASH_STEP_MS) {
    splash_set_opa(img,
                   (lv_opa_t)(255 * (SPLASH_FADE_OUT_MS - t) / SPLASH_FADE_OUT_MS));
    vTaskDelay(pdMS_TO_TICKS(SPLASH_STEP_MS));
  }
  splash_set_opa(img, LV_OPA_TRANSP);
}

void app_main(void) {
  fault_init();  // report reset reason; loud banner if last boot crashed
  esp_reset_reason_t reason = esp_reset_reason();
  ESP_LOGI(TAG, "ping badge booting (reset=%s)", reset_name(reason));
  hal_buttons_init();
  hal_led_init();
  hal_display_init();
  fault_show_boot_banner();  // hold the crash reason on-screen (crash only)
  hal_i2c_init();
  net_init();
  store_init();  // NVS ready (nvs_flash_init ran in net_init)
  game_init();
  store_note_boot(reason);  // boot forensics for `boot` cmd (brownout vs nav bug)
  debug_init();  // serial REPL over USB-Serial-JTAG
  boot_loading_screen();  // fade splash in/hold/out before the menu
  nav_show(SCR_MENU);

  btn_event_t ev;
  while (1) {
    bool changed = hal_buttons_poll(&ev);
    debug_merge(&ev);  // OR in serial-injected presses (`press` cmd)
    if (changed) ESP_LOGD(TAG, "btn edge a=%d b=%d home=%d down=%d left=%d right=%d up=%d",
                          ev.a, ev.b, ev.home, ev.down, ev.left, ev.right, ev.up);
    if (!debug_hijacked()) nav_tick(xTaskGetTickCount() * portTICK_PERIOD_MS, &ev);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
