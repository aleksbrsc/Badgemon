// Minimal MFRC522 (I2C) driver: init + poll for a 4-byte-UID tag.
// Register map from the NXP MFRC522 datasheet. I2C framing: first byte
// is (reg << 1) | R/W, per datasheet section 9.3.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

typedef struct {
  i2c_master_bus_handle_t bus;
  i2c_master_dev_handle_t dev;
  bool open;
} mfrc522_t;

// Reset + init registers + antenna on, using the shared bus handle.
// False if the chip doesn't answer.
bool mfrc522_begin(mfrc522_t *m, i2c_master_bus_handle_t bus);
// Single poll: true once per tag present. Fills uid[4], atqa[2], sak.
bool mfrc522_scan(mfrc522_t *m, uint8_t uid[4], uint8_t atqa[2], uint8_t *sak);
// Antenna on/off (field draws sustained current — duty-cycle it).
void mfrc522_antenna(mfrc522_t *m, bool on);
// Antenna off + release the I2C bus.
void mfrc522_end(mfrc522_t *m);
