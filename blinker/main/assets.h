// Badge art: pixel-art PNGs from blinker/assets, converted to LVGL v9 C
// arrays by blinker/assets/convert_assets.py (nearest-neighbor, crisp).
//   duel_bg   320x240 RGB565    battle scene (baked HP plates + dialog strip)
//   party_bg  320x240 RGB565    teal stripes (baked YOU plate + dialog bar)
//   bubble    292x129 RGB565A8  dialog frame
//   slot      177x36  RGB565A8  party row (ball + HP bar)
//   cursor      12x26 RGB565A8  selection arrow
//   cursor_sm    8x17 RGB565A8  selection arrow, duel move list
//
// Half-res overlay coordinates (full-res pixels / 2, verified against the
// PNG bytes — see convert_assets.py header). All rects are (x, y, w, h).
#pragma once

#include "lvgl.h"

LV_IMAGE_DECLARE(assets_duel_bg);
LV_IMAGE_DECLARE(assets_party_bg);
LV_IMAGE_DECLARE(assets_bubble);
LV_IMAGE_DECLARE(assets_slot);
LV_IMAGE_DECLARE(assets_cursor);
LV_IMAGE_DECLARE(assets_cursor_sm);

// Duel scene (assets_duel_bg): baked plates + dark dialog strip.
#define DUEL_BG_W 320
#define DUEL_BG_H 240
// Enemy (top-left) plate: name + numbers + fill bar over baked HP bar.
#define DUEL_FOE_NAME_X 26
#define DUEL_FOE_NAME_Y 27
#define DUEL_FOE_HP_X 26
#define DUEL_FOE_HP_Y 36
#define DUEL_FOE_BAR_X 69
#define DUEL_FOE_BAR_Y 47
#define DUEL_FOE_BAR_W 57
#define DUEL_FOE_BAR_H 3
// Player (bottom-right) plate.
#define DUEL_ME_NAME_X 192
#define DUEL_ME_NAME_Y 117
#define DUEL_ME_HP_X 192
#define DUEL_ME_HP_Y 141
#define DUEL_ME_BAR_X 236
#define DUEL_ME_BAR_Y 135
#define DUEL_ME_BAR_W 54
#define DUEL_ME_BAR_H 4
// Bottom strip: move list left, battle log right.
#define DUEL_STRIP_Y 166
#define DUEL_MOVES_X 14
#define DUEL_MOVES_W 100
#define DUEL_MOVE_ROW_H 17
#define DUEL_LOG_X 120
#define DUEL_LOG_Y 168
#define DUEL_LOG_W 192

// Party screens (assets_party_bg): baked YOU plate + bottom dialog bar.
#define PARTY_YOU_NAME_X 24
#define PARTY_YOU_NAME_Y 28
#define PARTY_YOU_BAR_X 69
#define PARTY_YOU_BAR_Y 53
#define PARTY_YOU_BAR_W 57
#define PARTY_YOU_BAR_H 7
// Baked dialog bar used for status/footer text (dark-on-white).
#define PARTY_DLG_X 14
#define PARTY_DLG_Y 202
#define PARTY_DLG_W 222

// Play lobby rows: slot image + cursor + name label.
#define PLAY_TITLE_X 150
#define PLAY_TITLE_Y 8
#define PLAY_ROWS_Y 80
#define PLAY_ROW_PITCH 38
#define PLAY_ROWS_SHOWN 3
#define PLAY_SLOT_X 30
#define PLAY_SLOT_W 177
#define PLAY_SLOT_H 36
#define PLAY_CURSOR_X 12
#define PLAY_NAME_X 212
#define PLAY_NAME_W 100

// Challenge dialog: bubble frame + text inside its white interior.
#define PLAY_BUBBLE_X 14
#define PLAY_BUBBLE_Y 55
#define PLAY_BUBBLE_TEXT_X 36
#define PLAY_BUBBLE_TEXT_Y 69
#define PLAY_BUBBLE_TEXT_W 248

// Menu screen on assets_party_bg.
#define MENU_TITLE_Y 88
#define MENU_BTNS_X 24
#define MENU_BTNS_W 272
#define MENU_BTNS_Y 118
#define MENU_BTN_H 34
