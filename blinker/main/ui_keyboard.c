#include "ui_keyboard.h"
#include "hal_display.h"
#include "ui_font.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "kbd";

// QWERTY rows stretched full-width via lv_buttonmatrix.
// Bottom row holds shift = up-arrow (one-shot caps), SPACE (wide), symbols.
// Key ids (index into the map, skipping "\n" entries).
static const char *KB_MAP[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
  "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
  "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
  "z", "x", "c", "v", "b", "n", "m", ".", "_", "\n",
  LV_SYMBOL_UP, "SPACE", "-", "+", "",
};
#define N_ROWS 5
// Row start ids: row0=0..9, row1=10..19, row2=20..28, row3=29..37, row4=38..41
#define KEY_SHIFT 38
#define KEY_SPACE 39
#define KEY_DASH 40
#define KEY_PLUS 41
#define N_KEYS 42
// Printable char for each key id (SHIFT/SPACE handled specially).
static char key_char(int id) {
  if (id <= 9) return '0' + ((id + 1) % 10);  // id0='1' ... id8='9', id9='0'
  if (id >= 10 && id <= 19) return "qwertyuiop"[id - 10];
  if (id >= 20 && id <= 28) return "asdfghjkl"[id - 20];
  if (id >= 29 && id <= 37) return "zxcvbnm._"[id - 29];
  if (id == KEY_DASH) return '-';
  if (id == KEY_PLUS) return '+';
  return 0;
}
// Row/col geometry for d-pad movement (ragged rows).
static const uint8_t ROW_LEN[N_ROWS] = {10, 10, 9, 9, 4};
static const uint8_t ROW_BASE[N_ROWS] = {0, 10, 20, 29, 38};

static bool active = false;
static char text[KEYBOARD_TEXT_MAX + 1];
static int text_max = KEYBOARD_TEXT_MAX;
static int cursor = 0;
static int cursor_row = 0, cursor_col = 0;
static bool shift_on = false;
static bool was_start = false;
static keyboard_done_cb done_cb = NULL;
static void *done_ctx = NULL;
static lv_obj_t *text_label;
static lv_obj_t *btnm;

static void refresh_btnm(void) {
  if (!btnm) return;
  lv_buttonmatrix_set_selected_button(btnm, (uint16_t)cursor);
  // Highlight SHIFT state via ctrl on the SHIFT key.
  if (shift_on)
    lv_buttonmatrix_set_button_ctrl(btnm, (uint16_t)KEY_SHIFT, LV_BUTTONMATRIX_CTRL_CHECKED);
  else
    lv_buttonmatrix_clear_button_ctrl(btnm, (uint16_t)KEY_SHIFT, LV_BUTTONMATRIX_CTRL_CHECKED);
}

static void redraw(void) {
  if (!lvgl_port_lock(0)) return;
  char tbuf[KEYBOARD_TEXT_MAX + 8];
  snprintf(tbuf, sizeof(tbuf), "%s%s_", text, shift_on ? "^" : "");
  if (text_label) lv_label_set_text(text_label, tbuf);
  refresh_btnm();
  lvgl_port_unlock();
}

static void move_cursor(int drow, int dcol) {
  if (dcol != 0) {
    int len = ROW_LEN[cursor_row];
    cursor_col = (cursor_col + dcol) % len;
    if (cursor_col < 0) cursor_col += len;
  } else {
    // Vertical move: keep proportional x so rows of different lengths
    // (9-letter rows vs the 4-key bottom row) don't jump sideways.
    int old_len = ROW_LEN[cursor_row];
    cursor_row = (cursor_row + drow + N_ROWS) % N_ROWS;
    int new_len = ROW_LEN[cursor_row];
    if (old_len > 1)
      cursor_col = (cursor_col * (new_len - 1) + (old_len - 1) / 2) / (old_len - 1);
    else
      cursor_col = 0;
  }
  cursor = ROW_BASE[cursor_row] + cursor_col;
}

void ui_keyboard_start(const char *initial, int max_len, keyboard_done_cb on_done, void *ctx) {
  ESP_LOGI(TAG, "open max=%d initial='%s'", max_len, initial ? initial : "");
  if (!lvgl_port_lock(0)) return;
  hal_display_reset();
  lv_obj_t *scr = lv_scr_act();
  lv_obj_t *t = lv_label_create(scr);
  lv_obj_set_style_text_font(t, BADGE_FONT, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 8, 2);
  lv_obj_set_width(t, HAL_LCD_W - 16);

  lv_obj_t *m = lv_buttonmatrix_create(scr);
  lv_buttonmatrix_set_map(m, KB_MAP);
  lv_buttonmatrix_set_one_checked(m, false);
  lv_buttonmatrix_set_selected_button(m, 0);
  // Selected button only renders FOCUS_KEY when the widget itself holds
  // that state — set it once so the d-pad cursor is always highlighted.
  lv_obj_add_state(m, LV_STATE_FOCUS_KEY);
  // Stretch across the screen; thin 1px borders between keys.
  lv_obj_set_size(m, HAL_LCD_W - 8, 168);
  lv_obj_align(m, LV_ALIGN_TOP_LEFT, 4, 40);
  lv_obj_set_style_text_font(m, badge_font(), LV_PART_ITEMS);
  lv_obj_set_style_border_width(m, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(m, lv_color_hex(0x084841), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(m, lv_color_hex(0x0B6B5E), LV_PART_ITEMS);
  lv_obj_set_style_text_color(m, lv_color_white(), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(m, lv_color_hex(0xE8442E), LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
  lv_obj_set_style_text_color(m, lv_color_white(), LV_PART_ITEMS | LV_STATE_FOCUS_KEY);
  // Armed shift (CHECKED ctrl on the arrow key) gets its own color so it
  // never clashes with the red cursor highlight.
  lv_obj_set_style_bg_color(m, lv_color_hex(0x2E9E44), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(m, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_pad_gap(m, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_all(m, 1, LV_PART_MAIN);
  // Bottom row spans the same 10-unit grid as the letter rows
  // (SHIFT=2, SPACE=6, -=1, +=1) so its key edges line up with the
  // columns above instead of floating mid-grid.
  lv_buttonmatrix_set_button_width(m, (uint16_t)KEY_SHIFT, 2);
  lv_buttonmatrix_set_button_width(m, (uint16_t)KEY_SPACE, 6);
  btnm = m;

  lv_obj_t *foot = lv_label_create(scr);
  lv_label_set_text(foot, "A pick B del Start save Home back");
  lv_obj_set_style_text_font(foot, BADGE_FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(foot, lv_color_hex(0x888888), LV_PART_MAIN);
  lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -6);
  lvgl_port_unlock();
  ui_keyboard_bind(t, m, initial, max_len, on_done, ctx);
}

void ui_keyboard_bind(lv_obj_t *t, lv_obj_t *g, const char *initial, int max_len,
                      keyboard_done_cb on_done, void *ctx) {
  text_max = max_len < 1 ? 1 : max_len > KEYBOARD_TEXT_MAX ? KEYBOARD_TEXT_MAX : max_len;
  strncpy(text, initial ? initial : "", text_max);
  text[text_max] = '\0';
  cursor = 0;
  cursor_row = 0;
  cursor_col = 0;
  shift_on = false;
  was_start = hal_buttons_start();
  done_cb = on_done;
  done_ctx = ctx;
  text_label = t;
  btnm = g;
  if (btnm) lv_obj_add_state(btnm, LV_STATE_FOCUS_KEY);
  active = true;
  redraw();
}

bool ui_keyboard_active(void) { return active; }

static void finish(const char *result) {
  ESP_LOGI(TAG, "close result='%s'", result ? result : "(cancel)");
  active = false;
  btnm = NULL;
  text_label = NULL;
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
  if (ev->left) { move_cursor(0, -1); dirty = true; }
  if (ev->right) { move_cursor(0, 1); dirty = true; }
  if (ev->up) { move_cursor(-1, 0); dirty = true; }
  if (ev->down) { move_cursor(1, 0); dirty = true; }
  if (ev->a && (int)len < text_max) {
    if (cursor == KEY_SHIFT) {
      shift_on = !shift_on;
      ESP_LOGD(TAG, "shift %s", shift_on ? "on" : "off");
    } else if (cursor == KEY_SPACE) {
      text[len] = ' ';
      text[len + 1] = '\0';
    } else {
      char c = key_char(cursor);
      if (shift_on && c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
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

void ui_keyboard_debug(char *out, int cap) {
  if (!out || cap < 1) return;
  snprintf(out, cap, "kbd active=%d text='%s' cursor=%d (r%d c%d) shift=%d", active ? 1 : 0,
           text, cursor, cursor_row, cursor_col, shift_on ? 1 : 0);
}
