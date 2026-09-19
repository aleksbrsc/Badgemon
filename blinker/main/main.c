/*
 * blinker: 6x WS2812 blink (GPIO3, dim red) + LVGL "hello htn" on ST7789.
 *
 * Pins/values from custom-firmware-hal.md (source of truth):
 *   LCD  MOSI=10 CLK=1 CS=2 DC=0 RST=4 (SPI2, 40MHz, mode 0, RGB565 320x240)
 *   LEDS WS2812 x6 on GPIO3, GRB order (dim for AA power)
 *
 * Build/flash (ESP-IDF v6, esp32c3):
 *   source /opt/esp-idf/export.sh
 *   make build | make flash | make monitor   (see Makefile + README.md)
 * Download mode: hold START (GPIO9) while plugging in USB.
 * NOTE: flash needs --no-stub on this board (see Makefile).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "led_strip.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define LCD_MOSI 10
#define LCD_CLK  1
#define LCD_CS   2
#define LCD_DC   0
#define LCD_RST  4
#define LED_GPIO 3
#define LED_COUNT 6

#define LCD_W 320
#define LCD_H 240

static const char *TAG = "blinker";

void app_main(void) {
  ESP_LOGI(TAG, "blinker booting");

  // ---- LEDs ----
  led_strip_handle_t strip = NULL;
  led_strip_config_t strip_cfg = {
      .strip_gpio_num = LED_GPIO,
      .max_leds = LED_COUNT,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
      .flags.invert_out = false,
  };
  led_strip_rmt_config_t rmt_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000,
  };
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip));

  // ---- LCD: SPI2 + ST7789 ----
  spi_bus_config_t bus = {.mosi_io_num = LCD_MOSI,
                          .miso_io_num = -1,
                          .sclk_io_num = LCD_CLK,
                          .quadwp_io_num = -1,
                          .quadhd_io_num = -1,
                          .max_transfer_sz = LCD_W * 40 * sizeof(uint16_t)};
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
  esp_lcd_panel_io_handle_t io = NULL;
  esp_lcd_panel_io_spi_config_t io_cfg = {.dc_gpio_num = LCD_DC,
                                          .cs_gpio_num = LCD_CS,
                                          .pclk_hz = 40 * 1000 * 1000,
                                          .spi_mode = 0,
                                          .trans_queue_depth = 10,
                                          .lcd_cmd_bits = 8,
                                          .lcd_param_bits = 8};
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io));
  esp_lcd_panel_handle_t panel = NULL;
  esp_lcd_panel_dev_config_t panel_cfg = {.reset_gpio_num = LCD_RST,
                                          .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                          .bits_per_pixel = 16};
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

  // ---- LVGL ----
  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
  const lvgl_port_display_cfg_t disp_cfg = {
      .io_handle = io,
      .panel_handle = panel,
      .buffer_size = LCD_W * 30,
      .double_buffer = true,
      .hres = LCD_W,
      .vres = LCD_H,
      .monochrome = false,
      .rotation =
          {
              .swap_xy = true,
              .mirror_x = true,
              .mirror_y = false,
          },
      .color_format = LV_COLOR_FORMAT_RGB565,
      .flags =
          {
              .buff_dma = true,
              .swap_bytes = true,
          },
  };
  lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
  assert(disp);

  if (lvgl_port_lock(0)) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x084841), LV_PART_MAIN);  // teal
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "hello htn");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(label);
    lvgl_port_unlock();
  }
  ESP_LOGI(TAG, "screen: hello htn shown (LVGL)");

  // ---- blink loop (LVGL runs in its own task) ----
  bool on = false;
  while (1) {
    on = !on;
    for (int i = 0; i < LED_COUNT; i++)
      ESP_ERROR_CHECK(led_strip_set_pixel(strip, i, on ? 24 : 0, 0, 0));
    ESP_ERROR_CHECK(led_strip_refresh(strip));
    ESP_LOGI(TAG, "%s", on ? "on" : "off");
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
