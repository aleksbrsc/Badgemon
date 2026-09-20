# ping badge

ESP-NOW ping badge with a main menu, settings + persistent name, and a
button keyboard. Flash the same firmware on every badge — no pairing,
no network (WiFi STA, channel 1, broadcast MAC).

## Screens

Navigation rule: **Home is always back** (menu is the root).
- **Menu** (`Up/Down` move, `A` open): `play`, `settings`. Greets you
  by name once set. (Raw `ping`/`message` screens still exist in the
  build but are hidden from the menu.)
- **Play (lobby)**:
  1. Opens with an auto-scan; `A` on `refresh` re-broadcasts DISCOVER.
     Every badge auto-replies with its name (settings name or MAC
     tail), so nearby players accumulate in the list (15 s expiry).
  2. Move to a name, `A` challenges — a broadcast only that MAC acts
     on. Waiter shows amber pulse, 10 s timeout.
  3. Receiver pops `X wants to play! A = yes, B = no` (green flash).
     Answer routes back; challenger sees `game on vs X!` / `X declined`.
     `Home` in a dialog/wait declines/cancels.
- **Settings**: view name, `edit name` (keyboard), `clear name`.
- **Keyboard** (reusable, `ui_keyboard.h`): full-width 7-column grid
  at font 24 — a-z, 0-9, `._-+`, plus `aA` (one-shot caps toggle) and
  `sp` (space). `A` pick, `B` delete, `START` save, `Home` cancel.
  Shift state shows as `^` after the text. Two modes: `ui_keyboard_start`
  (own screen, e.g. settings) or `ui_keyboard_bind` (embed into your
  labels, e.g. message). Name persists in NVS (`badge` namespace)
  across reboots and reflashes.

## Payload wire format (`main/payload.h`)

`mac[6] | seq u32 LE | type u8 | len u8 | vals[len]`, max 32 values
(44 bytes). Types: ping digits/ASCII, discover, presence (name),
challenge (target MAC + name), response (target MAC + accept + name).
Receivers show text when all bytes are printable, numbers otherwise.
Reflash ALL badges together — the header grew by one byte, old
firmware drops new packets (and vice versa).
`ping_pack` / `ping_unpack` do the marshalling; malformed packets are
dropped in the RX callback with a debug log.

## Code map (`main/`)

| file | owns |
|---|---|
| `hal_buttons.h/.c` | HC165 + START init, raw read, edge detect |
| `hal_led.h/.c` | WS2812 strip init + set/show |
| `hal_display.h/.c` | SPI + ST7789 + LVGL init, screen reset |
| `hal_i2c.h/.c` | shared I2C bus (accel + NFC) |
| `net.h/.c` | WiFi STA + ESP-NOW, MAC, send, RX queue |
| `lobby.h/.c` | discovery, peer list, challenge/response routing |
| `ui_play.h/.c` | lobby browser + challenge dialog |
| `store.h/.c` | NVS persistence (`badge` namespace) |
| `nav.h/.c` | screen switching (menu/ping/settings) |
| `ui_menu.h/.c` | reusable menu (title, subtitle, items, pick/back cbs) |
| `ui_menu.h/.c` | main menu |
| `ui_keyboard.h/.c` | reusable button keyboard |
| `ui_settings.h/.c` | name view/edit/clear |
| `payload.h` | wire format + `ping_pack`/`ping_unpack` |
| `ui_ping.h/.c` | composer keyboard + payload display + LEDs |
| `mfrc522.h/.c` | NFC reader driver (I2C) |
| `debug.h/.c` | serial REPL |
| `main.c` | init order + 20 ms poll loop only |

Rules: UI never touches ESP-NOW (use `net_*`), drivers never touch
LVGL, `net` never touches display/LEDs. `main.c` wires it together.

## Debug console (serial REPL)

`make monitor`, then type at the `badge>` prompt (`help` lists all):

| cmd | what it tells you |
|---|---|
| `i2c_scan` | every live I2C address — expect `0x19` (accel) always, `0x26` (NFC) when powered |
| `nfc_ver` | VersionReg — want `0x91`/`0x92` |
| `nfc_reg 37` | read one register (hex) |
| `nfc_regs` | dump all registers |
| `nfc_begin` | full init with antenna left on |
| `nfc_scan` | init + single tag poll (hold card at reader) |
| `btn` | live HC165 + START states |
| `ping [v0 v1 ..]` | send payload from REPL (default demo `1 3 3 7`) |
| `log <tag\|*> <level>` | runtime log level: `log net debug`, `log * debug` (tags: main ping net nfc dbg hal_i2c) |
| `free` / `reboot` | heap stats / restart |

Logging convention: INFO for lifecycle (enter, sent/received/found),
WARN for recoverable failures, DEBUG for per-step internals (off by
default, enable via `log`, e.g. `log net debug`). Tags: main ping net
nfc dbg hal_i2c store menu settings kbd nav.

NFC triage: `i2c_scan` first. Nothing at all → bus wedged, suspect
low AA voltage (the NFC chip drags the shared bus down — use USB power
or fresh batteries and re-run). `0x19` but no `0x26` → NFC has no
power. `0x26` present but `nfc_ver` fails → chip held in reset or init
timing; paste the `nfc_regs` dump.

## Setup (once)

```sh
/opt/esp-idf/install.sh          # only after installing/upgrading esp-idf
source /opt/esp-idf/export.sh    # every terminal (or add to ~/.bashrc)
```

## Build

```sh
cd blinker
source /opt/esp-idf/export.sh
make build
```

## Flash

The badge's native USB has no auto-reset, and the stub flasher dies on
this board's ESP32-C3 — so `idf.py flash` does NOT work. `make flash`
uses raw esptool with `--no-stub` instead.

1. Unplug USB.
2. **Hold START** while plugging USB back in (blank screen = download
   mode, expected). Release START.
3. Flash:

```sh
cd blinker
source /opt/esp-idf/export.sh
make flash                       # PORT=/dev/ttyACM0, BAUD=115200 by default
make flash PORT=/dev/ttyACM1     # override if needed
```

4. Unplug/replug **without** holding START to boot into the app.
   LEDs blink dim red; screen shows "hello world".

## Monitor serial output

```sh
make monitor                      # on/off log lines, Ctrl+] to exit
```

## Notes

- ESP-IDF v6: `led_strip` comes from the component registry
  (`idf.py add-dependency "espressif/led_strip"`, already done) and uses
  `color_component_format`, not the old `led_pixel_format`.
- If CMake doesn't pick up a new component, `rm -rf build` and rebuild.
- Brightness is capped (24/255) so AA power won't brown-out.
