// ST7789 320x240 (SPI2: MOSI=10 CLK=1 CS=2 DC=0 RST=4) + LVGL port.
#pragma once

#include "lvgl.h"

#define HAL_LCD_W 320
#define HAL_LCD_H 240

void hal_display_init(void);
void hal_display_reset(void);  // clear screen + teal bg (locks LVGL itself)
