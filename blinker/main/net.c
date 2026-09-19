#include "net.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "net";
static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t my_mac[6];
static uint32_t seq = 0;
static QueueHandle_t rx_queue;

// WiFi task context: validate + queue only.
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  ping_msg_t msg;
  if (!ping_unpack(data, len, &msg)) {
    ESP_LOGD(TAG, "dropped malformed %d-byte packet", len);
    return;
  }
  if (memcmp(msg.mac, my_mac, 6) == 0) return;  // own echo
  xQueueSend(rx_queue, &msg, 0);
}

void net_init(void) {
  rx_queue = xQueueCreate(8, sizeof(ping_msg_t));
  assert(rx_queue);
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_channel(PING_CHANNEL, WIFI_SECOND_CHAN_NONE));
  ESP_ERROR_CHECK(esp_read_mac(my_mac, ESP_MAC_WIFI_STA));
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
  esp_now_peer_info_t peer = {.channel = PING_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false};
  memcpy(peer.peer_addr, BROADCAST, 6);
  ESP_ERROR_CHECK(esp_now_add_peer(&peer));
  ESP_LOGI(TAG, "ready as %02X:%02X:%02X:%02X:%02X:%02X", my_mac[0], my_mac[1], my_mac[2],
           my_mac[3], my_mac[4], my_mac[5]);
}

const uint8_t *net_mac(void) { return my_mac; }

esp_err_t net_send(const uint8_t *vals, uint8_t len, uint32_t *seq_out) {
  uint8_t buf[32];
  uint32_t s = ++seq;
  int n = ping_pack(buf, my_mac, s, vals, len);
  if (n == 0) return ESP_ERR_INVALID_ARG;
  esp_err_t err = esp_now_send(BROADCAST, buf, n);
  ESP_LOGI(TAG, "sent #%u (%s)", (unsigned)s, esp_err_to_name(err));
  if (seq_out) *seq_out = s;
  return err;
}

bool net_recv(ping_msg_t *msg) { return xQueueReceive(rx_queue, msg, 0) == pdTRUE; }
