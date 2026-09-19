idf_component_register(SRCS "main.c"
  PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_lcd
    esp_wifi esp_netif nvs_flash)
