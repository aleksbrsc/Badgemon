# Flash any Lua app dir to the badge via badge-cli.
# Usage: make flash APP=ALight [PORT=/dev/ttyACM0]
#        make APP=ALight console | cmd CMD="apps" | reboot

APP ?= ALight
PORT ?=
PY ?= python3
CLI := badge-cli/badge.py

PUSH_ARGS := push $(APP)
CONSOLE_ARGS := console
ifneq ($(strip $(PORT)),)
PUSH_ARGS += --port $(PORT)
CONSOLE_ARGS += --port $(PORT)
endif

.PHONY: flash console cmd reboot ports list help

flash:
	$(PY) $(CLI) $(PUSH_ARGS)

console:
	$(PY) $(CLI) $(CONSOLE_ARGS)

cmd:
	$(PY) $(CLI) $(if $(strip $(PORT)),--port $(PORT),) cmd $(CMD)

reboot:
	$(PY) $(CLI) $(if $(strip $(PORT)),--port $(PORT),) reboot

ports:
	$(PY) $(CLI) ports

list:
	ls -d */ | grep -v badge-cli

help:
	@echo "make flash APP=<dir> [PORT=/dev/ttyACM0]"
	@echo "make console APP=<dir> | make cmd CMD=\"apps\" | make reboot | make ports"
