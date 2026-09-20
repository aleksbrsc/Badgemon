#include "ui_ping.h"
#include "hal_led.h"
#include "hal_display.h"
#include "hal_buttons.h"
#include "net.h"
#include "store.h"
#include "ui_msg.h"
#include "ui_font.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ping";

#define TX_SLOTS 4
static const uint8_t DEMO[TX_SLOTS] = {1, 3, 3, 7};  // hardcoded values

static lv_obj_t *tx_label;
static lv_obj_t *rx_label;
static lv_obj_t *event_label;
static uint8_t tx[TX_SLOTS];
static int cursor = 0;
static uint32_t last_tx_ms = 0;
static int burst = 0;
static int idle_div = 0;
static bool idle_on = false;
static bool was_start = false;

static void tx_redraw(void) {
  if (!lvgl_port_lock(0)) return;
  char buf[48];
  int n = snprintf(buf, sizeof(buf), "tx:");
  for (int i = 0; i < TX_SLOTS && n < (int)sizeof(buf) - 8; i++)
    n += snprintf(buf + n, sizeof(buf) - n, i == cursor ? " [%d]" : " %d", tx[i]);
  lv_label_set_text(tx_label, buf);
  lvgl_port_unlock();
}

static void event_show(const char *s) {
  if (!lvgl_port_lock(0)) return;
  lv_label_set_text(event_label, s);
  lvgl_port_unlock();
}

void ui_ping_enter(void) {
  const uint8_t *mac = net_mac();
  ESP_LOGI(TAG, "enter me=%02X:%02X", mac[4], mac[5]);
  memcpy(tx, DEMO, TX_SLOTS);
  cursor = 0;
  was_start = false;
  if (!lvgl_port_lock(0)) return;
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "ping");
  lv_obj_set_style_text_font(title, BADGE_FONT, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
  tx_label = lv_label_create(scr);
  lv_obj_set_style_text_font(tx_label, BADGE_FONT, LV_PART_MAIN);
  lv_obj_set_style_text_color(tx_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(tx_label, LV_ALIGN_TOP_MID, 0, 80);
  rx_label = lv_label_create(scr);
  lv_obj_set_style_text_font(rx_label, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(rx_label, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
  lv_obj_align(rx_label, LV_ALIGN_TOP_MID, 0, 130);
  lv_label_set_text(rx_label, "rx --:--: -");
  event_label = lv_label_create(scr);
  lv_obj_set_style_text_font(event_label, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(event_label, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
  lv_obj_align(event_label, LV_ALIGN_TOP_MID, 0, 160);
  char me[48];
  char name[STORE_NAME_MAX + 1];
  store_get_name(name, sizeof(name));
  if (name[0])
    snprintf(me, sizeof(me), "me %02X:%02X (%s)", mac[4], mac[5], name);
  else
    snprintf(me, sizeof(me), "me %02X:%02X", mac[4], mac[5]);
  lv_label_set_text(event_label, me);
  lv_obj_t *foot = lv_label_create(scr);
  lv_label_set_text(foot, "up/dn digit  l/r move\nA send  B clear  Start demo  Home back");
  lv_obj_set_style_text_font(foot, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(foot, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -8);
  lvgl_port_unlock();
  tx_redraw();
}

void ui_ping_tick(uint32_t now_ms, const btn_event_t *ev) {
  bool dirty = false;
  if (ev->up) {
    tx[cursor] = (tx[cursor] + 1) % 10;
    dirty = true;
  }
  if (ev->down) {
    tx[cursor] = (tx[cursor] + 9) % 10;
    dirty = true;
  }
  if (ev->left) {
    cursor = (cursor + TX_SLOTS - 1) % TX_SLOTS;
    dirty = true;
  }
  if (ev->right) {
    cursor = (cursor + 1) % TX_SLOTS;
    dirty = true;
  }
  if (dirty) tx_redraw();
  if (ev->b) {
    memset(tx, 0, TX_SLOTS);
    cursor = 0;
    tx_redraw();
    event_show("cleared");
  }
  // Start = reload demo (edge). Home = back, owned by nav.
  bool start_now = hal_buttons_start();
  bool start_edge = start_now && !was_start;
  was_start = start_now;
  if (start_edge) {
    memcpy(tx, DEMO, TX_SLOTS);
    cursor = 0;
    tx_redraw();
    event_show("demo loaded, A to send");
  }
  if (ev->a && now_ms - last_tx_ms > 500) {
    last_tx_ms = now_ms;
    uint32_t seq = 0;
    if (net_send(PKT_PING, tx, TX_SLOTS, &seq) == ESP_OK) {
      char msg[48];
      snprintf(msg, sizeof(msg), "sent #%u", (unsigned)seq);
      event_show(msg);
    } else {
      event_show("send failed");
    }
  }

  ping_msg_t rx;
  if (net_recv(&rx)) {
    ESP_LOGI(TAG, "rx #%u from %02X:%02X len=%d", (unsigned)rx.seq, rx.mac[4], rx.mac[5],
             rx.len);
    if (lvgl_port_lock(0)) {
      char buf[64];
      char line[72];
      msg_format_vals(buf, sizeof(buf), rx.mac, rx.vals, rx.len);
      snprintf(line, sizeof(line), "rx %s", buf);
      lv_label_set_text(rx_label, line);
      char ev2[40];
      snprintf(ev2, sizeof(ev2), "rx #%u from %02X:%02X", (unsigned)rx.seq, rx.mac[4],
               rx.mac[5]);
      lv_label_set_text(event_label, ev2);
      lvgl_port_unlock();
    }
    burst = 10;  // ~200 ms green burst at 20 ms ticks
  }

  if (burst > 0) {
    burst--;
    hal_led_set_all(0, 24, 0);
  } else if (++idle_div >= 10) {
    idle_div = 0;
    idle_on = !idle_on;
    hal_led_set_all(idle_on ? 24 : 0, 0, 0);
  }
}
