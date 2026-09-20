#include "store.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "store";

void store_init(void) { ESP_LOGI(TAG, "ready"); }

void store_get_name(char *out, size_t out_len) {
  if (out_len == 0) return;
  out[0] = '\0';
  nvs_handle_t h;
  if (nvs_open("badge", NVS_READONLY, &h) != ESP_OK) {
    ESP_LOGD(TAG, "no namespace yet (fresh)");
    return;
  }
  size_t len = out_len;
  esp_err_t err = nvs_get_str(h, "name", out, &len);
  nvs_close(h);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGD(TAG, "name unset");
    out[0] = '\0';
  } else if (err != ESP_OK) {
    ESP_LOGW(TAG, "get name failed (%s)", esp_err_to_name(err));
    out[0] = '\0';
  } else {
    ESP_LOGD(TAG, "get name '%s'", out);
  }
}

bool store_set_name(const char *name) {
  nvs_handle_t h;
  if (nvs_open("badge", NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "open for write failed");
    return false;
  }
  char tmp[STORE_NAME_MAX + 1];
  strncpy(tmp, name ? name : "", sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  esp_err_t err = nvs_set_str(h, "name", tmp);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "set name '%s' (%s)", tmp, esp_err_to_name(err));
  return err == ESP_OK;
}

void store_note_boot(esp_reset_reason_t reason) {
  nvs_handle_t h;
  if (nvs_open("badge", NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "boot note: open failed");
    return;
  }
  uint32_t count = 0;
  nvs_get_u32(h, "boot_cnt", &count);  // missing on first boot -> 0
  int32_t prev = (int32_t)ESP_RST_UNKNOWN;
  nvs_get_i32(h, "boot_reason", &prev);  // last boot's reason, or unknown
  count++;
  esp_err_t err = nvs_set_u32(h, "boot_cnt", count);
  if (err == ESP_OK) err = nvs_set_i32(h, "prev_reason", prev);
  if (err == ESP_OK) err = nvs_set_i32(h, "boot_reason", (int32_t)reason);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(TAG, "boot #%u reason=%d (%s)", (unsigned)count, (int)reason, esp_err_to_name(err));
}

void store_boot_info(uint32_t *count, esp_reset_reason_t *reason,
                     esp_reset_reason_t *prev_reason) {
  if (count) *count = 0;
  if (reason) *reason = ESP_RST_UNKNOWN;
  if (prev_reason) *prev_reason = ESP_RST_UNKNOWN;
  nvs_handle_t h;
  if (nvs_open("badge", NVS_READONLY, &h) != ESP_OK) return;
  uint32_t c = 0;
  int32_t r = (int32_t)ESP_RST_UNKNOWN, p = (int32_t)ESP_RST_UNKNOWN;
  nvs_get_u32(h, "boot_cnt", &c);
  nvs_get_i32(h, "boot_reason", &r);
  nvs_get_i32(h, "prev_reason", &p);
  nvs_close(h);
  if (count) *count = c;
  if (reason) *reason = (esp_reset_reason_t)r;
  if (prev_reason) *prev_reason = (esp_reset_reason_t)p;
}
