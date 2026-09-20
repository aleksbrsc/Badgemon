#include "ui_keyboard.h"
#include "hal_display.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "kbd";

// 40 chars + aA (shift) + sp (space) = 42 cells, 7 cols x 6 rows.
static const char CHARKEYS[] = "abcdefghijklmnopqrstuvwxyz0123456789._-+";
#define N_CHARKEYS (sizeof(CHARKEYS) - 1)  // 40
#define KEY_SHIFT 40
#define KEY_SPACE 41
#define N_KEYS 42
#define GRID_COLS 7

static bool active = false;
static char text[KEYBOARD_TEXT_MAX + 1];
static int text_max = KEYBOARD_TEXT_MAX;
static int cursor = 0;
static bool shift_on = false;
static bool was_start = false;  // edge-detect START (level would multi-fire)
static keyboard_done_cb done_cb = NULL;
static void *done_ctx = NULL;
static lv_obj_t *text_label;
static lv_obj_t *grid_label;

// Cell label: "aA"/"sp" for specials, else the char.
static void cell_str(int i, char out[4]) {
  if (i == KEY_SHIFT)
    strcpy(out, "aA");
  else if (i == KEY_SPACE)
    strcpy(out, "sp");
  else {
    out[0] = CHARKEYS[i];
    out[1] = '\0';
  }
}

static void redraw(void) {
  if (!lvgl_port_lock(0)) return;
  char tbuf[KEYBOARD_TEXT_MAX + 8];
  snprintf(tbuf, sizeof(tbuf), "%s%s_", text, shift_on ? "^" : "");
  lv_label_set_text(text_label, tbuf);
  char gbuf[320];
  int n = 0;
  char cell[4];
  for (int i = 0; i < N_KEYS && n < (int)sizeof(gbuf) - 24; i++) {
    if (i % GRID_COLS == 0 && i > 0) gbuf[n++] = '\n';
    cell_str(i, cell);
    char padded[8];
    snprintf(padded, sizeof(padded), "%-2s ", cell);  // uniform 3-wide cells
    if (i == cursor)
      n += snprintf(gbuf + n, sizeof(gbuf) - n, "#ff5050 %s#", padded);
    else if (i == KEY_SHIFT && shift_on)
      n += snprintf(gbuf + n, sizeof(gbuf) - n, "#50ff50 %s#", padded);
    else {
      memcpy(gbuf + n, padded, 3);
      n += 3;
    }
  }
  gbuf[n] = '\0';
  lv_label_set_text(grid_label, gbuf);
  lvgl_port_unlock();
}

void ui_keyboard_start(const char *initial, int max_len, keyboard_done_cb on_done, void *ctx) {
  ESP_LOGI(TAG, "open max=%d initial='%s'", max_len, initial ? initial : "");
  if (!lvgl_port_lock(0)) return;
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_t *t = lv_label_create(scr);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 8, 2);
  lv_obj_t *g = lv_label_create(scr);
  lv_label_set_recolor(g, true);
  lv_obj_set_style_text_font(g, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(g, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
  lv_obj_align(g, LV_ALIGN_TOP_LEFT, 8, 40);
  lv_obj_t *foot = lv_label_create(scr);
  lv_label_set_text(foot, "A pick B del Start save Home back");
  lv_obj_set_style_text_font(foot, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(foot, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -6);
  lvgl_port_unlock();
  ui_keyboard_bind(t, g, initial, max_len, on_done, ctx);
}

void ui_keyboard_bind(lv_obj_t *t, lv_obj_t *g, const char *initial, int max_len,
                      keyboard_done_cb on_done, void *ctx) {
  text_max = max_len < 1 ? 1 : max_len > KEYBOARD_TEXT_MAX ? KEYBOARD_TEXT_MAX : max_len;
  strncpy(text, initial ? initial : "", text_max);
  text[text_max] = '\0';
  cursor = 0;
  shift_on = false;
  was_start = hal_buttons_start();  // don't treat held START as fresh press
  done_cb = on_done;
  done_ctx = ctx;
  text_label = t;
  grid_label = g;
  active = true;
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
  int rows = (N_KEYS + GRID_COLS - 1) / GRID_COLS;
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
    if (cursor >= N_KEYS) cursor = N_KEYS - 1;
  }
  if (ev->a && (int)len < text_max) {
    if (cursor == KEY_SHIFT) {
      shift_on = !shift_on;
      ESP_LOGD(TAG, "shift %s", shift_on ? "on" : "off");
    } else if (cursor == KEY_SPACE) {
      text[len] = ' ';
      text[len + 1] = '\0';
    } else {
      char c = CHARKEYS[cursor];
      if (shift_on && c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';  // one-shot caps
        shift_on = false;
      }
      text[len] = c;
      text[len + 1] = '\0';
    }
    dirty = true;
  }
  if (ev->b && len > 0) {
    text[len - 1] = '\0';
    dirty = true;
  }
  if (dirty) redraw();
  // START is level-read: edge-detect so a hold sends exactly once.
  bool start_now = hal_buttons_start();
  bool start_edge = start_now && !was_start;
  was_start = start_now;
  if (start_edge) {
    char out[KEYBOARD_TEXT_MAX + 1];
    strcpy(out, text);
    finish(out);
    return;
  }
  if (ev->home) finish(NULL);
}
