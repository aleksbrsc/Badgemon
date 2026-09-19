# blinker

Badge-to-badge ping over ESP-NOW broadcast, LVGL screen, WS2812 blink.
Pins/values per `../custom-firmware-hal.md`.

## What it does

- Screen: big **"hello htn"** + status line showing your MAC tail
  (`me AB:CD`) and last ping activity.
- **Press A** → broadcasts a ping (sequence-numbered, tagged with your
  MAC). Your status line confirms `ping #N sent!`.
- **Receive** → status shows `ping #N from AB:CD!` + 300 ms green LED
  burst on all 6 LEDs.
- Idle: dim red blink (~10 Hz tick, toggles every 50 ms).
- No pairing, no network: flash this same firmware on every badge and
  they all hear each other (WiFi STA, channel 1, broadcast MAC).

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
