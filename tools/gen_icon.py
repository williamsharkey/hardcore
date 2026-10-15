#!/usr/bin/env python3
"""Draws the hardcore icon (Gruvbox Dark cream ">_" on its dark background).
OUT selects a PNG-in-ICO, a modern PNG-in-ICNS, or an .iconset directory.
No dependencies or platform image-encoder services are needed."""
import os
import struct, sys, zlib

FG = (0xEB, 0xDB, 0xB2)
BG = (0x28, 0x28, 0x28)

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
                    px[yy][xx] = FG + (255,)
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

def icns():
    # macOS 11+ accepts PNG elements at every size. Each element and the file
    # have a four-byte type and a big-endian length including that header.
    # The 1x/2x types match the ten representations produced by .iconset mode.
    # Format reference: Pillow's IcnsImagePlugin.py encoder/decoder.
    types = ((b'icp4', 16), (b'ic11', 32), (b'icp5', 32), (b'ic12', 64),
             (b'ic07', 128), (b'ic13', 256), (b'ic08', 256), (b'ic14', 512),
             (b'ic09', 512), (b'ic10', 1024))
    images = {n: png(n) for n in sorted({n for _, n in types})}
    entries = [(kind, struct.pack('>I', 8 + len(images[n])), images[n]) for kind, n in types]
    toc = b''.join(kind + size for kind, size, _ in entries)
    body = b'TOC ' + struct.pack('>I', 8 + len(toc)) + toc
    body += b''.join(kind + size + image for kind, size, image in entries)
    return b'icns' + struct.pack('>I', 8 + len(body)) + body

if sys.argv[1].endswith('.icns'):
    open(sys.argv[1], 'wb').write(icns())
    sys.exit(0)

if sys.argv[1].endswith('.iconset'):
    os.makedirs(sys.argv[1], exist_ok=True)
    for n in (16, 32, 128, 256, 512):
        open(os.path.join(sys.argv[1], f'icon_{n}x{n}.png'), 'wb').write(png(n))
        open(os.path.join(sys.argv[1], f'icon_{n}x{n}@2x.png'), 'wb').write(png(n * 2))
    sys.exit(0)

sizes = [16, 24, 32, 48, 64, 256]
images = [png(n) for n in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for n, img in zip(sizes, images):
    out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(img), off)
    off += len(img)
open(sys.argv[1], 'wb').write(out + b''.join(images))
