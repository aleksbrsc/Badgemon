# blinker

Blinks the badge's 6x WS2812 LEDs (GPIO3, dim red, ~1 Hz) and shows
white-on-teal **"hello world"** centered on the ST7789 screen.
Pins/values per `../custom-firmware-hal.md`.

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
