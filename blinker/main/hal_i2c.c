#include "hal_i2c.h"
#include "esp_log.h"

static const char *TAG = "hal_i2c";
static i2c_master_bus_handle_t bus;

void hal_i2c_init(void) {
  i2c_master_bus_config_t cfg = {.i2c_port = 0,
                                 .sda_io_num = HAL_I2C_SDA,
                                 .scl_io_num = HAL_I2C_SCL,
                                 .clk_source = I2C_CLK_SRC_DEFAULT,
                                 .glitch_ignore_cnt = 7,
                                 .flags.enable_internal_pullup = true};
  if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
    ESP_LOGW(TAG, "bus init failed; i2c clients unavailable");
    bus = NULL;
  }
}

i2c_master_bus_handle_t hal_i2c_bus(void) { return bus; }
