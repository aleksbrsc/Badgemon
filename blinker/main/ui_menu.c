#include "ui_menu.h"
#include "hal_led.h"
#include "hal_display.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <stdio.h>

static const char *TAG = "menu";

#define MENU_MAX_ITEMS 8

static const char **menu_items;
static int menu_n = 0;
static int selected = 0;
static menu_select_cb select_cb = NULL;
static menu_back_cb back_cb = NULL;
static void *cb_ctx = NULL;
static lv_obj_t *items_label;

static void redraw(void) {
  if (!lvgl_port_lock(0)) return;
  char buf[160];
  int n = 0;
  for (int i = 0; i < menu_n && n < (int)sizeof(buf) - 16; i++)
    n += snprintf(buf + n, sizeof(buf) - n, "%s %s\n", i == selected ? ">" : " ",
                  menu_items[i]);
  lv_label_set_text(items_label, buf);
  lvgl_port_unlock();
}

void ui_menu_open(const char *title, const char *subtitle, const char *items[], int n_items,
                  menu_select_cb on_select, menu_back_cb on_back, void *ctx) {
  ESP_LOGI(TAG, "open '%s' (%d items)", title ? title : "", n_items);
  if (n_items > MENU_MAX_ITEMS) n_items = MENU_MAX_ITEMS;
  menu_items = items;
  menu_n = n_items;
  selected = 0;
  select_cb = on_select;
  back_cb = on_back;
  cb_ctx = ctx;
  if (!lvgl_port_lock(0)) return;
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_t *t = lv_label_create(scr);
  lv_label_set_text(t, title ? title : "");
  lv_obj_set_style_text_font(t, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);
  if (subtitle && subtitle[0]) {
    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, subtitle);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 72);
  }
  items_label = lv_label_create(scr);
  lv_obj_set_style_text_font(items_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(items_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(items_label, LV_ALIGN_CENTER, 0, 10);
  lv_obj_t *foot = lv_label_create(scr);
  lv_label_set_text(foot, back_cb ? "up/down move, A open, Home back" : "up/down move, A open");
  lv_obj_set_style_text_font(foot, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(foot, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -8);
  lvgl_port_unlock();
  redraw();
  hal_led_set_all(0, 0, 0);
}

void ui_menu_tick(uint32_t now_ms, const btn_event_t *ev) {
  (void)now_ms;
  if (ev->up) {
    selected = (selected + menu_n - 1) % menu_n;
    redraw();
  }
  if (ev->down) {
    selected = (selected + 1) % menu_n;
    redraw();
  }
  if (ev->a && select_cb) {
    ESP_LOGI(TAG, "pick %d", selected);
    select_cb(selected, cb_ctx);
  }
  if (ev->home && back_cb) {
    ESP_LOGI(TAG, "back");
    back_cb(cb_ctx);
  }
}
