/*
 * blinker: 6x WS2812 blink (GPIO3, dim red) + "hello world" on ST7789.
 *
 * Pins/values from custom-firmware-hal.md (source of truth):
 *   LCD  MOSI=10 CLK=1 CS=2 DC=0 RST=4 (SPI2, 40MHz, mode 0, RGB565 320x240)
 *   LEDS WS2812 x6 on GPIO3, GRB order (dim for AA power)
 *
 * Build/flash (ESP-IDF v6, esp32c3):
 *   source /opt/esp-idf/export.sh
 *   make build | make flash | make monitor   (see Makefile)
 * Download mode: hold START (GPIO9) while plugging in USB.
 * NOTE: flash needs --no-stub on this board (see Makefile).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "led_strip.h"

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

// --- tiny 5x7 font, only glyphs needed for "hello world" ---
// Each glyph: 7 rows of 5 pixels, '#' = ink.
static const char *GLYPH_h[7] = {"#....", "#....", "#....", "####.", "#...#", "#...#", "#...#"};
static const char *GLYPH_e[7] = {".....", ".###.", "#...#", "#####", "#....", ".###.", "....."};
static const char *GLYPH_l[7] = {"..#..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."};
static const char *GLYPH_o[7] = {".....", ".###.", "#...#", "#...#", "#...#", ".###.", "....."};
static const char *GLYPH_w[7] = {".....", ".....", "#...#", "#...#", "#.#.#", "##.##", "#...#"};
static const char *GLYPH_r[7] = {".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."};
static const char *GLYPH_d[7] = {"...#.", "...#.", "...#.", ".####", "#...#", "#...#", ".###."};
static const char *GLYPH_space[7] = {".....", ".....", ".....", ".....", ".....", ".....", "....."};

static const char **glyph_for(char c) {
  switch (c) {
    case 'h': return GLYPH_h;
    case 'e': return GLYPH_e;
    case 'l': return GLYPH_l;
    case 'o': return GLYPH_o;
    case 'w': return GLYPH_w;
    case 'r': return GLYPH_r;
    case 'd': return GLYPH_d;
    default: return GLYPH_space;
  }
}

#define SCALE 4
#define MSG "hello world"
#define MSG_LEN 11
#define CHAR_ADV (5 * SCALE + SCALE)          // 5px glyph + 1px*SCALE gap
#define TEXT_W (MSG_LEN * CHAR_ADV - SCALE)   // 260
#define TEXT_H (7 * SCALE)                    // 28

#define COLOR_BG 0x0410    // dim teal
#define COLOR_FG 0xFFFF    // white

// Framebuffer for the text box: 260x28 RGB565 (~14.6 KB, static to spare stack).
static uint16_t text_fb[TEXT_W * TEXT_H];

static void render_text(void) {
  for (int i = 0; i < TEXT_W * TEXT_H; i++) text_fb[i] = COLOR_BG;
  for (int ci = 0; ci < MSG_LEN; ci++) {
    const char **g = glyph_for(MSG[ci]);
    int ox = ci * CHAR_ADV;
    for (int row = 0; row < 7; row++) {
      for (int col = 0; col < 5; col++) {
        if (g[row][col] != '#') continue;
        for (int dy = 0; dy < SCALE; dy++) {
          for (int dx = 0; dx < SCALE; dx++) {
            int x = ox + col * SCALE + dx;
            int y = row * SCALE + dy;
            text_fb[y * TEXT_W + x] = COLOR_FG;
          }
        }
      }
    }
  }
}

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
                          .max_transfer_sz = 32 * 320 * sizeof(uint16_t)};
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

  // Teal background, 32-row stripes (no full-frame DMA buffer).
  static uint16_t stripe[32 * 320];
  for (int i = 0; i < 32 * 320; i++) stripe[i] = COLOR_BG;
  for (int y = 0; y < LCD_H; y += 32)
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + 32, stripe));

  // "hello world", centered.
  render_text();
  int tx = (LCD_W - TEXT_W) / 2;
  int ty = (LCD_H - TEXT_H) / 2;
  ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, tx, ty, tx + TEXT_W, ty + TEXT_H, text_fb));
  ESP_LOGI(TAG, "screen: hello world shown");

  // ---- blink loop ----
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
