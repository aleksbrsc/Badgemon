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
LV_IMAGE_DECLARE(assets_menu_bg);
LV_IMAGE_DECLARE(assets_party_screen);
LV_IMAGE_DECLARE(assets_loading);
LV_IMAGE_DECLARE(assets_bubble);
LV_IMAGE_DECLARE(assets_speech_half);
LV_IMAGE_DECLARE(assets_caption);
LV_IMAGE_DECLARE(assets_caption_half);
LV_IMAGE_DECLARE(assets_slot);
LV_IMAGE_DECLARE(assets_cursor);
LV_IMAGE_DECLARE(assets_cursor_white);
LV_IMAGE_DECLARE(assets_cursor_sm);
LV_IMAGE_DECLARE(assets_cursor_white_sm);
LV_IMAGE_DECLARE(assets_vinyl_back_1);
LV_IMAGE_DECLARE(assets_vinyl_back_2);
LV_IMAGE_DECLARE(assets_vinyl_front_1);
LV_IMAGE_DECLARE(assets_vinyl_front_2);
LV_IMAGE_DECLARE(assets_pokeball);
LV_IMAGE_DECLARE(assets_pokeball_open);
LV_IMAGE_DECLARE(assets_pokeball_foe);
LV_IMAGE_DECLARE(assets_pokeball_open_foe);
LV_IMAGE_DECLARE(assets_ginny_back_1);
LV_IMAGE_DECLARE(assets_ginny_back_2);
LV_IMAGE_DECLARE(assets_ginny_front_1);
LV_IMAGE_DECLARE(assets_ginny_front_2);
LV_IMAGE_DECLARE(assets_patchy_back_1);
LV_IMAGE_DECLARE(assets_patchy_back_2);
LV_IMAGE_DECLARE(assets_patchy_front_1);
LV_IMAGE_DECLARE(assets_patchy_front_2);

// Vinyl battle sprites (2-frame send-out); frame positions from art layout.
#define DUEL_VINYL_BACK_X1 66
#define DUEL_VINYL_BACK_X2 70
#define DUEL_VINYL_BACK_Y 109
#define DUEL_VINYL_FOE_X1 208
#define DUEL_VINYL_FOE_X2 210
#define DUEL_VINYL_FOE_Y 39
#define DUEL_VINYL_POKEBALL_ME_X 79
#define DUEL_VINYL_POKEBALL_ME_Y 153
#define DUEL_VINYL_POKEBALL_FOE_X 218
#define DUEL_VINYL_POKEBALL_FOE_Y 83
// Ginny (Squirtle slot) battle sprites.
#define DUEL_GINNY_BACK_X 78
#define DUEL_GINNY_BACK_Y 104
#define DUEL_GINNY_FOE_X 213
#define DUEL_GINNY_FOE_Y 42
#define DUEL_GINNY_POKEBALL_ME_X 88
#define DUEL_GINNY_POKEBALL_ME_Y 154
#define DUEL_GINNY_POKEBALL_FOE_X 218
#define DUEL_GINNY_POKEBALL_FOE_Y 85
// Patch (Bulbasaur slot) battle sprites.
#define DUEL_PATCH_BACK_X 75
#define DUEL_PATCH_BACK_Y 121
#define DUEL_PATCH_FOE_X 204
#define DUEL_PATCH_FOE_Y 42
#define DUEL_PATCH_POKEBALL_ME_X 85
#define DUEL_PATCH_POKEBALL_ME_Y 165
#define DUEL_PATCH_POKEBALL_FOE_X 214
#define DUEL_PATCH_POKEBALL_FOE_Y 85

// Duel scene (assets_duel_bg): baked plates + dark dialog strip.
#define DUEL_BG_W 320
#define DUEL_BG_H 240
// Enemy (top-left) plate: name + numbers + fill bar over baked HP bar.
#define DUEL_FOE_NAME_X 26
#define DUEL_FOE_NAME_Y 27
#define DUEL_FOE_LEVEL_X 78
#define DUEL_FOE_LEVEL_Y 27
#define DUEL_FOE_LEVEL_W 44
#define DUEL_FOE_HP_X 26
#define DUEL_FOE_HP_Y 36
#define DUEL_FOE_BAR_X 69
#define DUEL_FOE_BAR_Y 47
#define DUEL_FOE_BAR_W 57
#define DUEL_FOE_BAR_H 3
// Player (bottom-right) plate. Bar extends slightly past the baked
// art so the fill reads clearly.
#define DUEL_ME_NAME_X 195
#define DUEL_ME_NAME_Y 117
#define DUEL_ME_LEVEL_X 192
#define DUEL_ME_LEVEL_Y 117
#define DUEL_ME_LEVEL_W 98
#define DUEL_ME_HP_X 192
#define DUEL_ME_HP_Y 144
#define DUEL_ME_HP_W 98
#define DUEL_ME_BAR_X 234
#define DUEL_ME_BAR_Y 135
#define DUEL_ME_BAR_W 60
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
// Col 1 sits far enough right that BADGEMON's N clears RUN by ~2
// letters; both cols sit right enough that the cursor (x - 10)
// clears the speech bubble's left edge.
#define DUEL_CMD_COL_X0 198
#define DUEL_CMD_COL_X1 278
#define DUEL_CMD_ROW_Y0 189
#define DUEL_CMD_ROW_Y1 213
// Move list (inside the full caption): 2 cols x 2 rows, nudged toward
// the centre so the cursor stays on-screen on the left column.
#define DUEL_MV_COL_X0 36
#define DUEL_MV_COL_X1 164
// Move grid: slightly below command rows but not as low as the first
// nudge (between the old 183 and 189 positions).
#define DUEL_MV_ROW_Y0 186
#define DUEL_MV_ROW_Y1 210
// PP text sits just right of the move name on the same row.
#define DUEL_MV_PP_GAP 8
// Cursor sits left of the active text; command menu uses a tighter gap.
#define DUEL_CMD_CUR_DX 10
#define DUEL_MV_CUR_DX 18
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

// Menu screen on assets_menu_bg (title art baked in, footer baked in).
#define MENU_TITLE_Y 88
#define MENU_BTNS_X 40
#define MENU_BTNS_W 240
#define MENU_BTNS_Y 106
#define MENU_BTN_H 34

// Party screen on assets_party_screen (updated art): current mon in
// the top-left light-blue panel (name above, fill bar + number over the
// baked HP trough), a bottom white dialog bar for the prompt, and a
// purple CANCEL tab bottom-right. Coords measured from party-screen.png.
#define PARTY2_NAME_X 52
#define PARTY2_NAME_Y 34
#define PARTY2_HP_X 70
#define PARTY2_HP_Y 62
#define PARTY2_HP_W 58
#define PARTY2_BAR_X 69
#define PARTY2_BAR_Y 55
#define PARTY2_BAR_W 58
#define PARTY2_BAR_H 4
#define PARTY2_SLOT_X 138
#define PARTY2_SLOT_Y 8
#define PARTY2_SLOT_PITCH 40
// Bottom white dialog bar prompt ("Choose a Badgemon.").
#define PARTY2_DLG_X 16
#define PARTY2_DLG_Y 212
#define PARTY2_DLG_W 232
// Purple CANCEL tab (white text, centered in the purple rect).
#define PARTY2_CANCEL_X 261
#define PARTY2_CANCEL_Y 212
#define PARTY2_CANCEL_W 50

// Battle intro wipe: two full-width black bars, 120px tall each.
#define INTRO_BAR_H 120
#define INTRO_MS 2000
