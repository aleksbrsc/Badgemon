// Lobby: nearby-player discovery + challenge routing over broadcast.
//
// Flow: refresh() broadcasts DISCOVER; every badge auto-replies with
// PRESENCE (its display name); peers accumulate by MAC. Challenge a
// peer by MAC; only that badge raises the dialog. Responses route back
// the same way. The medium is broadcast — "addressing" is inside the
// packet (payload.h). Call lobby_tick() often (drains net queue).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "payload.h"

#define LOBBY_MAX_PEERS 8
#define LOBBY_NAME_MAX 15
// Peers older than this vanish from the list (ms).
#define LOBBY_PEER_TTL_MS 15000

typedef struct {
  uint8_t mac[6];
  char name[LOBBY_NAME_MAX + 1];  // display name, or "AB:CD" fallback
  uint32_t last_ms;
} lobby_peer_t;

typedef struct {
  bool is_response;  // false = incoming challenge, true = response to mine
  uint8_t mac[6];    // challenger (challenge) or responder (response)
  char name[LOBBY_NAME_MAX + 1];
  bool accept;  // valid when is_response
} lobby_event_t;

// My display name: settings name, or MAC-tail fallback.
void lobby_myname(char *out, int cap);
void lobby_refresh(void);  // broadcast DISCOVER
int lobby_list(lobby_peer_t *out, int cap);
void lobby_challenge(const uint8_t *mac);       // challenge one peer
void lobby_respond(const uint8_t *mac, bool accept);  // answer a challenge
bool lobby_event(lobby_event_t *ev);  // incoming challenge/response for me
void lobby_tick(void);
