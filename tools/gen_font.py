#!/usr/bin/env python3
"""Pack GNU Unifont .hex files into font.bin for hardcore.

    gen_font.py OUT IN.hex[.gz] [IN2.hex ...]     later inputs override earlier ones

Layout (little endian), a two-level table so all 17 planes cost 17 KB of index:
  'WEF2'                     magic
  uint32 page[0x1100]        cp >> 8 → byte offset of a 256-entry block, 0 = empty page
  uint32 block[256] ...      cp & 255 → byte offset of the glyph record, 0 = no glyph
  records: uint8 width (8|16), then 16 rows of 1 byte (width 8) or 2 bytes (width 16)
"""
import gzip, struct, sys

dst, srcs = sys.argv[1], sys.argv[2:]
glyphs = {}
for src in srcs:
    f = gzip.open(src, 'rt', encoding='ascii') if src.endswith('.gz') else open(src, encoding='ascii')
    for line in f:
        line = line.strip()
        if line and not line.startswith('#'):
            cp_hex, bits = line.split(':')
            glyphs[int(cp_hex, 16)] = bytes.fromhex(bits)

NPAGES = 0x1100
pages = sorted({cp >> 8 for cp in glyphs})
page_off = [0] * NPAGES
base = 4 + NPAGES * 4
blocks_size = len(pages) * 256 * 4
rec_base = base + blocks_size
blocks = bytearray(blocks_size)
records = bytearray()
for pi, page in enumerate(pages):
    page_off[page] = base + pi * 1024
    for lo in range(256):
        bits = glyphs.get((page << 8) | lo)
        if bits is None:
            continue
        struct.pack_into('<I', blocks, pi * 1024 + lo * 4, rec_base + len(records))
        records.append(8 if len(bits) == 16 else 16)
        records += bits
with open(dst, 'wb') as f:
    f.write(b'WEF2')
    f.write(struct.pack('<%dI' % NPAGES, *page_off))
    f.write(blocks)
    f.write(records)
print(f'{dst}: {len(glyphs)} glyphs in {len(pages)} pages, {rec_base + len(records)} bytes')
