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
LV_IMAGE_DECLARE(assets_speech_half);
LV_IMAGE_DECLARE(assets_caption);
LV_IMAGE_DECLARE(assets_caption_half);
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
// Player (bottom-right) plate. Bar extends a few px left and a bit
// more right vs the baked art so the fill reads clearly.
#define DUEL_ME_NAME_X 192
#define DUEL_ME_NAME_Y 117
#define DUEL_ME_HP_X 192
#define DUEL_ME_HP_Y 141
#define DUEL_ME_HP_W 98
#define DUEL_ME_BAR_X 232
#define DUEL_ME_BAR_Y 135
#define DUEL_ME_BAR_W 66
#define DUEL_ME_BAR_H 4
// Bottom strip: caption bubbles carry the log / prompt, the speech
// bubble carries the command menu. NOTE: speech-bubble-full art does
// not exist yet — the FIGHT move list reuses the full caption bubble
// (same 316x64 rect), swap to it when the art lands.
//   caption full  (2,171)   316x64  log / wait / over / move list
//   caption half  (2,171)   168x64  "What will X do?" prompt
//   speech half   (172,171) 146x64  FIGHT BAG / BADGEMON RUN options
#define DUEL_STRIP_Y 166
#define DUEL_CAP_X 2
#define DUEL_CAP_Y 171
#define DUEL_LOG_X 18
#define DUEL_LOG_Y 185
#define DUEL_LOG_W 284
#define DUEL_PROMPT_W 144
#define DUEL_SPEECH_X 172
#define DUEL_SPEECH_Y 171
// Command options: 2 cols x 2 rows (FIGHT BAG / BADGEMON RUN).
#define DUEL_CMD_COL_X0 190
#define DUEL_CMD_COL_X1 244
#define DUEL_CMD_ROW_Y0 183
#define DUEL_CMD_ROW_Y1 207
// Move list (inside the full caption): 2 cols x 2 rows, nudged toward
// the centre so the cursor stays on-screen on the left column.
#define DUEL_MV_COL_X0 36
#define DUEL_MV_COL_X1 164
#define DUEL_MV_ROW_Y0 183
#define DUEL_MV_ROW_Y1 207
// Small cursor sits left of the active text: x - 10, y - 4.
#define DUEL_CUR_DX 10
#define DUEL_CUR_DY 4

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

// Play lobby rows: plain text list (no slot art) + cursor + name.
#define PLAY_TITLE_X 150
#define PLAY_TITLE_Y 8
#define PLAY_ROWS_Y 80
#define PLAY_ROW_PITCH 38
#define PLAY_ROWS_SHOWN 3
#define PLAY_SLOT_X 30
#define PLAY_SLOT_W 177
#define PLAY_SLOT_H 36
#define PLAY_CURSOR_X 12
#define PLAY_NAME_X 36
#define PLAY_NAME_W 220

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
