// Debug REPL over USB-Serial-JTAG: `make monitor`, type `help`.
//
// Triage for "nfc: reader not found":
//   1. `i2c_scan` — is anything on the bus? Expect 0x19 (accel) always,
//      0x26 (NFC) when powered. Neither => bus wedged (low battery) or
//      wiring. Only 0x19 => NFC chip has no power.
//   2. `nfc_ver` — VersionReg should read 0x91/0x92. 0x00/0xFF or timeout
//      => chip in reset / brown-out. Try USB power or fresh AAs.
//   3. `nfc_regs` — dump all registers; compare against a good boot.
//   4. `nfc_scan` — hold a tag at the reader, run a single poll.
#include "debug.h"
#include "hal_buttons.h"
#include "hal_i2c.h"
#include "net.h"
#include "payload.h"
#include "mfrc522.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "dbg";

// Temp device handle for one-shot probes (scan/ver/reg).
static bool probe_dev(uint8_t addr, i2c_master_dev_handle_t *out) {
  if (!hal_i2c_bus()) {
    printf("no i2c bus (init failed at boot)\n");
    return false;
  }
  i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                             .device_address = addr,
                             .scl_speed_hz = 400000};
  if (i2c_master_bus_add_device(hal_i2c_bus(), &cfg, out) != ESP_OK) {
    printf("add device 0x%02X failed\n", addr);
    return false;
  }
  return true;
}

static int cmd_i2c_scan(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (!hal_i2c_bus()) {
    printf("no i2c bus\n");
    return 1;
  }
  printf("probing 0x08..0x77 (expect 0x19 accel, 0x26 nfc):\n");
  for (uint8_t a = 0x08; a < 0x78; a++) {
    if (i2c_master_probe(hal_i2c_bus(), a, 50) == ESP_OK) {
      const char *who = a == HAL_ACCEL_ADDR ? " (accel)" : a == HAL_NFC_ADDR ? " (nfc)" : "";
      printf("  found 0x%02X%s\n", a, who);
    }
  }
  return 0;
}

static bool nfc_read(uint8_t reg, uint8_t *val) {
  i2c_master_dev_handle_t dev;
  if (!probe_dev(HAL_NFC_ADDR, &dev)) return false;
  // Standard framing: plain reg byte, STOP, then separate read.
  esp_err_t err = i2c_master_transmit(dev, &reg, 1, 100);
  if (err == ESP_OK) err = i2c_master_receive(dev, val, 1, 100);
  i2c_master_bus_rm_device(dev);
  return err == ESP_OK;
}

static int cmd_nfc_ver(int argc, char **argv) {
  (void)argc;
  (void)argv;
  uint8_t ver = 0;
  if (!nfc_read(0x37, &ver)) {
    printf("VersionReg read FAILED (timeout/nack)\n");
    return 1;
  }
  printf("VersionReg = 0x%02X (want 0x91/0x92)\n", ver);
  return (ver == 0x91 || ver == 0x92) ? 0 : 1;
}

static int cmd_nfc_reg(int argc, char **argv) {
  if (argc != 2) {
    printf("usage: nfc_reg <hex addr>, e.g. nfc_reg 37\n");
    return 1;
  }
  uint8_t reg = (uint8_t)strtoul(argv[1], NULL, 16);
  uint8_t val = 0;
  if (!nfc_read(reg, &val)) {
    printf("reg 0x%02X read FAILED\n", reg);
    return 1;
  }
  printf("reg 0x%02X = 0x%02X\n", reg, val);
  return 0;
}

static int cmd_nfc_regs(int argc, char **argv) {
  (void)argc;
  (void)argv;
  printf("MFRC522 reg dump (0x09=FIFO, reads consume a byte):\n");
  for (uint8_t r = 0; r < 0x40; r++) {
    uint8_t val = 0;
    bool ok = nfc_read(r, &val);
    printf("  0x%02X: %s0x%02X%s", r, ok ? "" : "FAIL ", val, (r % 4 == 3) ? "\n" : "  ");
  }
  printf("\n");
  return 0;
}

static mfrc522_t dbg_nfc;  // owned by nfc_begin/nfc_end/nfc_scan

static int cmd_nfc_begin(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (dbg_nfc.open) mfrc522_end(&dbg_nfc);
  printf("running full init (reset, timers, antenna on)...\n");
  printf(mfrc522_begin(&dbg_nfc, hal_i2c_bus()) ? "init OK (antenna left ON)\n" : "init FAILED\n");
  return 0;
}

static int cmd_nfc_end(int argc, char **argv) {
  (void)argc;
  (void)argv;
  mfrc522_end(&dbg_nfc);  // antenna off
  printf("antenna off, device released\n");
  return 0;
}

static int cmd_nfc_scan(int argc, char **argv) {
  (void)argc;
  (void)argv;
  mfrc522_t m;
  if (!mfrc522_begin(&m, hal_i2c_bus())) {
    printf("init FAILED, no scan attempted\n");
    return 1;
  }
  uint8_t uid[4], atqa[2], sak = 0;
  bool found = mfrc522_scan(&m, uid, atqa, &sak);
  mfrc522_end(&m);
  if (found)
    printf("TAG uid %02X:%02X:%02X:%02X atqa %02X%02X sak %02X\n", uid[0], uid[1], uid[2],
           uid[3], atqa[0], atqa[1], sak);
  else
    printf("no tag (hold a card at the reader and retry)\n");
  return found ? 0 : 1;
}

static const char *BTN_NAMES[8] = {"A", "B", "Home", "Down", "Left", "Right", "Up", "Aux1"};

static int cmd_btn(int argc, char **argv) {
  (void)argc;
  (void)argv;
  uint8_t pressed = hal_buttons_read();
  printf("shift: 0x%02X (", pressed);
  if (!pressed) printf("none");
  for (int i = 0; i < 8; i++)
    if (pressed & (1 << i)) printf("%s ", BTN_NAMES[i]);
  printf(")  START=%s\n", hal_buttons_start() ? "pressed" : "released");
  return 0;
}

static int cmd_ping(int argc, char **argv) {
  // ping [v0 v1 ..] — send demo payload (default 1 3 3 7).
  uint8_t vals[PING_MAX_VALS] = {1, 3, 3, 7};
  uint8_t len = 4;
  if (argc > 1) {
    len = 0;
    for (int i = 1; i < argc && len < PING_MAX_VALS; i++)
      vals[len++] = (uint8_t)strtoul(argv[i], NULL, 0);
  }
  uint8_t buf[32];
  uint32_t seq = 0;
  esp_err_t err = net_send(PKT_PING, vals, len, &seq);
  printf("ping #%u %s\n", (unsigned)seq, esp_err_to_name(err));
  return 0;
}

static int cmd_log(int argc, char **argv) {
  if (argc != 3) {
    printf("usage: log <tag|*> <none|error|warn|info|debug|verbose>\n");
    printf("tags: main ping net nfc dbg hal_i2c, or * for all\n");
    return 1;
  }
  esp_log_level_t level;
  if (!strcmp(argv[2], "none"))
    level = ESP_LOG_NONE;
  else if (!strcmp(argv[2], "error"))
    level = ESP_LOG_ERROR;
  else if (!strcmp(argv[2], "warn"))
    level = ESP_LOG_WARN;
  else if (!strcmp(argv[2], "info"))
    level = ESP_LOG_INFO;
  else if (!strcmp(argv[2], "debug"))
    level = ESP_LOG_DEBUG;
  else if (!strcmp(argv[2], "verbose"))
    level = ESP_LOG_VERBOSE;
  else {
    printf("bad level\n");
    return 1;
  }
  esp_log_level_set(argv[1], level);
  printf("log %s = %s\n", argv[1], argv[2]);
  return 0;
}

static int cmd_free(int argc, char **argv) {  (void)argc;
  (void)argv;
  printf("heap free: %u internal, min ever %u\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  return 0;
}

static int cmd_reboot(int argc, char **argv) {
  (void)argc;
  (void)argv;
  esp_restart();
  return 0;
}

static void register_cmd(const char *name, const char *help, const char *hint,
                         int (*func)(int, char **)) {
  esp_console_cmd_t cmd = {.command = name, .help = help, .hint = hint, .func = func};
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void repl_task(void *arg) {
  (void)arg;
  esp_console_repl_t *repl = NULL;
  esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  rc.prompt = "badge>";
  rc.max_cmdline_length = 128;
  ESP_ERROR_CHECK(esp_console_register_help_command());
  register_cmd("i2c_scan", "probe shared I2C bus for devices", NULL, cmd_i2c_scan);
  register_cmd("nfc_ver", "read MFRC522 VersionReg (want 0x91/0x92)", NULL, cmd_nfc_ver);
  register_cmd("nfc_reg", "read one MFRC522 register", "<hex addr>", cmd_nfc_reg);
  register_cmd("nfc_regs", "dump all MFRC522 registers", NULL, cmd_nfc_regs);
  register_cmd("nfc_begin", "run full NFC init, leave antenna on", NULL, cmd_nfc_begin);
  register_cmd("nfc_end", "antenna off, release NFC device", NULL, cmd_nfc_end);
  register_cmd("nfc_scan", "init + single tag poll", NULL, cmd_nfc_scan);
  register_cmd("btn", "print button states", NULL, cmd_btn);
  register_cmd("ping", "send one ESP-NOW ping", NULL, cmd_ping);
  register_cmd("log", "set log level at runtime", "<tag|*> <level>", cmd_log);
  register_cmd("free", "heap stats", NULL, cmd_free);
  register_cmd("reboot", "restart the badge", NULL, cmd_reboot);
  ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
  ESP_LOGI(TAG, "repl ready, type help");
  ESP_ERROR_CHECK(esp_console_start_repl(repl));  // never returns
  vTaskDelete(NULL);
}

void debug_init(void) {
  xTaskCreate(repl_task, "dbg_repl", 6144, NULL, 5, NULL);
}
