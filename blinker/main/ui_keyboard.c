#include "ui_keyboard.h"
#include "hal_display.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "kbd";

// Grid charset (no '#' — reserved for LVGL recolor tags).
static const char CHARSET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    " +-_.";
#define CHARSET_LEN (sizeof(CHARSET) - 1)
#define GRID_COLS 11  // fills 320px at montserrat_14, left-aligned

static bool active = false;
static char text[KEYBOARD_TEXT_MAX + 1];
static int cursor = 0;
static keyboard_done_cb done_cb = NULL;
static void *done_ctx = NULL;
static lv_obj_t *text_label;
static lv_obj_t *grid_label;

static void redraw(void) {
  if (!lvgl_port_lock(0)) return;
  char tbuf[KEYBOARD_TEXT_MAX + 8];
  snprintf(tbuf, sizeof(tbuf), "%s_", text);
  lv_label_set_text(text_label, tbuf);
  // Grid with cursor wrapped in a red recolor tag.
  char gbuf[256];
  int n = 0;
  for (int i = 0; i < CHARSET_LEN && n < (int)sizeof(gbuf) - 16; i++) {
    if (i % GRID_COLS == 0 && i > 0) gbuf[n++] = '\n';
    else if (i > 0)
      gbuf[n++] = ' ';
    if (i == cursor) {
      n += snprintf(gbuf + n, sizeof(gbuf) - n, "#ff5050 %c#", CHARSET[i]);
    } else {
      gbuf[n++] = CHARSET[i];
    }
  }
  gbuf[n] = '\0';
  lv_label_set_text(grid_label, gbuf);
  lvgl_port_unlock();
}

void ui_keyboard_start(const char *initial, keyboard_done_cb on_done, void *ctx) {
  ESP_LOGI(TAG, "open initial='%s'", initial ? initial : "");
  strncpy(text, initial ? initial : "", sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';
  cursor = 0;
  done_cb = on_done;
  done_ctx = ctx;
  active = true;
  if (!lvgl_port_lock(0)) return;
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();
  text_label = lv_label_create(scr);
  lv_obj_set_style_text_font(text_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(text_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(text_label, LV_ALIGN_TOP_MID, 0, 8);
  grid_label = lv_label_create(scr);
  lv_label_set_recolor(grid_label, true);
  lv_obj_set_style_text_font(grid_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(grid_label, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
  lv_obj_align(grid_label, LV_ALIGN_TOP_LEFT, 8, 52);
  lv_obj_t *foot = lv_label_create(scr);
  lv_label_set_text(foot, "A pick  B del  Start save  Home cancel");
  lv_obj_set_style_text_font(foot, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(foot, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -8);
  lvgl_port_unlock();
  redraw();
}

bool ui_keyboard_active(void) { return active; }

static void finish(const char *result) {
  ESP_LOGI(TAG, "close result='%s'", result ? result : "(cancel)");
  active = false;
  keyboard_done_cb cb = done_cb;
  void *ctx = done_ctx;
  done_cb = NULL;
  done_ctx = NULL;
  if (cb) cb(result, ctx);
}

void ui_keyboard_tick(uint32_t now_ms, const btn_event_t *ev) {
  (void)now_ms;
  if (!active) return;
  size_t len = strlen(text);
  bool dirty = false;
  int rows = (CHARSET_LEN + GRID_COLS - 1) / GRID_COLS;
  int row = cursor / GRID_COLS;
  int col = cursor % GRID_COLS;
  if (ev->left) {
    col = (col + GRID_COLS - 1) % GRID_COLS;
    dirty = true;
  }
  if (ev->right) {
    col = (col + 1) % GRID_COLS;
    dirty = true;
  }
  if (ev->up) {
    row = (row + rows - 1) % rows;
    dirty = true;
  }
  if (ev->down) {
    row = (row + 1) % rows;
    dirty = true;
  }
  if (dirty) {
    cursor = row * GRID_COLS + col;
    if (cursor >= CHARSET_LEN) cursor = CHARSET_LEN - 1;
  }
  if (ev->a && len < KEYBOARD_TEXT_MAX) {
    text[len] = CHARSET[cursor];
    text[len + 1] = '\0';
    dirty = true;
  }
  if (ev->b && len > 0) {
    text[len - 1] = '\0';
    dirty = true;
  }
  if (dirty) redraw();
  if (hal_buttons_start()) {
    char out[KEYBOARD_TEXT_MAX + 1];
    strcpy(out, text);
    finish(out);
    return;
  }
  if (ev->home) finish(NULL);
}
