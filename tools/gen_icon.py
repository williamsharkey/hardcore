#!/usr/bin/env python3
"""Draws the hardcore icon (amber ">_" on near-black, dumbterm's phosphor color)
as a PNG-in-ICO with 16/24/32/48/64/256 px frames. No dependencies."""
import struct, sys, zlib

AMBER = (0xFF, 0xA6, 0x0D)
BG = (0x1B, 0x1B, 0x1B)

def render(n):
    px = [[(0, 0, 0, 0)] * n for _ in range(n)]
    r = n * 0.18                      # corner radius
    for y in range(n):
        for x in range(n):
            cx = min(max(x + 0.5, r), n - r); cy = min(max(y + 0.5, r), n - r)
            if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= r * r:
                px[y][x] = BG + (255,)
    t = max(1, round(n / 11))         # stroke thickness
    def dot(x, y):
        for yy in range(int(y), int(y) + t):
            for xx in range(int(x), int(x) + t):
                if 0 <= xx < n and 0 <= yy < n:
                    px[yy][xx] = AMBER + (255,)
    # chevron ">"
    x0, y0, h = n * 0.22, n * 0.28, n * 0.40
    steps = max(8, n)
    for i in range(steps + 1):
        f = i / steps
        dot(x0 + f * h * 0.55, y0 + f * h / 2)
        dot(x0 + f * h * 0.55, y0 + h - f * h / 2)
    # underscore "_"
    for x in range(int(n * 0.50), int(n * 0.80)):
        dot(x, n * 0.68)
    return px

def png(n):
    px = render(n)
    raw = b''.join(b'\x00' + bytes(c for p in row for c in p) for row in px)
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

sizes = [16, 24, 32, 48, 64, 256]
images = [png(n) for n in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for n, img in zip(sizes, images):
    out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(img), off)
    off += len(img)
open(sys.argv[1], 'wb').write(out + b''.join(images))
