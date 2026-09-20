#include "fault.h"
#include "hal_led.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>  // abort()

static const char *TAG = "fault";
static esp_reset_reason_t s_reason;

static const char *reason_str(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

static bool is_crash(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
      return true;
    default:
      return false;
  }
}

bool fault_was_crash(void) { return is_crash(s_reason); }

static void log_heap(void) {
  ESP_LOGE(TAG, "heap: free=%u  min-ever=%u (bytes, internal)",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

// Runs on the esp_restart() path (e.g. after abort()) in task context.
static void on_shutdown(void) {
  ESP_LOGW(TAG, "shutting down / restarting");
  log_heap();
}

void fault_init(void) {
  s_reason = esp_reset_reason();
  if (is_crash(s_reason)) {
    ESP_LOGE(TAG, "=========================================================");
    ESP_LOGE(TAG, " RECOVERED FROM A CRASH  (last reset: %s)", reason_str(s_reason));
    ESP_LOGE(TAG, " The panic backtrace was printed on the PREVIOUS boot,");
    ESP_LOGE(TAG, " just above this banner. Scroll up in the serial monitor.");
    ESP_LOGE(TAG, " Decode it with: idf.py monitor  (auto-decodes addresses)");
    log_heap();
    ESP_LOGE(TAG, "=========================================================");
  } else {
    ESP_LOGI(TAG, "boot ok (reset=%s)", reason_str(s_reason));
  }
  // Log heap + a note whenever we intentionally restart.
  esp_register_shutdown_handler(on_shutdown);
}

void fault_show_boot_banner(void) {
  if (!is_crash(s_reason)) return;  // clean boot: don't slow startup
  if (!lvgl_port_lock(0)) return;
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x400000), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "CRASH");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFF6060), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *body = lv_label_create(scr);
  char buf[96];
  snprintf(buf, sizeof(buf), "last reset:\n%s\n\nsee serial log\nfor backtrace",
           reason_str(s_reason));
  lv_label_set_text(body, buf);
  lv_obj_set_style_text_font(body, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(body, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(body, LV_ALIGN_CENTER, 0, 20);
  lvgl_port_unlock();

  // Hold so a reboot loop is readable instead of just flashing.
  hal_led_set_all(40, 0, 0);
  vTaskDelay(pdMS_TO_TICKS(4000));
}

void fault_fatal(const char *file, int line, const char *func, const char *fmt, ...) {
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  ESP_LOGE(TAG, "FATAL %s:%d (%s): %s", file, line, func, msg);
  log_heap();
  hal_led_set_all(80, 0, 0);  // solid red = fatal

  // Give the UART time to flush before the panic handler takes over.
  fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(50));
  abort();  // -> panic handler prints backtrace (see sdkconfig delay)
}
