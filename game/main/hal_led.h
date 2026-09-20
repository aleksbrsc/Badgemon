// 6x WS2812B on GPIO3, GRB order. Keep values modest (AA power).
#pragma once

#include <stdint.h>

#define HAL_LED_COUNT 6

void hal_led_init(void);
void hal_led_set_all(uint8_t r, uint8_t g, uint8_t b);
void hal_led_set_one(int i, uint8_t r, uint8_t g, uint8_t b);
void hal_led_show(void);
