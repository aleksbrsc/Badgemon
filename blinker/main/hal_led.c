#include "hal_led.h"
#include "esp_err.h"
#include "led_strip.h"

#define LED_GPIO 3

static led_strip_handle_t strip;

void hal_led_init(void) {
  led_strip_config_t cfg = {
      .strip_gpio_num = LED_GPIO,
      .max_leds = HAL_LED_COUNT,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
      .flags.invert_out = false,
  };
  led_strip_rmt_config_t rmt = {.clk_src = RMT_CLK_SRC_DEFAULT,
                                .resolution_hz = 10 * 1000 * 1000};
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &strip));
}

void hal_led_set_all(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < HAL_LED_COUNT; i++) led_strip_set_pixel(strip, i, r, g, b);
  led_strip_refresh(strip);
}

void hal_led_set_one(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i >= HAL_LED_COUNT) return;
  led_strip_set_pixel(strip, i, r, g, b);
}

void hal_led_show(void) { led_strip_refresh(strip); }
