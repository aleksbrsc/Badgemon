/*
 * blinker: ESP-NOW badge ping + LVGL screen + WS2812 blink.
 *
 * Press A -> broadcasts an ESP-NOW ping. Any badge flashed with this
 * firmware hears it, shows "ping from <mac> #<seq>" and bursts green.
 * No pairing, no network: flash same firmware on all badges, done.
 *
 * Pins/values from custom-firmware-hal.md (source of truth):
 *   LCD  MOSI=10 CLK=1 CS=2 DC=0 RST=4 (SPI2, 40MHz, mode 0, RGB565 320x240)
 *   BTNS HC165 DATA=7 LOAD=20 CLK=21, order A,B,Home,Down,Left,Right,Up,Aux1
 *        (active-low, A shifts out first). START=GPIO9 (separate).
 *   LEDS WS2812 x6 on GPIO3, GRB order (dim for AA power)
 *   WiFi STA channel 1, ESP-NOW broadcast (all badges stay on ch 1).
 *
 * Build/flash (ESP-IDF v6, esp32c3):
 *   source /opt/esp-idf/export.sh
 *   make build | make flash | make monitor   (see Makefile + README.md)
 * Download mode: hold START (GPIO9) while plugging in USB.
 * NOTE: flash needs --no-stub on this board (see Makefile).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "driver/gpio.h"
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
#define BTN_DATA 7
#define BTN_LOAD 20
#define BTN_CLK  21
#define LED_GPIO 3
#define LED_COUNT 6

#define LCD_W 320
#define LCD_H 240

static const char *TAG = "ping";

// --- ESP-NOW ping protocol ---
typedef struct {
  uint8_t mac[6];   // sender STA MAC
  uint32_t seq;     // sender sequence counter
} ping_pkt_t;

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t my_mac[6];
static uint32_t my_seq = 0;
static QueueHandle_t rx_queue;  // esp-now cb (wifi task) -> main loop

// Runs in WiFi task context: only copy + queue, no LVGL/LED here.
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(ping_pkt_t)) return;
  ping_pkt_t pkt;
  memcpy(&pkt, data, sizeof(pkt));
  // Ignore our own broadcast echo.
  if (memcmp(pkt.mac, my_mac, 6) == 0) return;
  xQueueSend(rx_queue, &pkt, 0);
  (void)info;
}

// --- HC165 buttons: bit i = 1 means pressed, bit 0 = A (shifts first) ---
static void delay_us(int us) { esp_rom_delay_us(us); }

static uint8_t read_shift_buttons(void) {
  gpio_set_level(BTN_LOAD, 0);
  delay_us(2);
  gpio_set_level(BTN_LOAD, 1);
  delay_us(2);
  uint8_t pressed = 0;
  for (int i = 0; i < 8; i++) {
    if (gpio_get_level(BTN_DATA) == 0) pressed |= (1 << i);
    gpio_set_level(BTN_CLK, 1);
    delay_us(2);
    gpio_set_level(BTN_CLK, 0);
    delay_us(2);
  }
  return pressed;
}

static void set_all(led_strip_handle_t strip, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < LED_COUNT; i++) led_strip_set_pixel(strip, i, r, g, b);
  led_strip_refresh(strip);
}

void app_main(void) {
  ESP_LOGI(TAG, "badge ping booting");
  rx_queue = xQueueCreate(8, sizeof(ping_pkt_t));
  assert(rx_queue);

  // ---- buttons ----
  gpio_config_t out = {.pin_bit_mask = (1ULL << BTN_LOAD) | (1ULL << BTN_CLK),
                       .mode = GPIO_MODE_OUTPUT};
  ESP_ERROR_CHECK(gpio_config(&out));
  gpio_config_t in = {.pin_bit_mask = (1ULL << BTN_DATA), .mode = GPIO_MODE_INPUT,
                      .pull_up_en = GPIO_PULLUP_ENABLE};
  ESP_ERROR_CHECK(gpio_config(&in));
  gpio_set_level(BTN_LOAD, 1);
  gpio_set_level(BTN_CLK, 0);

  // ---- LEDs ----
  led_strip_handle_t strip = NULL;
  led_strip_config_t strip_cfg = {
      .strip_gpio_num = LED_GPIO,
      .max_leds = LED_COUNT,
      .led_model = LED_MODEL_WS2812,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
      .flags.invert_out = false,
  };
  led_strip_rmt_config_t rmt_cfg = {.clk_src = RMT_CLK_SRC_DEFAULT,
                                    .resolution_hz = 10 * 1000 * 1000};
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
      .rotation = {.swap_xy = true, .mirror_x = true, .mirror_y = false},
      .color_format = LV_COLOR_FORMAT_RGB565,
      .flags = {.buff_dma = true, .swap_bytes = true},
  };
  assert(lvgl_port_add_disp(&disp_cfg));

  lv_obj_t *title = NULL;
  lv_obj_t *status = NULL;
  if (lvgl_port_lock(0)) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x084841), LV_PART_MAIN);
    title = lv_label_create(scr);
    lv_label_set_text(title, "hello htn");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);
    status = lv_label_create(scr);
    lv_label_set_text(status, "press A to ping");
    lv_obj_set_style_text_font(status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(status, lv_color_hex(0xB0FFB0), LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 40);
    lvgl_port_unlock();
  }

  // ---- WiFi + ESP-NOW (STA, channel 1, broadcast) ----
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
  ESP_ERROR_CHECK(esp_read_mac(my_mac, ESP_MAC_WIFI_STA));
  ESP_LOGI(TAG, "me %02X:%02X:%02X:%02X:%02X:%02X",
           my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
  esp_now_peer_info_t peer = {.channel = 1, .ifidx = WIFI_IF_STA, .encrypt = false};
  memcpy(peer.peer_addr, BROADCAST, 6);
  ESP_ERROR_CHECK(esp_now_add_peer(&peer));

  if (lvgl_port_lock(0)) {
    char me[32];
    snprintf(me, sizeof(me), "me %02X:%02X  press A to ping", my_mac[4], my_mac[5]);
    lv_label_set_text(status, me);
    lvgl_port_unlock();
  }

  // ---- main loop: 50 ms tick; A-press edge -> ping; rx -> label + burst ----
  bool last_a = false;
  uint32_t last_tx_ms = 0;
  int burst = 0;          // remaining green-burst ticks
  bool idle_on = false;   // idle red blink phase
  while (1) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // A button edge (debounced by 300 ms resend guard).
    bool a = (read_shift_buttons() & 0x01) != 0;
    if (a && !last_a && now - last_tx_ms > 300) {
      last_tx_ms = now;
      ping_pkt_t pkt;
      memcpy(pkt.mac, my_mac, 6);
      pkt.seq = ++my_seq;
      esp_err_t err = esp_now_send(BROADCAST, (uint8_t *)&pkt, sizeof(pkt));
      ESP_LOGI(TAG, "ping #%u sent (%s)", (unsigned)pkt.seq, esp_err_to_name(err));
      if (lvgl_port_lock(0)) {
        lv_label_set_text_fmt(status, "ping #%u sent!", (unsigned)pkt.seq);
        lvgl_port_unlock();
      }
    }
    last_a = a;

    // Incoming pings.
    ping_pkt_t rx;
    if (xQueueReceive(rx_queue, &rx, 0)) {
      ESP_LOGI(TAG, "ping #%u from %02X:%02X", (unsigned)rx.seq, rx.mac[4], rx.mac[5]);
      if (lvgl_port_lock(0)) {
        lv_label_set_text_fmt(status, "ping #%u from %02X:%02X!", (unsigned)rx.seq, rx.mac[4],
                              rx.mac[5]);
        lvgl_port_unlock();
      }
      burst = 6;  // 6 ticks x 50 ms = 300 ms green burst
    }

    // LEDs: green burst on receive, else idle dim-red blink.
    if (burst > 0) {
      burst--;
      set_all(strip, 0, 24, 0);
    } else {
      idle_on = !idle_on;
      if (idle_on)
        set_all(strip, 24, 0, 0);
      else
        set_all(strip, 0, 0, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
