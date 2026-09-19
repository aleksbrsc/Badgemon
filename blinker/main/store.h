// Persistent settings in NVS namespace "badge". Survives reflash of the
// app (NVS partition untouched) and power loss.
#pragma once

#include <stddef.h>
#include <stdbool.h>

#define STORE_NAME_MAX 15  // chars, not counting NUL

void store_init(void);  // call after nvs_flash_init (net_init does it)
// Returns stored name, or "" if unset. Always NUL-terminated.
void store_get_name(char *out, size_t out_len);
bool store_set_name(const char *name);  // false on error
