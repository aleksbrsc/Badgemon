#include "hal_buttons.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BTN_DATA 7
#define BTN_LOAD 20
#define BTN_CLK 21
#define BTN_START 9

static const char *TAG = "hal_btn";

static uint8_t stable_state;  // last confirmed (debounced twice) reading
static uint8_t last_raw;
static uint8_t confirm;  // consecutive identical raw reads (saturates)
static uint8_t start_hist;  // 2-bit history of START samples, 1=pressed
static uint32_t start_inject_until;  // debug hold override (tick time, ms)

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
  stable_state = 0;
  last_raw = 0;
  confirm = 0;
  start_hist = 0;
  start_inject_until = 0;
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
  // Debounce: a single glitched shift-register read (e.g. rail droop
  // during an ESP-NOW TX burst) must not become a phantom press. An edge
  // is only accepted after 2 consecutive identical raw reads (~40 ms).
  uint8_t raw = hal_buttons_read();
  if (raw == last_raw) {
    if (confirm < 2) confirm++;
  } else {
    last_raw = raw;
    confirm = 0;
  }
  bool any = false;
  if (confirm >= 1) {
    uint8_t edge = last_raw & ~stable_state;
    stable_state = last_raw;
    confirm = 0;  // re-arm: next edge needs a fresh pair
    ev->a = edge & 0x01;
    ev->b = (edge >> 1) & 1;
    ev->home = (edge >> 2) & 1;
    ev->down = (edge >> 3) & 1;
    ev->left = (edge >> 4) & 1;
    ev->right = (edge >> 5) & 1;
    ev->up = (edge >> 6) & 1;
    any = edge != 0;
  } else {
    ev->a = ev->b = ev->home = ev->down = ev->left = ev->right = ev->up = false;
  }
  // Track START samples alongside (see hal_buttons_start).
  start_hist = (start_hist << 1) | (gpio_get_level(BTN_START) == 0 ? 1 : 0);
  return any;
}

bool hal_buttons_start(void) {
  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if (now < start_inject_until) return true;  // debug override
  // Debounced: pressed only after 2 consecutive pressed samples, so a
  // one-sample droop glitch can't fake a save/cancel in the keyboard.
  return (start_hist & 0x03) == 0x03;
}

void hal_buttons_inject_start(uint32_t ms) {
  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  start_inject_until = now + ms;
  ESP_LOGI(TAG, "start injected for %u ms", (unsigned)ms);
}
