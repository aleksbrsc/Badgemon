// Minimal MFRC522 I2C driver: reset, init, REQA/anticoll/select/halt.
// I2C framing is STANDARD (arozcan/M5Stack libs, NXP datasheet 8.1.4):
// plain register byte, R/W via SLA direction, reads split across STOP.
#include "mfrc522.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nfc";

// Registers.
#define CommandReg 0x01
#define ComIrqReg 0x04
#define ErrorReg 0x06
#define FIFODataReg 0x09
#define FIFOLevelReg 0x0A
#define ControlReg 0x0C
#define BitFramingReg 0x0D
#define ModeReg 0x11
#define TxControlReg 0x14
#define TxASKReg 0x15
#define TModeReg 0x2A
#define TPrescalerReg 0x2B
#define TReloadRegH 0x2C
#define TReloadRegL 0x2D
#define VersionReg 0x37

// Commands.
#define PCD_Idle 0x00
#define PCD_Transceive 0x0C
#define PCD_SoftReset 0x0F

#define PICC_REQIDL 0x26

#define I2C_ADDR 0x26  // per custom-firmware-hal.md
#define I2C_SDA 5
#define I2C_SCL 6
#define I2C_TIMEOUT_MS 100

static esp_err_t wr(mfrc522_t *m, uint8_t reg, uint8_t val) {
  uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(m->dev, buf, 2, I2C_TIMEOUT_MS);
}

static esp_err_t wr_multi(mfrc522_t *m, uint8_t reg, const uint8_t *val, int n) {
  uint8_t buf[10];
  if (n + 1 > (int)sizeof(buf)) return ESP_ERR_INVALID_SIZE;
  buf[0] = reg;
  memcpy(buf + 1, val, n);
  return i2c_master_transmit(m->dev, buf, n + 1, I2C_TIMEOUT_MS);
}

static esp_err_t rd(mfrc522_t *m, uint8_t reg, uint8_t *val) {
  esp_err_t err = i2c_master_transmit(m->dev, &reg, 1, I2C_TIMEOUT_MS);
  if (err != ESP_OK) return err;
  return i2c_master_receive(m->dev, val, 1, I2C_TIMEOUT_MS);
}

static esp_err_t rd_multi(mfrc522_t *m, uint8_t reg, uint8_t *val, int n) {
  esp_err_t err = i2c_master_transmit(m->dev, &reg, 1, I2C_TIMEOUT_MS);
  if (err != ESP_OK) return err;
  return i2c_master_receive(m->dev, val, n, I2C_TIMEOUT_MS);
}

static void set_mask(mfrc522_t *m, uint8_t reg, uint8_t mask) {
  uint8_t v = 0;
  if (rd(m, reg, &v) == ESP_OK) wr(m, reg, v | mask);
}

static void clear_mask(mfrc522_t *m, uint8_t reg, uint8_t mask) {
  uint8_t v = 0;
  if (rd(m, reg, &v) == ESP_OK) wr(m, reg, v & ~mask);
}

// ISO14443-A CRC (poly 0x8408, init 0x6363) in software: no CalcCRC timing.
static void crc_a(const uint8_t *d, int n, uint8_t out[2]) {
  uint16_t crc = 0x6363;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
  }
  out[0] = crc & 0xFF;
  out[1] = crc >> 8;
}

// Transceive cmd with send[send_len] (last_bits_tx partial bits, e.g. 7 for
// REQA). Returns payload bytes in back[] (<back_cap), bit length in *bits.
static bool transceive(mfrc522_t *m, uint8_t cmd, const uint8_t *send, int send_len,
                       uint8_t last_bits_tx, uint8_t *back, int back_cap, int *bits) {
  if (wr(m, CommandReg, PCD_Idle) != ESP_OK) return false;
  if (wr(m, ComIrqReg, 0x7F) != ESP_OK) return false;    // clear IRQs
  if (wr(m, FIFOLevelReg, 0x80) != ESP_OK) return false;  // flush FIFO
  if (wr_multi(m, FIFODataReg, send, send_len) != ESP_OK) return false;
  if (wr(m, BitFramingReg, last_bits_tx) != ESP_OK) return false;
  if (wr(m, CommandReg, cmd) != ESP_OK) return false;

  // Wait RxIRq/IdleIRq, bounded (~36 ms).
  uint8_t irq = 0;
  for (int t = 0; t < 36; t++) {
    if (rd(m, ComIrqReg, &irq) != ESP_OK) return false;
    if (irq & 0x30) break;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  uint8_t err = 0;
  if (rd(m, ErrorReg, &err) != ESP_OK) return false;
  if (!(irq & 0x30) || (err & 0x13)) {
    ESP_LOGD(TAG, "xcv fail cmd=0x%02X irq=0x%02X err=0x%02X", cmd, irq, err);
    return false;  // timeout / BufferOvfl / Parity / Protocol
  }
  if (cmd != PCD_Transceive) return true;

  uint8_t n = 0, ctrl = 0;
  if (rd(m, FIFOLevelReg, &n) != ESP_OK) return false;
  if (rd(m, ControlReg, &ctrl) != ESP_OK) return false;
  uint8_t last_bits = ctrl & 0x07;
  if (n > back_cap) return false;
  if (rd_multi(m, FIFODataReg, back, n) != ESP_OK) return false;
  *bits = last_bits ? (n - 1) * 8 + last_bits : n * 8;
  return true;
}

static bool request(mfrc522_t *m, uint8_t atqa[2]) {
  uint8_t cmd = PICC_REQIDL, back[2];
  int bits = 0;
  if (!transceive(m, PCD_Transceive, &cmd, 1, 0x07, back, 2, &bits)) return false;
  if (bits != 16) return false;
  atqa[0] = back[0];
  atqa[1] = back[1];
  if (wr(m, BitFramingReg, 0x00) != ESP_OK) return false;
  ESP_LOGD(TAG, "req ok atqa=%02X%02X", atqa[0], atqa[1]);
  return true;
}

static bool anticoll(mfrc522_t *m, uint8_t uid[4]) {
  uint8_t cmd[2] = {0x93, 0x20}, back[5];
  int bits = 0;
  if (!transceive(m, PCD_Transceive, cmd, 2, 0x00, back, 5, &bits)) return false;
  if (bits != 40) return false;
  if ((back[0] ^ back[1] ^ back[2] ^ back[3] ^ back[4]) != 0) {
    ESP_LOGD(TAG, "anticoll BCC mismatch");
    return false;  // BCC
  }
  memcpy(uid, back, 4);
  return true;
}

static bool select_tag(mfrc522_t *m, const uint8_t uid[4], uint8_t *sak) {
  uint8_t frame[9] = {0x93, 0x70, uid[0], uid[1], uid[2], uid[3],
                      (uint8_t)(uid[0] ^ uid[1] ^ uid[2] ^ uid[3])};
  crc_a(frame, 7, &frame[7]);
  uint8_t back[3];
  int bits = 0;
  if (!transceive(m, PCD_Transceive, frame, 9, 0x00, back, 3, &bits)) return false;
  if (bits != 24) return false;
  *sak = back[0];
  return true;
}

static void halt(mfrc522_t *m) {
  uint8_t frame[4] = {0x50, 0x00};
  crc_a(frame, 2, &frame[2]);
  uint8_t back[4];
  int bits = 0;
  transceive(m, PCD_Transceive, frame, 4, 0x00, back, 4, &bits);  // no response expected
}

bool mfrc522_begin(mfrc522_t *m, i2c_master_bus_handle_t bus) {
  memset(m, 0, sizeof(*m));
  m->bus = bus;  // shared bus owned by main.c; we only add/remove our device
  i2c_device_config_t dev_cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                 .device_address = I2C_ADDR,
                                 .scl_speed_hz = 400000};
  if (i2c_master_bus_add_device(m->bus, &dev_cfg, &m->dev) != ESP_OK) return false;
  m->open = true;
  wr(m, CommandReg, PCD_SoftReset);
  vTaskDelay(pdMS_TO_TICKS(50));
  wr(m, TModeReg, 0x80);
  wr(m, TPrescalerReg, 0xA9);
  wr(m, TReloadRegH, 0x03);
  wr(m, TReloadRegL, 0xE8);
  wr(m, TxASKReg, 0x40);
  wr(m, ModeReg, 0x3D);
  set_mask(m, TxControlReg, 0x03);  // antenna on
  uint8_t ver = 0;
  if (rd(m, VersionReg, &ver) != ESP_OK || ver == 0x00 || ver == 0xFF) {
    ESP_LOGW(TAG, "no answer (ver=0x%02X)", ver);
    mfrc522_end(m);
    return false;
  }
  // 0x91/0x92 genuine, 0x90 v0.0, 0x88 FM17522 clone (all usable).
  ESP_LOGI(TAG, "mfrc522 ver=0x%02X%s", ver,
           (ver == 0x91 || ver == 0x92) ? "" : " (clone/unexpected, trying anyway)");
  return true;
}

void mfrc522_antenna(mfrc522_t *m, bool on) {
  if (!m->open) return;
  if (on)
    set_mask(m, TxControlReg, 0x03);
  else
    clear_mask(m, TxControlReg, 0x03);
}

bool mfrc522_scan(mfrc522_t *m, uint8_t uid[4], uint8_t atqa[2], uint8_t *sak) {  if (!m->open) return false;
  if (!request(m, atqa)) {
    ESP_LOGD(TAG, "scan: no REQA answer (no tag in field)");
    return false;
  }
  if (!anticoll(m, uid)) {
    ESP_LOGD(TAG, "scan: anticollision failed");
    return false;
  }
  if (!select_tag(m, uid, sak)) {
    ESP_LOGD(TAG, "scan: select failed");
    return false;
  }
  halt(m);
  return true;
}

void mfrc522_end(mfrc522_t *m) {
  if (!m->open) return;
  clear_mask(m, TxControlReg, 0x03);  // antenna off (save battery)
  i2c_master_bus_rm_device(m->dev);
  m->open = false;
}
