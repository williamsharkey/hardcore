#!/usr/bin/env python3
"""Decode asymmetric fixtures through production ImageIO and palette code."""
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
COLORS = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0))


def rgb_pixels(width, height):
    return b"".join(bytes(COLORS[(y >= height // 2) * 2 + (x >= width // 2)])
                    for y in range(height) for x in range(width))


def png(width, height):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    pixels = rgb_pixels(width, height)
    scanlines = b"".join(b"\0" + pixels[y*width*3:(y+1)*width*3] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def tiff(width, height, orientation):
    pixels = rgb_pixels(width, height)
    bits_offset = 8 + 2 + 11*12 + 4
    data_offset = bits_offset + 6
    tags = [(256, 4, 1, width), (257, 4, 1, height), (258, 3, 3, bits_offset),
            (259, 3, 1, 1), (262, 3, 1, 2), (273, 4, 1, data_offset),
            (274, 3, 1, orientation), (277, 3, 1, 3), (278, 4, 1, height),
            (279, 4, 1, len(pixels)), (284, 3, 1, 1)]
    return (b"II" + struct.pack("<HIH", 42, 8, len(tags))
            + b"".join(struct.pack("<HHII", *tag) for tag in tags)
            + struct.pack("<IHHH", 0, 8, 8, 8) + pixels)


HARNESS = r'''
#import <Cocoa/Cocoa.h>
#include <stdint.h>
#include <limits.h>
static struct {uint32_t pal[16];} colors,*g_shown=&colors;
#define DEF_BG 0
#define DEF_FG 0xffffff
#include "hardcore_browser_images.h"
static void browser_image_ready(int identity){(void)identity;}
int main(int argc,char **argv){@autoreleasepool {
    if(argc!=4)return 2;
    colors.pal[0]=0xff0000;colors.pal[1]=0x00ff00;colors.pal[2]=0x0000ff;colors.pal[3]=0xffff00;
    NSData *data=[NSData dataWithContentsOfFile:@(argv[1])];
    NSString *url=[@"data:image/png;base64," stringByAppendingString:[data base64EncodedStringWithOptions:0]];
    HCBrowserImage *image=[[HCBrowserImage alloc] initWithSource:url owner:0];
    if(!image->rgba){[image release];return 3;}
    int cw=atoi(argv[2]),ch=atoi(argv[3]),cols,rows;
    browser_image_size(image->source_width,image->source_height,cw,ch,&cols,&rows);
    browser_image_palette(image,cols*cw,rows*ch);
    const unsigned char *pixels=image->rgba.bytes;
    int offsets[]={0,image->sw-1,(image->sh-1)*image->sw,image->sw*image->sh-1};
    int quantized[]={0,image->qw-1,(image->qh-1)*image->qw,image->qw*image->qh-1};
    printf("{\"decoded\":[%d,%d],\"source\":[%d,%d],\"cells\":[%d,%d],\"corners\":[",
           image->sw,image->sh,image->source_width,image->source_height,cols,rows);
    for(int i=0;i<4;i++) {const unsigned char *p=pixels+offsets[i]*4;
        printf("%s[%u,%u,%u]",i?",":"",p[0],p[1],p[2]);}
    printf("],\"quantized\":[");
    for(int i=0;i<4;i++)printf("%s%u",i?",":"",image->palette[image->indexed[quantized[i]]]);
    printf("]}\n");[image release];
}return 0;}
'''


@unittest.skipUnless(sys.platform == "darwin", "ImageIO regression requires macOS")
class BrowserImageDecode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="hc-browser-image-decode-")
        cls.root = Path(cls.temporary.name)
        source = cls.root/"decode.m";source.write_text(HARNESS)
        cls.binary = cls.root/"decode"
        subprocess.run([os.environ.get("CC", "clang"), "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        *shlex.split(os.environ.get("HC_TEST_CFLAGS", "")), "-I", str(ROOT), str(source),
                        "-framework", "Cocoa", "-framework", "ImageIO", "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def decode(self, data, cw=8, ch=16):
        fixture = self.root/"fixture.image";fixture.write_bytes(data)
        return json.loads(subprocess.check_output([str(self.binary), str(fixture), str(cw), str(ch)], text=True))

    def test_top_and_bottom_survive_decode_and_palette_sampling(self):
        result = self.decode(png(64, 48))
        self.assertEqual(result["corners"], [list(c) for c in COLORS])
        self.assertEqual(result["quantized"], [0xff0000, 0x00ff00, 0x0000ff, 0xffff00])
        self.assertEqual(result["cells"], [8, 3])

    def test_exif_rotation_is_applied_once_and_intrinsic_size_is_upright(self):
        result = self.decode(tiff(4, 2, 6), cw=1, ch=1)
        self.assertEqual(result["decoded"], [2, 4])
        self.assertEqual(result["source"], [2, 4])
        self.assertEqual(result["corners"], [list(COLORS[i]) for i in (2, 0, 3, 1)])

    def test_large_source_retains_intrinsic_dimensions_after_thumbnail_decode(self):
        result = self.decode(png(1600, 800))
        self.assertEqual(result["decoded"], [512, 256])
        self.assertEqual(result["source"], [1600, 800])
        self.assertEqual(result["cells"], [24, 6])
        result = self.decode(png(1600, 800), cw=12, ch=26)
        self.assertEqual(result["cells"], [24, 6])

    def test_maximum_and_small_originals_round_to_integral_cells(self):
        self.assertEqual(self.decode(png(384, 256), cw=6, ch=12)["cells"], [24, 8])
        self.assertEqual(self.decode(png(12, 24), cw=6, ch=12)["cells"], [2, 2])
        self.assertEqual(self.decode(png(12, 24))["cells"], [2, 2])


if __name__ == "__main__":
    unittest.main()
