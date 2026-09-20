// Ping payload: typed packets marshalled to a byte array.
//
// Wire format (all multi-byte little-endian):
//   mac[6] | seq u32 | type u8 | len u8 | vals[len]
// Max on-air size: 6 + 4 + 1 + 1 + 32 = 44 bytes (ESP-NOW allows 250).
//
// Types: PING carries composer digits or message ASCII; DISCOVER asks
// nearby badges to announce; PRESENCE carries a display name;
// CHALLENGE carries target_mac[6] + challenger name; RESP carries
// target_mac[6] + accept u8 + responder name; DUEL carries target_mac[6]
// + turn u8 + move_idx u8 (deterministic lockstep, see ui_duel.c).
// Broadcast medium — "addressing" is by MAC inside the packet; everyone hears everything.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define PING_MAX_VALS 32
#define PING_CHANNEL 1

typedef enum {
  PKT_PING = 0,      // vals: digits or ASCII
  PKT_DISCOVER = 1,  // vals: sender display name
  PKT_PRESENCE = 2,  // vals: sender display name
  PKT_CHALLENGE = 3, // vals: target_mac[6] + challenger name
  PKT_RESP = 4,      // vals: target_mac[6] + accept u8 + responder name
  PKT_DUEL = 5,      // vals: target_mac[6] + turn u8 + move_idx u8
} pkt_type_t;

typedef struct {
  uint8_t mac[6];
  uint32_t seq;
  uint8_t type;
  uint8_t len;
  uint8_t vals[PING_MAX_VALS];
} ping_msg_t;

// Pack into buf, returns byte count (0 if vals overflow).
static inline int ping_pack(uint8_t *buf, const uint8_t *mac, uint32_t seq, uint8_t type,
                            const uint8_t *vals, uint8_t len) {
  if (len > PING_MAX_VALS) return 0;
  memcpy(buf, mac, 6);
  memcpy(buf + 6, &seq, 4);
  buf[10] = type;
  buf[11] = len;
  memcpy(buf + 12, vals, len);
  return 12 + len;
}

// Unpack from buf of size n. False on truncation/overflow.
static inline bool ping_unpack(const uint8_t *buf, int n, ping_msg_t *out) {
  if (n < 12) return false;
  memcpy(out->mac, buf, 6);
  memcpy(&out->seq, buf + 6, 4);
  out->type = buf[10];
  out->len = buf[11];
  if (out->len > PING_MAX_VALS || 12 + out->len > n) return false;
  memcpy(out->vals, buf + 12, out->len);
  return true;
}
