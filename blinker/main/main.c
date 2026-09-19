/*
 * ping badge: single ESP-NOW ping app with button keyboard composer.
 * Controls: Up/Down digit, Left/Right cursor, A send, B clear, H demo.
 * Payload format: payload.h. See README.md.
 *
 * Pins/values from custom-firmware-hal.md (source of truth):
 *   LCD  MOSI=10 CLK=1 CS=2 DC=0 RST=4 (SPI2, 40MHz, mode 0, RGB565 320x240)
 *   BTNS HC165 DATA=7 LOAD=20 CLK=21, order A,B,Home,Down,Left,Right,Up,Aux1
 *        (active-low, A shifts first). START=GPIO9 (separate).
 *   LEDS WS2812 x6 on GPIO3, GRB order (dim for AA power)
 *   I2C  SDA=5 SCL=6 (accel 0x19, NFC 0x26) — debug console probes.
 *   WiFi STA channel 1, ESP-NOW broadcast.
 */
#include "hal_buttons.h"
#include "hal_led.h"
#include "hal_display.h"
#include "hal_i2c.h"
#include "net.h"
#include "store.h"
#include "nav.h"
#include "debug.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

void app_main(void) {
  ESP_LOGI(TAG, "ping badge booting");
  hal_buttons_init();
  hal_led_init();
  hal_display_init();
  hal_i2c_init();
  net_init();
  store_init();  // NVS ready (nvs_flash_init ran in net_init)
  debug_init();  // serial REPL over USB-Serial-JTAG
  nav_show(SCR_MENU);

  btn_event_t ev;
  while (1) {
    if (hal_buttons_poll(&ev)) ESP_LOGD(TAG, "btn edge a=%d b=%d home=%d down=%d left=%d right=%d up=%d",
                                        ev.a, ev.b, ev.home, ev.down, ev.left, ev.right, ev.up);
    nav_tick(xTaskGetTickCount() * portTICK_PERIOD_MS, &ev);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
