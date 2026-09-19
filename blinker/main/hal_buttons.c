#include "hal_buttons.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_rom_sys.h"

#define BTN_DATA 7
#define BTN_LOAD 20
#define BTN_CLK 21
#define BTN_START 9

static uint8_t last_state;

void hal_buttons_init(void) {
  gpio_config_t out = {.pin_bit_mask = (1ULL << BTN_LOAD) | (1ULL << BTN_CLK),
                       .mode = GPIO_MODE_OUTPUT};
  ESP_ERROR_CHECK(gpio_config(&out));
  gpio_config_t in = {.pin_bit_mask = (1ULL << BTN_DATA) | (1ULL << BTN_START),
                      .mode = GPIO_MODE_INPUT,
                      .pull_up_en = GPIO_PULLUP_ENABLE};
  ESP_ERROR_CHECK(gpio_config(&in));
  gpio_set_level(BTN_LOAD, 1);
  gpio_set_level(BTN_CLK, 0);
  last_state = 0;
}

uint8_t hal_buttons_read(void) {
  gpio_set_level(BTN_LOAD, 0);
  esp_rom_delay_us(2);
  gpio_set_level(BTN_LOAD, 1);
  esp_rom_delay_us(2);
  uint8_t pressed = 0;
  for (int i = 0; i < 8; i++) {
    if (gpio_get_level(BTN_DATA) == 0) pressed |= (1 << i);
    gpio_set_level(BTN_CLK, 1);
    esp_rom_delay_us(2);
    gpio_set_level(BTN_CLK, 0);
    esp_rom_delay_us(2);
  }
  return pressed;
}

bool hal_buttons_poll(btn_event_t *ev) {
  uint8_t pressed = hal_buttons_read();
  uint8_t edge = pressed & ~last_state;
  last_state = pressed;
  ev->a = edge & 0x01;
  ev->b = (edge >> 1) & 1;
  ev->home = (edge >> 2) & 1;
  ev->down = (edge >> 3) & 1;
  ev->left = (edge >> 4) & 1;
  ev->right = (edge >> 5) & 1;
  ev->up = (edge >> 6) & 1;
  return edge != 0;
}

bool hal_buttons_start(void) { return gpio_get_level(BTN_START) == 0; }
