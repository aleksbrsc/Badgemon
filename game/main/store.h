// Persistent settings in NVS namespace "badge". Survives reflash of the
// app (NVS partition untouched) and power loss.
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_system.h"

#define STORE_NAME_MAX 15  // chars, not counting NUL

void store_init(void);  // call after nvs_flash_init (net_init does it)
// Returns stored name, or "" if unset. Always NUL-terminated.
void store_get_name(char *out, size_t out_len);
bool store_set_name(const char *name);  // false on error

// Boot forensics: each boot increments a counter and records this boot's
// reset reason, so `boot` can tell a brownout/panic reboot ("kicked to
// menu by a reset") apart from an in-app nav glitch. Call once at startup.
void store_note_boot(esp_reset_reason_t reason);
void store_boot_info(uint32_t *count, esp_reset_reason_t *reason,
                     esp_reset_reason_t *prev_reason);
