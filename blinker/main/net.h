// Networking: WiFi STA (channel 1) + ESP-NOW broadcast.
// Owns the RX queue and sequence counter; RX callback only unpacks
// (payload.h) and queues — all display/LED work stays in the UI layer.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "payload.h"

void net_init(void);
const uint8_t *net_mac(void);  // 6-byte STA MAC, valid after net_init
esp_err_t net_send(uint8_t type, const uint8_t *vals, uint8_t len, uint32_t *seq_out);
bool net_recv(ping_msg_t *msg);  // non-blocking drain of RX queue
