# Blinker shortcuts. Source ESP-IDF first: source /opt/esp-idf/export.sh
# Usage: make build | make flash [PORT=/dev/ttyACM0] [BAUD=115200] | make monitor
PORT ?= /dev/ttyACM0
BAUD ?= 115200

.PHONY: build flash monitor

build:
	idf.py build

# --no-stub: the stub flasher dies on this board's ESP32-C3 (rev v0.4);
# plain idf.py flash fails, raw esptool with --no-stub works.
flash:
	idf.py build
	cd build && python -m esptool --chip esp32c3 -p $(PORT) -b $(BAUD) \
	  --before=default-reset --after=hard-reset --no-stub write-flash @flash_args

monitor:
	idf.py -p $(PORT) monitor
