#!/usr/bin/env python3
"""
badge.py - talk to a Hack the North 2026 Hacker Badge over USB (no browser).

This reimplements the wire protocol used by the web IDE at
https://badge.hackthenorth.com/ide/ (app.js). The badge exposes a plain text
REPL over USB-Serial-JTAG at 115200 baud. This tool lets you:

  * open an interactive console to the `badge> ` prompt
  * run one-off console commands (apps, heap, uitree, cat, rm, reload, reboot, ...)
  * push a single-file .lua app (manifest header + main.lua) or a directory

Protocol notes (mirrored from the IDE, do not "simplify" these):
  * 115200 baud, USB-Serial-JTAG.
  * Line ending is a BARE CR (\\r). Firmware uses ESP_LINE_ENDINGS_CR; sending
    \\r\\n injects a phantom empty command.
  * Prompt to sync on is "badge> ".
  * Ctrl-C is byte 0x03.
  * The badge RX ring is only 256 bytes and cannot be grown. Host->badge data
    MUST be paced: 128-byte chunks with a 20 ms pause between them, or the
    transfer is silently truncated and cmd_put wedges forever (no read timeout).
  * File upload:
        mkdir <dir>                 -> wait "badge> "
        put <remote> <N>            -> wait "READY"
        <N raw bytes, paced>        -> wait "OK <N>"
    Binary files need `put --binary` first (reply "PUT BINARY OK"); older
    firmware CR-translates payload bytes otherwise.
  * After pushing, send `reload` (wait "reload:") so the launcher rescans
    /littlefs/apps, otherwise the app won't appear.

Usage:
    python badge.py console
    python badge.py cmd apps
    python badge.py cmd "cat /littlefs/apps/demo_counter/main.lua"
    python badge.py push myapp.lua
    python badge.py push ./myapp_dir            # dir containing manifest.cfg etc.
    python badge.py reboot

    # optional: --port /dev/tty.usbmodemXXXX to skip autodetect
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time

try:
    import serial  # pyserial
    from serial.tools import list_ports
except ImportError:
    sys.exit(
        "pyserial is not installed.\n"
        "  python3 -m venv .venv && source .venv/bin/activate && pip install pyserial\n"
        "or: pip install pyserial"
    )

BAUD = 115200
PROMPT = "badge> "
WRITE_CHUNK = 128       # bytes per write burst (RX ring is 256B)
WRITE_PAUSE = 0.020     # 20 ms between bursts
CTRL_C = b"\x03"

# Single-file bundle header, as extracted by the IDE importer.
HEADER_RE = re.compile(
    r"--\[==\[badge-app\r?\n(.*?)\r?\n\]==\]\r?\n?(.*)",
    re.DOTALL,
)

# Files present in the workspace but never pushed to the badge.
NO_PUSH = {"README.md"}


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


# --------------------------------------------------------------------------- #
# Serial transport
# --------------------------------------------------------------------------- #
class Badge:
    def __init__(self, port: str, verbose: bool = True):
        self.verbose = verbose
        self.ser = serial.Serial(port, BAUD, timeout=0.05)
        self.buf = ""  # decoded read buffer, like state.readBuffer in app.js

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    # ---- low level ----
    def _pump(self, echo: bool = True) -> None:
        """Read whatever is available into self.buf (and optionally to stdout)."""
        data = self.ser.read(4096)
        if data:
            text = data.decode("utf-8", errors="replace")
            self.buf += text
            if len(self.buf) > 32768:
                self.buf = self.buf[-32768:]
            if echo and self.verbose:
                sys.stdout.write(text)
                sys.stdout.flush()

    def send_line(self, s: str, echo: bool = True) -> None:
        # Bare CR line ending (ESP_LINE_ENDINGS_CR).
        self.ser.write((s + "\r").encode("utf-8"))
        self.ser.flush()
        if echo and self.verbose:
            sys.stdout.write(f">> {s}\n")
            sys.stdout.flush()

    def send_bytes(self, data: bytes) -> None:
        # Chunked + paced so we never overflow the 256-byte RX ring.
        for i in range(0, len(data), WRITE_CHUNK):
            self.ser.write(data[i : i + WRITE_CHUNK])
            self.ser.flush()
            if i + WRITE_CHUNK < len(data):
                time.sleep(WRITE_PAUSE)

    def send_break(self) -> None:
        self.ser.write(CTRL_C)
        self.ser.flush()

    def wait_for(self, pattern: str, timeout: float = 5.0, echo: bool = True) -> str:
        """Block until `pattern` appears in the buffer; consume through it."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            idx = self.buf.find(pattern)
            if idx >= 0:
                end = idx + len(pattern)
                resp = self.buf[:end]
                self.buf = self.buf[end:]
                return resp
            self._pump(echo=echo)
            time.sleep(0.01)
        raise TimeoutError(f'timeout waiting for "{pattern}"')

    def sync_prompt(self, timeout: float = 4.0) -> None:
        """Make sure the badge is sitting at a fresh prompt.

        The console only emits `badge> ` in response to a CR, and the first CR
        right after opening the port is often dropped, so we resend it a few
        times until the prompt shows up.
        """
        self.buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.send_line("", echo=False)
            try:
                self.wait_for(PROMPT, 0.6, echo=False)
                return
            except TimeoutError:
                continue
        raise TimeoutError(
            'no "badge> " prompt. Is the badge on with a data cable? '
            "If it is running an app, return to the launcher (HOME) or reboot."
        )

    # ---- high level ----
    def run_cmd(self, cmd: str, timeout: float = 8.0) -> str:
        """Run one console command and return everything up to the next prompt."""
        self.sync_prompt()
        self.buf = ""
        self.send_line(cmd, echo=False)
        resp = self.wait_for(PROMPT, timeout, echo=False)
        # Strip the trailing prompt for cleaner output.
        return resp[: -len(PROMPT)]


# --------------------------------------------------------------------------- #
# Port discovery
# --------------------------------------------------------------------------- #
def find_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    candidates = list(list_ports.comports())
    # Prefer Espressif USB JTAG/serial debug units.
    def score(p) -> int:
        s = 0
        hay = f"{p.device} {p.description} {p.manufacturer} {p.vid:04x}".lower() if p.vid else f"{p.device} {p.description}".lower()
        if p.vid == 0x303A:  # Espressif
            s += 100
        if "jtag" in hay or "espressif" in hay:
            s += 50
        if "usbmodem" in hay or "usbserial" in hay:
            s += 10
        if "bluetooth" in hay or "debug-console" in hay:
            s -= 100
        return s
    if not candidates:
        sys.exit("No serial ports found. Plug in the badge (data cable), power it on, and retry.")
    candidates.sort(key=score, reverse=True)
    best = candidates[0]
    log(f"[badge] using port {best.device} ({best.description})")
    others = [c.device for c in candidates[1:]]
    if others:
        log(f"[badge] other ports: {', '.join(others)}  (override with --port)")
    return best.device


# --------------------------------------------------------------------------- #
# App bundle parsing
# --------------------------------------------------------------------------- #
def parse_single_file(text: str) -> dict[str, bytes]:
    """Split a combined .lua bundle into {manifest.cfg, main.lua}."""
    m = HEADER_RE.match(text.lstrip("\ufeff"))
    if not m:
        sys.exit(
            "That .lua file has no badge-app header. Expected a first line\n"
            "  --[==[badge-app\n  slug=...\n  ]==]\n"
            "followed by the Lua code."
        )
    manifest = m.group(1).strip() + "\n"
    main = m.group(2)
    return {
        "manifest.cfg": manifest.encode("utf-8"),
        "main.lua": main.encode("utf-8"),
    }


def load_app(path: str) -> dict[str, bytes]:
    """Load an app either from a single .lua file or a directory of files."""
    if os.path.isdir(path):
        files: dict[str, bytes] = {}
        for root, _dirs, names in os.walk(path):
            for name in names:
                full = os.path.join(root, name)
                rel = os.path.relpath(full, path).replace(os.sep, "/")
                if rel in NO_PUSH or os.path.basename(rel).startswith("."):
                    continue
                with open(full, "rb") as fh:
                    files[rel] = fh.read()
        if "manifest.cfg" not in files:
            sys.exit(f"{path} has no manifest.cfg")
        return files
    with open(path, "r", encoding="utf-8") as fh:
        return parse_single_file(fh.read())


def get_slug(manifest: bytes) -> str:
    m = re.search(r"^\s*slug\s*=\s*(\S+)", manifest.decode("utf-8", "replace"), re.M)
    if not m:
        sys.exit("manifest.cfg is missing a slug= line")
    return m.group(1)


def is_binary(name: str, data: bytes) -> bool:
    if name.endswith(".lua") or name.endswith(".cfg") or name.endswith(".md") or name.endswith(".txt"):
        return False
    return b"\x00" in data or name.endswith(".bin")


# --------------------------------------------------------------------------- #
# Commands
# --------------------------------------------------------------------------- #
def cmd_push(badge: Badge, path: str) -> None:
    files = load_app(path)
    slug = get_slug(files["manifest.cfg"])
    remote_dir = f"/littlefs/apps/{slug}"

    log(f"[push] slug={slug}, {len(files)} file(s)")
    badge.sync_prompt()

    # If any file is binary, flip the badge into binary-safe put mode first.
    if any(is_binary(n, d) for n, d in files.items()):
        badge.buf = ""
        badge.send_line("put --binary", echo=False)
        reply = badge.wait_for(PROMPT, 5.0, echo=False)
        if "PUT BINARY OK" not in reply:
            sys.exit("badge firmware needs an update for binary uploads (put --binary unsupported)")

    badge.send_line(f"mkdir {remote_dir}", echo=False)
    badge.wait_for(PROMPT, 5.0, echo=False)

    known_dirs = {remote_dir}
    for i, name in enumerate(sorted(files), start=1):
        if name in NO_PUSH:
            continue
        data = files[name]
        remote = f"{remote_dir}/{name}"
        parent = remote.rsplit("/", 1)[0]
        if parent not in known_dirs:
            badge.send_line(f"mkdir {parent}", echo=False)
            badge.wait_for(PROMPT, 5.0, echo=False)
            known_dirs.add(parent)

        log(f"[push] {i}/{len(files)} {name} ({len(data)} B) ...")
        badge.buf = ""
        badge.send_line(f"put {remote} {len(data)}", echo=False)
        badge.wait_for("READY", 5.0, echo=False)
        badge.send_bytes(data)
        badge.wait_for(f"OK {len(data)}", 20.0, echo=False)

    # Launcher only rescans /littlefs/apps on reload/boot.
    badge.buf = ""
    badge.send_line("reload", echo=False)
    try:
        badge.wait_for("reload:", 8.0, echo=False)
        log("[push] reload confirmed")
    except TimeoutError:
        log("[push] reload not confirmed - reboot the badge if the app doesn't appear")
    log(f"[push] done: {slug}")


def cmd_console(badge: Badge) -> None:
    """Interactive REPL. Type commands; Ctrl-D to quit, Ctrl-C sends a break."""
    import threading

    log("[console] connected. Type commands, Ctrl-D to quit, Ctrl-C sends ^C to badge.")
    badge.send_line("", echo=False)

    stop = threading.Event()

    def reader() -> None:
        while not stop.is_set():
            badge._pump(echo=True)
            time.sleep(0.01)

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    try:
        while True:
            try:
                line = input()
            except KeyboardInterrupt:
                badge.send_break()
                continue
            except EOFError:
                break
            badge.send_line(line, echo=False)
    finally:
        stop.set()
        t.join(timeout=0.5)


def main() -> None:
    ap = argparse.ArgumentParser(description="Talk to a Hack the North 2026 badge over USB.")
    ap.add_argument("--port", help="serial device (default: autodetect Espressif JTAG)")
    sub = ap.add_subparsers(dest="action", required=True)

    sub.add_parser("console", help="interactive REPL to the badge")

    p_cmd = sub.add_parser("cmd", help="run one console command and print the reply")
    p_cmd.add_argument("command", nargs="+", help="e.g. apps | heap | uitree | cat <path>")

    p_push = sub.add_parser("push", help="upload a single-file .lua app or an app directory")
    p_push.add_argument("path", help="path to <app>.lua or a directory with manifest.cfg")

    sub.add_parser("reboot", help="reboot the badge")
    sub.add_parser("ports", help="list candidate serial ports and exit")

    args = ap.parse_args()

    if args.action == "ports":
        for p in list_ports.comports():
            print(f"{p.device}\t{p.description}\tvid={p.vid and hex(p.vid)}")
        return

    port = find_port(args.port)
    badge = Badge(port, verbose=True)
    try:
        if args.action == "console":
            cmd_console(badge)
        elif args.action == "cmd":
            out = badge.run_cmd(" ".join(args.command))
            sys.stdout.write(out)
            if not out.endswith("\n"):
                sys.stdout.write("\n")
        elif args.action == "push":
            cmd_push(badge, args.path)
        elif args.action == "reboot":
            badge.sync_prompt()
            badge.send_line("reboot", echo=False)
            log("[badge] reboot sent")
    finally:
        badge.close()


if __name__ == "__main__":
    main()
