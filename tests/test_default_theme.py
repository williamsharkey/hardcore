#!/usr/bin/env python3
"""Fresh theme, saved theme overrides, and installed icon palette regressions.

Compiles the actual palette/configuration functions with font-only stubs and
generates icons in a temporary directory. No app or user settings are opened.
"""
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

from test_mac_import_resize import bounded_run, production_function

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* THEME_DECLARATIONS */
static uint32_t PAL[256];
typedef struct { char key; const char *name; int w, h; } Font;
static const Font FONTS[] = {{'U', "Unifont", 8, 16}, {'H', "Hairline", 12, 26}};
#define NFONTS 2
static const Font *g_font = FONTS, *g_tfont = FONTS, *g_cf = FONTS;
static const Font *font_by_key(char key) {
    for (int i = 0; i < NFONTS; i++) if (FONTS[i].key == key) return FONTS+i;
    return NULL;
}
static int cell_w(const Font *font) { return font->w; }
static int cell_h(const Font *font) { return font->h; }
static int g_tab_underlay, g_chrome_reflow = 1, g_chrome_debug, g_dock_row;

/* THEME_FUNCTIONS */

static void require(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void saved_theme_is(char key) {
    FILE *file = tmpfile(); require(file != NULL, "temporary config file");
    config_look_save(file); rewind(file);
    char line[2048], wanted[20]; int found = 0;
    snprintf(wanted, sizeof(wanted), "theme=%c\n", key);
    while (fgets(line, sizeof(line), file)) if (!strcmp(line, wanted)) found++;
    fclose(file);
    require(found == 1, "selected theme not saved exactly once");
}
int main(int argc, const char **argv) {
    require(argc == 2, "case required");
    if (!strcmp(argv[1], "fresh")) {
        require(g_scheme == scheme_by_key('g'), "fresh install does not select Gruvbox Dark");
        require(g_shown == g_scheme, "initial visible scheme differs from selection");
        palette_init();
        require(g_look.bg == 0x282828 && g_look.fg == 0xEBDBB2 && g_look.accent == 0xEBDBB2, "fresh drawing colors are not Gruvbox Dark");
        for (int i = 0; i < 16; i++) require(PAL[i] == g_scheme->pal[i], "initial ANSI palette differs from selected theme");
        saved_theme_is('g');
    } else if (!strcmp(argv[1], "saved-overrides")) {
        palette_init();
        for (int i = 0; i < NSCHEMES; i++) {
            char value[2] = {SCHEMES[i].key, 0};
            config_look("theme", value);
            require(g_scheme == SCHEMES+i && g_shown == g_scheme, "saved theme choice was overwritten by new default");
            require(g_look.bg == g_scheme->bg && g_look.fg == g_scheme->fg, "saved theme drawing colors differ");
            saved_theme_is(value[0]);
        }
        const Scheme *before = g_scheme;
        config_look("theme", "?");
        require(g_scheme == before, "unknown theme overrides saved choice");
        config_look("font", "H");
        require(g_scheme == before && g_font->key == 'H', "font setting changes theme selection");
    } else if (!strcmp(argv[1], "legacy")) {
        require(SCHEMES[0].key == 'h', "scheme array reordered; legacy identity broken");
        config_look("theme", "h");
        require(g_scheme == SCHEMES && g_shown == SCHEMES, "legacy theme key changed identity");
        require(g_look.accent == 0xFFA60D && g_look.bar == 0x1B1B1B && g_look.bold == 0xF2F2F2,
                "legacy hardcore appearance changed");
        saved_theme_is('h');
    } else require(0, "unknown case");
    printf("passed %s\n", argv[1]);
    return 0;
}
'''


def source_match(source, pattern):
    match = re.search(pattern, source, re.MULTILINE | re.DOTALL)
    if match is None:
        raise AssertionError('production declaration missing: '+pattern)
    return match.group(0)


@unittest.skipUnless(shutil.which('clang'), 'C compiler required')
class DefaultThemeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-default-theme-')
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        source = (ROOT/'hardcore.c').read_text()
        declarations = '\n'.join([
            source_match(source, r'typedef struct \{\n    char key;\n    const char \*name;\n    int dark;.*?\n\} Scheme;'),
            source_match(source, r'static const Scheme SCHEMES\[\] = \{.*?^\};\n#define NSCHEMES[^\n]*'),
            source_match(source, r'typedef struct \{\n    uint32_t bg, fg, bold, accent, sel, bar, hover;.*?\n\} Look;'),
            source_match(source, r'^static Look g_look;'),
            source_match(source, r'^static const Scheme \*g_scheme = [^\n]*'),
            source_match(source, r'^static const Scheme \*g_shown = [^\n]*'),
        ])
        functions = '\n\n'.join(production_function(ROOT/'hardcore.c', signature) for signature in [
            'static const Scheme *scheme_by_key(int key)',
            'static uint32_t mix(uint32_t a, uint32_t b, float t)',
            'static void look_set(const Scheme *s)',
            'static void palette_init(void)',
            'static void config_look(const char *key, const char *v)',
            'static void config_look_save(FILE *f)',
        ])
        harness = root/'theme.c'
        harness.write_text(HARNESS.replace('/* THEME_DECLARATIONS */', declarations)
                           .replace('/* THEME_FUNCTIONS */', functions))
        cls.binary = root/'theme'
        result = bounded_run(['clang', '-O0', '-Wall', '-Wextra', '-Werror',
                              str(harness), '-o', str(cls.binary)], timeout=30)
        if result.returncode:
            raise AssertionError(result.stderr)

    def case(self, name):
        result = bounded_run([str(self.binary), name], timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(result.stdout.strip(), 'passed '+name)

    def test_fresh_install_uses_gruvbox_dark(self): self.case('fresh')
    def test_every_saved_theme_remains_selectable_and_persisted(self): self.case('saved-overrides')
    def test_original_hardcore_palette_keeps_special_case(self): self.case('legacy')


class StaticIconTests(unittest.TestCase):
    def test_generated_icon_frames_use_gruvbox_palette(self):
        with tempfile.TemporaryDirectory(prefix='hc-default-icon-') as directory:
            output = Path(directory)/'hardcore.ico'
            result = bounded_run([sys.executable, str(ROOT/'tools/gen_icon.py'), str(output)], timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            icon = output.read_bytes()
        self.assertEqual(struct.unpack_from('<HHH', icon), (0, 1, 6))
        for index, size in enumerate((16, 24, 32, 48, 64, 256)):
            with self.subTest(size=size):
                _, _, _, _, _, _, length, offset = struct.unpack_from('<BBBBHHII', icon, 6+index*16)
                png = icon[offset:offset+length]
                self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
                chunks, at = {}, 8
                while at < len(png):
                    count = struct.unpack_from('>I', png, at)[0]
                    tag, payload = png[at+4:at+8], png[at+8:at+8+count]
                    chunks[tag] = chunks.get(tag, b'')+payload
                    at += count+12
                self.assertEqual(struct.unpack('>IIBBBBB', chunks[b'IHDR']), (size, size, 8, 6, 0, 0, 0))
                raw = zlib.decompress(chunks[b'IDAT'])
                pixels = []
                for row in range(size):
                    start = row*(size*4+1)
                    self.assertEqual(raw[start], 0)
                    pixels.extend(tuple(raw[x:x+4]) for x in range(start+1, start+1+size*4, 4))
                self.assertEqual(set(pixels), {(0, 0, 0, 0), (0x28, 0x28, 0x28, 255), (0xEB, 0xDB, 0xB2, 255)})
                self.assertEqual(pixels[0], (0, 0, 0, 0), 'rounded icon corner should stay transparent')
                foreground = sum(pixel == (0xEB, 0xDB, 0xB2, 255) for pixel in pixels)
                self.assertGreater(foreground, size*size*.05)
                self.assertLess(foreground, size*size*.4)


class DirectICNSTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-direct-icon-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.icon = cls.directory/'hardcore.icns'
        result = bounded_run([sys.executable, str(ROOT/'tools/gen_icon.py'), str(cls.icon)], timeout=15)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    def test_container_types_lengths_and_png_dimensions(self):
        icon = self.icon.read_bytes()
        self.assertEqual(icon[:4], b'icns')
        self.assertEqual(struct.unpack_from('>I', icon, 4)[0], len(icon))
        expected = {b'icp4':16, b'ic11':32, b'icp5':32, b'ic12':64, b'ic07':128,
                    b'ic13':256, b'ic08':256, b'ic14':512, b'ic09':512, b'ic10':1024}
        at, entries, actual_toc = 8, {}, b''
        while at < len(icon):
            kind, length = struct.unpack_from('>4sI', icon, at)
            self.assertGreaterEqual(length, 8)
            self.assertLessEqual(at+length, len(icon))
            self.assertNotIn(kind, entries)
            entries[kind] = icon[at+8:at+length]
            if kind != b'TOC ':
                actual_toc += icon[at:at+8]
            at += length
        self.assertEqual(at, len(icon))
        self.assertEqual(entries.pop(b'TOC '), actual_toc)
        self.assertEqual(set(entries), set(expected))
        for kind, image in entries.items():
            with self.subTest(kind=kind):
                self.assertEqual(image[:8], b'\x89PNG\r\n\x1a\n')
                self.assertEqual(image[12:16], b'IHDR')
                self.assertEqual(struct.unpack_from('>II', image, 16), (expected[kind], expected[kind]))

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('clang'), 'native macOS image decoder required')
    def test_appkit_and_imageio_decode_every_representation(self):
        source = self.directory/'decode.m'
        source.write_text(r'''
#import <Cocoa/Cocoa.h>
#import <ImageIO/ImageIO.h>
#include <stdio.h>
int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 1;
        NSString *path = [NSString stringWithUTF8String:argv[1]];
        NSImage *icon = [[NSImage alloc] initWithContentsOfFile:path];
        if (!icon || ![icon isValid] || [[icon representations] count] != 10) return 2;
        NSURL *url = [NSURL fileURLWithPath:path];
        CGImageSourceRef source = CGImageSourceCreateWithURL((CFURLRef)url, NULL);
        if (!source || CGImageSourceGetCount(source) != 10) return 3;
        int found[11] = {0};
        for (size_t i = 0; i < 10; i++) {
            CGImageRef image = CGImageSourceCreateImageAtIndex(source, i, NULL);
            if (!image || CGImageGetWidth(image) != CGImageGetHeight(image)) return 4;
            size_t width = CGImageGetWidth(image), shift = 0;
            while (width > 1 && !(width & 1)) { width >>= 1; shift++; }
            if (width != 1 || shift > 10) return 5;
            found[shift]++;
            if (CGImageGetAlphaInfo(image) == kCGImageAlphaNone) return 6;
            CGImageRelease(image);
        }
        if (found[4] != 1 || found[5] != 2 || found[6] != 1 || found[7] != 1 ||
            found[8] != 2 || found[9] != 2 || found[10] != 1) return 7;
        CFRelease(source); [icon release];
        puts("decoded all 10 ICNS images");
    }
    return 0;
}
''')
        binary = self.directory/'decode'
        result = bounded_run(['clang', '-O0', '-Wall', '-Wextra', '-Werror',
                              '-mmacosx-version-min=11.0', '-framework', 'Cocoa', '-framework', 'ImageIO',
                              str(source), '-o', str(binary)], timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        result = bounded_run([str(binary), str(self.icon)], timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(result.stdout.strip(), 'decoded all 10 ICNS images')


if __name__ == '__main__':
    unittest.main()
