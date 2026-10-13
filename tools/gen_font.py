#!/usr/bin/env python3
"""Pack GNU Unifont .hex into font.bin for wsl-easy.

Layout (little endian):
  'WEF1'                     magic
  uint32 offset[65536]       0 = no glyph, else byte offset of its record
  records: uint8 width (8|16), then 16 rows of 1 byte (width 8) or 2 bytes (width 16)
"""
import struct, sys

src, dst = sys.argv[1], sys.argv[2]
offsets = [0] * 65536
data = bytearray()
base = 4 + 65536 * 4
for line in open(src, encoding='ascii'):
    line = line.strip()
    if not line:
        continue
    cp_hex, bits = line.split(':')
    cp = int(cp_hex, 16)
    if cp > 0xFFFF:
        continue
    width = 8 if len(bits) == 32 else 16
    offsets[cp] = base + len(data)
    data.append(width)
    data += bytes.fromhex(bits)
with open(dst, 'wb') as f:
    f.write(b'WEF1')
    f.write(struct.pack('<65536I', *offsets))
    f.write(data)
print(f'{dst}: {sum(1 for o in offsets if o)} glyphs, {base + len(data)} bytes')
