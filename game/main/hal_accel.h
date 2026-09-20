// SC7A20 accelerometer on the shared I2C bus (0x19). See custom-firmware-hal.md.
//
// SAFETY: every call is a no-op / returns false if the device is missing
// or an I2C transfer fails. Nothing here blocks longer than a short I2C
// timeout, so it is safe to poll from the main loop each tick.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void hal_accel_init(void);
bool hal_accel_ok(void);  // present + configured

// Raw signed axis counts (~1 mg per count at +-2g). False on error.
bool hal_accel_read(int16_t *x, int16_t *y, int16_t *z);

// Vector magnitude in milli-g (~1000 at rest). Returns -1 on error/absent.
int hal_accel_magnitude_mg(void);

// Motion intensity from deviation from 1 g and sample-to-sample jerk.
typedef enum {
  ACCEL_MOTION_NONE = 0,
  ACCEL_MOTION_SLOW,
  ACCEL_MOTION_MEDIUM,
  ACCEL_MOTION_FAST,
} accel_motion_t;

// `activity_mg_out` optional: max(|mag-1000|, jerk) used for the level.
accel_motion_t hal_accel_motion(int *activity_mg_out);

// Debounced shake: fires once when activity exceeds `thresh_mg`, then quiet
// for `cooldown_ms`. Zero-initialize accel_shake_t.
typedef struct {
  uint32_t last_fire;
} accel_shake_t;

bool hal_accel_shake(accel_shake_t *s, uint32_t now_ms, int thresh_mg,
                     uint32_t cooldown_ms);
