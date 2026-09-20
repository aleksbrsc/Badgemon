idf_component_register(SRCS "main.c"
    "hal_buttons.c" "hal_led.c" "hal_display.c" "hal_i2c.c"
    "net.c" "store.c" "nav.c" "ui_menu.c" "ui_play.c" "lobby.c" "ui_ping.c" "ui_msg.c" "ui_settings.c"
    "ui_keyboard.c" "mfrc522.c" "debug.c"
  PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_driver_i2c esp_lcd
    esp_wifi esp_netif nvs_flash console)
