// Message: keyboard embedded directly in the screen — compose, see it
// live, and send with START. A = pick, B = delete, Home = back.
#include "ui_msg.h"
#include "hal_led.h"
#include "hal_display.h"
#include "net.h"
#include "nav.h"
#include "ui_keyboard.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "msg";

#define MSG_MAX 32

static lv_obj_t *text_label;
static lv_obj_t *grid_label;
static lv_obj_t *rx_label;
static lv_obj_t *event_label;
static int burst = 0;
static uint32_t last_send_ms = 0;
#define SEND_MIN_GAP_MS 800  // ESP-NOW TX queue + rail recovery

static bool printable(const uint8_t *v, int len) {
  if (len <= 0) return false;
  for (int i = 0; i < len; i++)
    if (v[i] < 32 || v[i] > 126) return false;
  return true;
}

// Shared with ui_ping digit payloads: text if all-printable, else numbers.
void msg_format_vals(char *buf, int cap, const uint8_t *mac, const uint8_t *vals, int len) {
  int n;
  if (printable(vals, len)) {
    char tmp[PING_MAX_VALS + 1];
    memcpy(tmp, vals, len);
    tmp[len] = '\0';
    n = snprintf(buf, cap, "%02X:%02X: %s", mac[4], mac[5], tmp);
  } else {
    n = snprintf(buf, cap, "%02X:%02X:", mac[4], mac[5]);
    for (int i = 0; i < len && n < cap - 4; i++) n += snprintf(buf + n, cap - n, " %d", vals[i]);
  }
  (void)n;
}

static void on_text_done(const char *text, void *ctx) {
  (void)ctx;
  if (!text) {
    ESP_LOGI(TAG, "cancelled");
    nav_show(SCR_MENU);  // Home is always back
    return;
  }
  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if (now - last_send_ms < SEND_MIN_GAP_MS) {
    ESP_LOGD(TAG, "send dropped: too soon (gap)");
    if (lvgl_port_lock(0)) {
      lv_label_set_text(event_label, "slow down...");
      lvgl_port_unlock();
    }
    // Stay composable: rebind so keys keep working.
    ui_keyboard_bind(text_label, grid_label, text, MSG_MAX, on_text_done, NULL);
    return;
  }
  last_send_ms = now;
  uint32_t seq = 0;
  ESP_LOGI(TAG, "send req len=%d", (int)strlen(text));
  esp_err_t err = net_send(PKT_PING, (const uint8_t *)text, strlen(text), &seq);
  ESP_LOGI(TAG, "sent #%u (%s): '%s'", (unsigned)seq, esp_err_to_name(err), text);
  if (lvgl_port_lock(0)) {
    char msg[64];
    if (err == ESP_OK)
      snprintf(msg, sizeof(msg), "sent #%u", (unsigned)seq);
    else
      snprintf(msg, sizeof(msg), "send busy, retry");
    lv_label_set_text(event_label, msg);
    lvgl_port_unlock();
  }
  // Keep composing: rebind the same labels with the sent text.
  ui_keyboard_bind(text_label, grid_label, text, MSG_MAX, on_text_done, NULL);
}

void ui_msg_enter(void) {
  ESP_LOGI(TAG, "enter");
  burst = 0;
  if (!lvgl_port_lock(0)) return;
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "message");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 0);
  rx_label = lv_label_create(scr);
  lv_obj_set_style_text_font(rx_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(rx_label, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
  lv_obj_set_width(rx_label, 312);
  lv_label_set_long_mode(rx_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(rx_label, LV_ALIGN_TOP_LEFT, 8, 18);
  lv_label_set_text(rx_label, "rx --:--");
  text_label = lv_label_create(scr);
  lv_obj_set_style_text_font(text_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(text_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_width(text_label, 312);
  lv_label_set_long_mode(text_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(text_label, LV_ALIGN_TOP_LEFT, 8, 38);
  grid_label = lv_label_create(scr);
  lv_label_set_recolor(grid_label, true);
  lv_obj_set_style_text_font(grid_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(grid_label, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
  lv_obj_set_style_text_line_space(grid_label, -6, LV_PART_MAIN);  // 6 rows fit
  lv_obj_align(grid_label, LV_ALIGN_TOP_LEFT, 8, 72);
  event_label = lv_label_create(scr);
  lv_obj_set_style_text_font(event_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(event_label, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(event_label, LV_ALIGN_BOTTOM_LEFT, 8, -6);
  const uint8_t *mac = net_mac();
  char me[32];
  snprintf(me, sizeof(me), "me %02X:%02X", mac[4], mac[5]);
  lv_label_set_text(event_label, me);
  lvgl_port_unlock();
  ui_keyboard_bind(text_label, grid_label, "", MSG_MAX, on_text_done, NULL);
  hal_led_set_all(0, 0, 8);
}

void ui_msg_tick(uint32_t now_ms, const btn_event_t *ev) {
  if (ui_keyboard_active()) ui_keyboard_tick(now_ms, ev);

  ping_msg_t rx;
  if (net_recv(&rx)) {
    ESP_LOGI(TAG, "rx #%u from %02X:%02X len=%d", (unsigned)rx.seq, rx.mac[4], rx.mac[5],
             rx.len);
    if (lvgl_port_lock(0)) {
      char buf[64];
      msg_format_vals(buf, sizeof(buf), rx.mac, rx.vals, rx.len);
      char line[96];
      snprintf(line, sizeof(line), "rx %s", buf);
      lv_label_set_text(rx_label, line);
      lvgl_port_unlock();
    }
    burst = 10;
  }

  if (burst > 0) {
    burst--;
    hal_led_set_all(0, 24, 0);
  } else {
    hal_led_set_all(0, 0, 8);
  }
}
