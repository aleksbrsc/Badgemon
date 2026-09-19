// Shared I2C bus: SDA=5 SCL=6 400kHz (accel 0x19 + NFC 0x26).
// Owned here; clients add/remove their own device handles.
#pragma once

#include "driver/i2c_master.h"

#define HAL_I2C_SDA 5
#define HAL_I2C_SCL 6
#define HAL_NFC_ADDR 0x26
#define HAL_ACCEL_ADDR 0x19

void hal_i2c_init(void);
i2c_master_bus_handle_t hal_i2c_bus(void);  // NULL if init failed
