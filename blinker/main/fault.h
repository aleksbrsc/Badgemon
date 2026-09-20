// Fault handling + crash diagnostics.
//
// Why this exists: panics reboot the chip (see sdkconfig), so a bug in
// the game loop shows up as "repeated flashing" — a boot loop where the
// backtrace scrolls past too fast to read. This module:
//   * reports the reset reason on every boot, loudly if it was a crash;
//   * paints the crash reason on the LCD and holds, so a reboot loop is
//     legible instead of just flashing;
//   * gives FATAL()/CHECK() to fail *with context* (file:line + message)
//     before aborting, so the panic backtrace is easy to interpret.
#pragma once

#include <stdbool.h>
#include "esp_log.h"

// Call as early as possible in app_main (before HAL init is fine).
void fault_init(void);

// Paint the last reset reason on the LCD. Call AFTER hal_display_init().
// No-op on a clean boot; on a crash it shows a red banner and blocks a
// few seconds so a reboot loop is readable.
void fault_show_boot_banner(void);

// True if the most recent reset was a crash (panic / WDT / brownout).
bool fault_was_crash(void);

// Log a fatal condition with source location + heap stats, then abort()
// (which triggers the panic handler -> backtrace). Use for broken
// invariants that must never happen.
#define FATAL(fmt, ...) \
  fault_fatal(__FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

// assert() that is always compiled in and logs context first.
#define CHECK(cond, fmt, ...)                                        \
  do {                                                               \
    if (!(cond))                                                     \
      FATAL("CHECK(%s) failed: " fmt, #cond, ##__VA_ARGS__);         \
  } while (0)

void fault_fatal(const char *file, int line, const char *func, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 4, 5)));
