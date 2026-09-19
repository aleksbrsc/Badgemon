// Ping payload: a short list of values marshalled to a byte array.
//
// Wire format (all multi-byte little-endian):
//   mac[6] | seq u32 | len u8 | vals[len]
// Max on-air size: 6 + 4 + 1 + 8 = 19 bytes.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define PING_MAX_VALS 8
#define PING_CHANNEL 1

typedef struct {
  uint8_t mac[6];
  uint32_t seq;
  uint8_t len;
  uint8_t vals[PING_MAX_VALS];
} ping_msg_t;

// Pack into buf, returns byte count (0 if vals overflow).
static inline int ping_pack(uint8_t *buf, const uint8_t *mac, uint32_t seq,
                            const uint8_t *vals, uint8_t len) {
  if (len > PING_MAX_VALS) return 0;
  memcpy(buf, mac, 6);
  memcpy(buf + 6, &seq, 4);
  buf[10] = len;
  memcpy(buf + 11, vals, len);
  return 11 + len;
}

// Unpack from buf of size n. False on truncation/overflow.
static inline bool ping_unpack(const uint8_t *buf, int n, ping_msg_t *out) {
  if (n < 11) return false;
  memcpy(out->mac, buf, 6);
  memcpy(&out->seq, buf + 6, 4);
  out->len = buf[10];
  if (out->len > PING_MAX_VALS || 11 + out->len > n) return false;
  memcpy(out->vals, buf + 11, out->len);
  return true;
}
