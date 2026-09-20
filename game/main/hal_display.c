#include "hal_display.h"
#include "esp_err.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"

#define LCD_MOSI 10
#define LCD_CLK 1
#define LCD_CS 2
#define LCD_DC 0
#define LCD_RST 4

void hal_display_init(void) {
  spi_bus_config_t bus = {.mosi_io_num = LCD_MOSI,
                          .miso_io_num = -1,
                          .sclk_io_num = LCD_CLK,
                          .quadwp_io_num = -1,
                          .quadhd_io_num = -1,
                          .max_transfer_sz = HAL_LCD_W * 40 * sizeof(uint16_t)};
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

  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
  const lvgl_port_display_cfg_t disp_cfg = {
      .io_handle = io,
      .panel_handle = panel,
      .buffer_size = HAL_LCD_W * 30,
      .double_buffer = true,
      .hres = HAL_LCD_W,
      .vres = HAL_LCD_H,
      .monochrome = false,
      .rotation = {.swap_xy = true, .mirror_x = true, .mirror_y = false},
      .color_format = LV_COLOR_FORMAT_RGB565,
      .flags = {.buff_dma = true, .swap_bytes = true},
  };
  assert(lvgl_port_add_disp(&disp_cfg));
}

void hal_display_reset(void) {
  if (!lvgl_port_lock(0)) return;
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x084841), LV_PART_MAIN);
  lvgl_port_unlock();
}
