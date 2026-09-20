#include "hal_accel.h"
#include "hal_i2c.h"
#include "esp_log.h"

static const char *TAG = "accel";

// SC7A20 (badge HAL): same register layout as LIS3DH family subset.
#define REG_WHO_AM_I 0x0F
#define REG_CTRL1 0x20
#define REG_CTRL4 0x23
#define REG_STATUS 0x27
#define REG_OUT_X_L 0x28
#define WHO_AM_I_SC7A20 0x11
#define AUTO_INC 0x80
#define I2C_TIMEOUT_MS 50

static i2c_master_dev_handle_t dev;
static bool ok;
static int prev_mg = 1000;
static bool prev_mg_valid;

static esp_err_t reg_write(uint8_t reg, uint8_t val) {
  if (!dev) return ESP_ERR_INVALID_STATE;
  uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *val, int n) {
  if (!dev) return ESP_ERR_INVALID_STATE;
  uint8_t r = (n > 1) ? (uint8_t)(reg | AUTO_INC) : reg;
  esp_err_t err = i2c_master_transmit(dev, &r, 1, I2C_TIMEOUT_MS);
  if (err != ESP_OK) return err;
  return i2c_master_receive(dev, val, n, I2C_TIMEOUT_MS);
}

void hal_accel_init(void) {
  ok = false;
  dev = NULL;
  prev_mg_valid = false;
  i2c_master_bus_handle_t bus = hal_i2c_bus();
  if (!bus) {
    ESP_LOGW(TAG, "no i2c bus; accel disabled");
    return;
  }
  i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                             .device_address = HAL_ACCEL_ADDR,
                             .scl_speed_hz = 400000};
  if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
    ESP_LOGW(TAG, "add device failed; accel disabled");
    dev = NULL;
    return;
  }
  uint8_t who = 0;
  if (reg_read(REG_WHO_AM_I, &who, 1) != ESP_OK) {
    ESP_LOGW(TAG, "WHO_AM_I read failed; accel disabled");
    return;
  }
  if (who != WHO_AM_I_SC7A20) {
    ESP_LOGW(TAG, "unexpected WHO_AM_I 0x%02x (want 0x%02x); accel disabled",
             who, WHO_AM_I_SC7A20);
    return;
  }
  // custom-firmware-hal.md: 100 Hz, axes on; BDU, +-2g.
  if (reg_write(REG_CTRL1, 0x57) != ESP_OK) return;
  if (reg_write(REG_CTRL4, 0x80) != ESP_OK) return;
  ok = true;
  ESP_LOGI(TAG, "SC7A20 ready at 0x%02x", HAL_ACCEL_ADDR);
}

bool hal_accel_ok(void) { return ok; }

bool hal_accel_read(int16_t *x, int16_t *y, int16_t *z) {
  if (!ok) return false;
  uint8_t st = 0;
  (void)reg_read(REG_STATUS, &st, 1);
  uint8_t b[6];
  if (reg_read(REG_OUT_X_L, b, 6) != ESP_OK) return false;
  int16_t rx = (int16_t)((b[1] << 8) | b[0]);
  int16_t ry = (int16_t)((b[3] << 8) | b[2]);
  int16_t rz = (int16_t)((b[5] << 8) | b[4]);
  if (x) *x = (int16_t)(rx >> 4);
  if (y) *y = (int16_t)(ry >> 4);
  if (z) *z = (int16_t)(rz >> 4);
  return true;
}

static uint32_t isqrt32(uint32_t v) {
  uint32_t r = 0, bit = 1u << 30;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

int hal_accel_magnitude_mg(void) {
  int16_t x, y, z;
  if (!hal_accel_read(&x, &y, &z)) return -1;
  uint32_t s = (uint32_t)((int32_t)x * x + (int32_t)y * y + (int32_t)z * z);
  return (int)isqrt32(s);
}

static int activity_mg(int mg) {
  int dev = mg - 1000;
  if (dev < 0) dev = -dev;
  int jerk = 0;
  if (prev_mg_valid) {
    jerk = mg - prev_mg;
    if (jerk < 0) jerk = -jerk;
  }
  prev_mg = mg;
  prev_mg_valid = true;
  int act = dev;
  if (jerk > act) act = jerk;
  return act;
}

accel_motion_t hal_accel_motion(int *activity_mg_out) {
  int mg = hal_accel_magnitude_mg();
  if (mg < 0) {
    if (activity_mg_out) *activity_mg_out = -1;
    return ACCEL_MOTION_NONE;
  }
  int act = activity_mg(mg);
  if (activity_mg_out) *activity_mg_out = act;
  if (act < 140) return ACCEL_MOTION_NONE;
  if (act < 280) return ACCEL_MOTION_SLOW;
  if (act < 420) return ACCEL_MOTION_MEDIUM;
  return ACCEL_MOTION_FAST;
}

bool hal_accel_shake(accel_shake_t *s, uint32_t now_ms, int thresh_mg,
                     uint32_t cooldown_ms) {
  if (!s || !ok) return false;
  if (s->last_fire != 0 && now_ms - s->last_fire < cooldown_ms) return false;
  int act = 0;
  accel_motion_t m = hal_accel_motion(&act);
  if (act < 0) return false;
  if (m >= ACCEL_MOTION_FAST || act >= thresh_mg) {
    s->last_fire = now_ms ? now_ms : 1;
    return true;
  }
  return false;
}
