#include "lobby.h"
#include "net.h"
#include "store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "lobby";

static lobby_peer_t peers[LOBBY_MAX_PEERS];
static int n_peers = 0;

// 4-slot ring for incoming challenge/response events.
static lobby_event_t events[4];
static int ev_head = 0, ev_tail = 0, ev_count = 0;

void lobby_myname(char *out, int cap) {
  char saved[STORE_NAME_MAX + 1];
  store_get_name(saved, sizeof(saved));
  const uint8_t *mac = net_mac();
  if (saved[0])
    snprintf(out, cap, "%s", saved);
  else
    snprintf(out, cap, "%02X:%02X", mac[4], mac[5]);
}

static void push_event(const lobby_event_t *ev) {
  events[ev_tail] = *ev;
  ev_tail = (ev_tail + 1) % 4;
  if (ev_count < 4)
    ev_count++;
  else
    ev_head = (ev_head + 1) % 4;  // drop oldest
}

bool lobby_event(lobby_event_t *ev) {
  if (!ev_count) return false;
  *ev = events[ev_head];
  ev_head = (ev_head + 1) % 4;
  ev_count--;
  return true;
}

static void upsert(const uint8_t *mac, const char *name) {
  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  for (int i = 0; i < n_peers; i++) {
    if (!memcmp(peers[i].mac, mac, 6)) {
      strncpy(peers[i].name, name, LOBBY_NAME_MAX);
      peers[i].name[LOBBY_NAME_MAX] = '\0';
      peers[i].last_ms = now;
      ESP_LOGD(TAG, "peer refresh %02X:%02X '%s'", mac[4], mac[5], peers[i].name);
      return;
    }
  }
  if (n_peers >= LOBBY_MAX_PEERS) {
    // Evict stalest.
    int oldest = 0;
    for (int i = 1; i < n_peers; i++)
      if (peers[i].last_ms < peers[oldest].last_ms) oldest = i;
    peers[oldest] = peers[n_peers - 1];
    n_peers--;
  }
  memcpy(peers[n_peers].mac, mac, 6);
  strncpy(peers[n_peers].name, name, LOBBY_NAME_MAX);
  peers[n_peers].name[LOBBY_NAME_MAX] = '\0';
  peers[n_peers].last_ms = now;
  n_peers++;
  ESP_LOGI(TAG, "peer +%02X:%02X '%s' (%d total)", mac[4], mac[5], name, n_peers);
}

int lobby_list(lobby_peer_t *out, int cap) {
  uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
  int n = 0;
  for (int i = 0; i < n_peers && n < cap; i++) {
    if (now - peers[i].last_ms > LOBBY_PEER_TTL_MS) continue;
    out[n++] = peers[i];
  }
  return n;
}

// vals -> NUL-terminated name (vals are raw bytes, may be short).
static void vals_name(char *out, const uint8_t *vals, int len) {
  int n = len < LOBBY_NAME_MAX ? len : LOBBY_NAME_MAX;
  memcpy(out, vals, n);
  out[n] = '\0';
}

void lobby_refresh(void) {
  char me[LOBBY_NAME_MAX + 1];
  lobby_myname(me, sizeof(me));
  ESP_LOGI(TAG, "discover as '%s'", me);
  net_send(PKT_DISCOVER, (const uint8_t *)me, strlen(me), NULL);
}

void lobby_challenge(const uint8_t *mac) {
  char me[LOBBY_NAME_MAX + 1];
  lobby_myname(me, sizeof(me));
  uint8_t buf[6 + LOBBY_NAME_MAX + 1];
  memcpy(buf, mac, 6);  // target
  int nl = strlen(me);
  memcpy(buf + 6, me, nl);
  ESP_LOGI(TAG, "challenge %02X:%02X as '%s'", mac[4], mac[5], me);
  net_send(PKT_CHALLENGE, buf, 6 + nl, NULL);
}

void lobby_respond(const uint8_t *mac, bool accept) {
  char me[LOBBY_NAME_MAX + 1];
  lobby_myname(me, sizeof(me));
  uint8_t buf[6 + 1 + LOBBY_NAME_MAX + 1];
  memcpy(buf, mac, 6);  // challenger
  buf[6] = accept ? 1 : 0;
  int nl = strlen(me);
  memcpy(buf + 7, me, nl);
  ESP_LOGI(TAG, "respond %s to %02X:%02X", accept ? "accept" : "decline", mac[4], mac[5]);
  net_send(PKT_RESP, buf, 7 + nl, NULL);
}

static bool for_me(const uint8_t *target) { return !memcmp(target, net_mac(), 6); }

void lobby_tick(void) {
  ping_msg_t m;
  while (net_recv(&m)) {
    char me[LOBBY_NAME_MAX + 1];
    switch (m.type) {
      case PKT_DISCOVER:
        vals_name(me, m.vals, m.len);
        upsert(m.mac, me[0] ? me : "??");
        // Announce back so the discoverer lists us.
        lobby_myname(me, sizeof(me));
        net_send(PKT_PRESENCE, (const uint8_t *)me, strlen(me), NULL);
        break;
      case PKT_PRESENCE:
        vals_name(me, m.vals, m.len);
        upsert(m.mac, me[0] ? me : "??");
        break;
      case PKT_CHALLENGE:
        if (m.len >= 6 && for_me(m.vals)) {
          lobby_event_t ev = {.is_response = false};
          memcpy(ev.mac, m.mac, 6);
          vals_name(ev.name, m.vals + 6, m.len - 6);
          ESP_LOGI(TAG, "challenge from '%s'", ev.name);
          push_event(&ev);
        } else {
          ESP_LOGD(TAG, "challenge for someone else");
        }
        break;
      case PKT_RESP:
        if (m.len >= 7 && for_me(m.vals)) {
          lobby_event_t ev = {.is_response = true, .accept = m.vals[6] != 0};
          memcpy(ev.mac, m.mac, 6);
          vals_name(ev.name, m.vals + 7, m.len - 7);
          ESP_LOGI(TAG, "resp %s from '%s'", ev.accept ? "accept" : "decline", ev.name);
          push_event(&ev);
        }
        break;
      default:
        break;  // PKT_PING ignored here (hidden screens don't drain)
    }
  }
}
