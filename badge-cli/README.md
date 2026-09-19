# badge-cli

Talk to a Hack the North 2026 Hacker Badge over USB from the terminal — no
browser required. This reimplements the serial protocol used by the web IDE
(<https://badge.hackthenorth.com/ide/>): the badge exposes a plain-text REPL
over USB-Serial-JTAG at 115200 baud.

## Setup

```sh
cd ~/development/badge-cli
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Then, with the badge **turned off**, plug in a **data-capable** USB-C cable and
turn it back on (don't hold Start).

## Usage

```sh
python badge.py ports                 # list serial ports (debugging)
python badge.py console               # interactive REPL to `badge> `
python badge.py cmd apps              # run one command, print reply
python badge.py cmd heap
python badge.py cmd uitree
python badge.py cmd "cat /littlefs/apps/demo_counter/main.lua"
python badge.py push myapp.lua        # push a single-file bundle
python badge.py push ./myapp_dir      # push a directory (manifest.cfg + main.lua + ...)
python badge.py reboot

# override autodetect:
python badge.py --port /dev/tty.usbmodemXXXX console
```

In `console` mode: type a command and press Enter, **Ctrl-C** sends a `^C`
(0x03) break to the badge, **Ctrl-D** quits the tool.

## Known console commands

These are the read-only / control commands the IDE uses and that the badge
guide documents. There may be more — connect with `console` and type `help`.

| Command | Purpose |
|---|---|
| `apps` | list installed apps |
| `heap` | system + LVGL memory |
| `uitree` | current visible widgets/text |
| `cat <path>` | read a file, e.g. `/littlefs/apps/<slug>/main.lua` |
| `rm <path>` | remove a file (e.g. an old `icon.bin`) |
| `reload` | rescan `/littlefs/apps` and refresh the launcher |
| `reboot` | restart the badge |
| `mkdir <path>` | create a directory (used by push) |
| `put <path> <N>` | receive N bytes into a file (used by push) |
| `put --binary` | switch put into binary-safe mode |
| `help [-v 1]` | list all registered console commands (run this to discover more) |
| `press <name>` | inject a button press (not exposed by the IDE) |
| `shot` | stream the screen as RLE+base64 RGB565 (not exposed by the IDE) |

Run `python badge.py cmd "help -v 1"` for the authoritative, firmware-specific
list — there are more commands than the web IDE uses (e.g. `press`, `shot`).

## Protocol notes (why the code looks the way it does)

- **Bare CR line ending** (`\r`), not `\r\n`. Firmware uses
  `ESP_LINE_ENDINGS_CR`; a trailing `\n` injects a phantom empty command.
- **Pacing is mandatory.** The USB-Serial-JTAG RX ring is only 256 bytes and
  can't be grown. Host→badge payloads go out in **128-byte chunks with 20 ms
  pauses**; a larger burst is silently truncated and `cmd_put` wedges forever
  (the firmware has no read timeout).
- **Upload sequence** per file:
  `put <remote> <N>` → wait `READY` → send N paced bytes → wait `OK <N>`.
  Binary files require `put --binary` (reply `PUT BINARY OK`) first.
- After uploading, send `reload` (wait `reload:`) so the launcher rescans.

## Scope / caveats

This is the **host/deploy/debug** channel. Apps you upload still run inside the
badge's Lua sandbox — talking to the device over serial does **not** grant your
Lua app Wi-Fi/HTTP or raw-peripheral access. It's the equivalent of the IDE's
Connect + Push + console, usable from scripts and CI.
