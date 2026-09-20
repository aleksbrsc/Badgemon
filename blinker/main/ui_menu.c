#include "ui_menu.h"
#include "hal_led.h"
#include "hal_display.h"
#include "ui_font.h"
#include "assets.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "menu";

#define MENU_MAX_ITEMS 8
#define MENU_ITEM_LEN 32

// Same game-UI language as the keyboard: full-width keys on 1px gaps,
// teal idle, red cursor. One-column button matrix; LVGL renders the
// FOCUS_KEY state on the selected button (see ui_keyboard.c).
static char map_text[MENU_MAX_ITEMS][MENU_ITEM_LEN];
static const char *map[MENU_MAX_ITEMS * 2 + 1];

static const char **menu_items;
static int menu_n = 0;
static int selected = 0;
static menu_select_cb select_cb = NULL;
static menu_back_cb back_cb = NULL;
static void *cb_ctx = NULL;
static lv_obj_t *btnm;

static void refresh_sel(void) {
  if (!btnm) return;
  lv_buttonmatrix_set_selected_button(btnm, (uint16_t)selected);
}

void ui_menu_open(const char *title, const char *subtitle, const char *items[], int n_items,
                  menu_select_cb on_select, menu_back_cb on_back, void *ctx) {
  ESP_LOGI(TAG, "open '%s' (%d items)", title ? title : "", n_items);
  if (n_items > MENU_MAX_ITEMS) n_items = MENU_MAX_ITEMS;
  if (n_items < 1) n_items = 1;
  menu_items = items;
  menu_n = n_items;
  selected = 0;
  select_cb = on_select;
  back_cb = on_back;
  cb_ctx = ctx;
  // Flatten items into a one-per-row button-matrix map.
  for (int i = 0; i < menu_n; i++) {
    strncpy(map_text[i], items[i] ? items[i] : "", MENU_ITEM_LEN - 1);
    map_text[i][MENU_ITEM_LEN - 1] = '\0';
    map[i * 2] = map_text[i];
    map[i * 2 + 1] = "\n";
  }
  map[menu_n * 2 - 1] = "";
  if (!lvgl_port_lock(0)) return;
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();

  // Party backdrop (baked YOU plate + dialog bar are decorative here;
  // the dialog bar frames the footer hint).
  lv_obj_t *bg = lv_image_create(scr);
  lv_image_set_src(bg, &assets_party_bg);
  lv_obj_set_pos(bg, 0, 0);

  lv_obj_t *t = lv_label_create(scr);
  lv_label_set_text(t, title ? title : "");
  lv_obj_set_style_text_font(t, BADGE_FONT, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_hex(0xFFE45E), LV_PART_MAIN);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, MENU_TITLE_Y);

  if (subtitle && subtitle[0]) {
    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, subtitle);
    lv_obj_set_style_text_font(sub, BADGE_FONT_SMALL, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, MENU_TITLE_Y + 24);
  }

  lv_obj_t *m = lv_buttonmatrix_create(scr);
  lv_buttonmatrix_set_map(m, map);
  lv_buttonmatrix_set_one_checked(m, false);
  lv_buttonmatrix_set_selected_button(m, 0);
  lv_obj_add_state(m, LV_STATE_FOCUS_KEY);  // cursor highlight (see draw_main)
  int32_t rows_h = menu_n * MENU_BTN_H + (menu_n - 1) * 2;
  lv_obj_set_size(m, MENU_BTNS_W, rows_h);
  lv_obj_set_pos(m, MENU_BTNS_X, MENU_BTNS_Y);
  lv_obj_set_style_text_font(m, badge_font(), LV_PART_ITEMS);
  lv_obj_set_style_border_width(m, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(m, lv_color_hex(0x084841), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(m, lv_color_hex(0x0B6B5E), LV_PART_ITEMS);
  lv_obj_set_style_text_color(m, lv_color_white(), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(m, lv_color_hex(0xE8442E), LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_text_color(m, lv_color_white(), LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_radius(m, 4, LV_PART_ITEMS);
  lv_obj_set_style_pad_gap(m, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(m, 2, LV_PART_MAIN);
  btnm = m;

  lv_obj_t *foot = lv_label_create(scr);
  lv_label_set_text(foot, back_cb ? "up/down move, A open, Home back" : "up/down move, A open");
  lv_obj_set_style_text_font(foot, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(foot, lv_color_hex(0x1F353C), LV_PART_MAIN);
  lv_obj_set_pos(foot, PARTY_DLG_X, PARTY_DLG_Y);
  lv_obj_set_width(foot, PARTY_DLG_W);
  lvgl_port_unlock();
  refresh_sel();
  hal_led_set_all(0, 0, 0);
}

void ui_menu_tick(uint32_t now_ms, const btn_event_t *ev) {
  (void)now_ms;
  if (ev->up) {
    selected = (selected + menu_n - 1) % menu_n;
    refresh_sel();
  }
  if (ev->down) {
    selected = (selected + 1) % menu_n;
    refresh_sel();
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

void ui_menu_debug(char *out, int cap) {
  if (!out || cap < 1) return;
  int n = snprintf(out, cap, "menu sel=%d/%d items=[", selected, menu_n);
  for (int i = 0; i < menu_n && n < cap - 4; i++)
    n += snprintf(out + n, (size_t)(cap - n), "%s%s", i ? " " : "", menu_items[i]);
  snprintf(out + (n < cap ? n : cap - 1), (size_t)(cap - (n < cap ? n : cap - 1)), "]");
}
