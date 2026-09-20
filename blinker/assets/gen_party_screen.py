#!/usr/bin/env python3
"""Generate blinker/assets/party-screen.png (320x240) approximating the
reference party screen: teal striped backdrop, light-blue status panel
top-left with an empty HP trough, white bottom dialog bar, purple tab
bottom-right. Dynamic text/bars are overlaid by ui_duel.c at the
PARTY2_* coordinates in main/assets.h (kept in sync with the rects
below).
Stdlib only.
"""
import struct
import zlib
import os

W, H = 320, 240
HERE = os.path.dirname(os.path.abspath(__file__))

DARK = (33, 107, 99)
LIGHT = (54, 137, 132)
BLACK = (0, 0, 0)
PANEL = (172, 218, 234)
PANEL_TOP = (148, 200, 222)
TROUGH = (96, 104, 100)
TROUGH_EDGE = (40, 60, 55)
WHITE = (246, 246, 246)
GRAY = (132, 138, 142)
PURPLE = (106, 76, 162)
RED = (224, 48, 48)

# Must match PARTY2_* in main/assets.h
BAR_X, BAR_Y, BAR_W, BAR_H = 32, 64, 110, 8


def rect(px, x0, y0, x1, y1, c):
    for y in range(max(0, y0), min(H, y1)):
        for x in range(max(0, x0), min(W, x1)):
            px[y * W + x] = c


def ball(px, cx, cy, r):
    for y in range(cy - r - 1, cy + r + 2):
        for x in range(cx - r - 1, cx + r + 2):
            if not (0 <= x < W and 0 <= y < H):
                continue
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
            if d <= r + 0.5:
                if d > r - 1.2:
                    px[y * W + x] = BLACK
                elif abs(y - cy) <= 1 and abs(x - cx) <= r:
                    px[y * W + x] = BLACK
                elif (x - cx) ** 2 + (y - cy) ** 2 <= 9:
                    px[y * W + x] = BLACK if (x == cx and y == cy) else WHITE
                elif y < cy:
                    px[y * W + x] = RED
                else:
                    px[y * W + x] = WHITE


def main():
    px = []
    for y in range(H):
        c = DARK if (y % 14) < 5 else LIGHT
        px += [c] * W

    # Top-left status panel with black border.
    rect(px, 4, 6, 152, 90, BLACK)
    rect(px, 7, 9, 149, 87, PANEL)
    rect(px, 7, 9, 149, 27, PANEL_TOP)
    ball(px, 22, 18, 11)
    # Empty HP trough (the fill is an LVGL bar overlaid by code).
    rect(px, BAR_X - 2, BAR_Y - 2, BAR_X + BAR_W + 2, BAR_Y + BAR_H + 2,
         TROUGH_EDGE)
    rect(px, BAR_X, BAR_Y, BAR_X + BAR_W, BAR_Y + BAR_H, TROUGH)

    # Bottom dialog bar.
    rect(px, 4, 196, 316, 236, GRAY)
    rect(px, 7, 199, 313, 233, WHITE)
    # Purple tab bottom-right.
    rect(px, 238, 206, 316, 236, BLACK)
    rect(px, 241, 209, 316, 236, PURPLE)
    ball(px, 252, 222, 11)

    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            r, g, b = px[y * W + x]
            raw += bytes((r, g, b))
    blob = struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)
    ihdr = b'IHDR' + blob
    ihdr += struct.pack('>I', zlib.crc32(ihdr) & 0xFFFFFFFF)
    comp = zlib.compress(bytes(raw), 9)
    idat = b'IDAT' + comp
    idat += struct.pack('>I', zlib.crc32(idat) & 0xFFFFFFFF)
    png = (b'\x89PNG\r\n\x1a\n' + struct.pack('>I', 13) + ihdr +
           struct.pack('>I', len(comp)) + idat +
           struct.pack('>I', 0) + b'IEND' +
           struct.pack('>I', zlib.crc32(b'IEND') & 0xFFFFFFFF))
    out = os.path.join(HERE, 'party-screen.png')
    with open(out, 'wb') as f:
        f.write(png)
    print('wrote', out, len(png), 'bytes')


if __name__ == '__main__':
    main()
