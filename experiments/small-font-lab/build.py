#!/usr/bin/env python3
"""A self-contained native-pixel comparison, using Hardcore's glyph renderer.

No installed fonts or preferences change. Original BDFs are immutable. Missing
boxes can be completed with the production box generator; both versions are
exported through the actual C glyph_bits function, including Unifont fallback.
"""
from __future__ import annotations

import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SELECTED = sorted(set(range(32, 127)) | set(range(0x2500, 0x25A0)) |
                  {0x00B7, 0x2190, 0x2191, 0x2192, 0x2193, 0x2713, 0x2717})


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GEN = load_module('small_font_gen', ROOT / 'tools/gen_fonts.py')


def box_center(glyphs, width, height):
    """Match native light horizontal/vertical strokes, with an explicit fallback."""
    vertical = glyphs.get(0x2502, (width, [0] * height))[1]
    horizontal = glyphs.get(0x2500, (width, [0] * height))[1]
    cols = [sum(bool(row & (1 << (width - 1 - x))) for row in vertical)
            for x in range(width)]
    cx = cols.index(max(cols)) if any(cols) else (width - 1) // 2
    cy = max(range(height), key=lambda y: horizontal[y].bit_count()) if any(horizontal) else (height - 1) // 2
    return cx, cy


def c_renderer(configs, blob, directory):
    """Compile only unchanged production lookup/raster functions, then dump pixels."""
    source = (ROOT / 'hardcore.c').read_text()
    font_struct = re.search(r'typedef struct \{\n    char key;.*?\n\} Font;', source, re.S).group(0)
    functions = []
    for name in ('font_record', 'font_by_key', 'glyph_bits'):
        functions.append(re.search(r'^static [^\n]*\b' + name + r'\([^\n]*\) \{.*?^\}', source, re.M | re.S).group(0))
    slot = re.search(r'^#define SLOT \d+', source, re.M).group(0)
    rows = ['{0,"Unifont",8,16,1,0,0,-1}']
    for i, config in enumerate(configs):
        rows.append('{%d,"Candidate",%d,%d,1,0,0,%d}' % (i + 1, *config))
    harness = '''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
FONT_STRUCT
static const Font FONTS[] = {FONT_LIST};
#define NFONTS ((int)(sizeof(FONTS)/sizeof(FONTS[0])))
SLOT_DEFINE
static const unsigned char *fonts_blob, *font_blob, *font_blob_end;
FUNCTIONS
static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f=fopen(path,"rb"); if(!f) exit(2);
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    if(n<=0) exit(3);
    unsigned char *p=malloc((size_t)n);
    if(!p || fread(p,1,(size_t)n,f)!=(size_t)n) exit(4);
    fclose(f); *size=(size_t)n; return p;
}
int main(int argc, char **argv) {
    if(argc!=3) return 5;
    size_t size; fonts_blob=read_file(argv[1],&size);
    font_blob=read_file(argv[2],&size); font_blob_end=font_blob+size;
    const uint32_t cps[]={CODEPOINTS};
    putchar('[');
    for(int i=1;i<NFONTS;i++) {
        if(i>1) putchar(','); putchar('{');
        for(size_t j=0;j<sizeof(cps)/sizeof(cps[0]);j++) {
            if(j) putchar(',');
            unsigned char bits[SLOT*SLOT]={0};
            int w=glyph_bits(&FONTS[i],cps[j],bits);
            printf("\\\"%u\\\":{\\\"width\\\":%d,\\\"rows\\\":[",cps[j],w);
            for(int y=0;y<FONTS[i].h;y++) {
                uint32_t row=0;
                for(int x=0;x<w;x++) row=(row<<1)|(bits[y*SLOT+x]!=0);
                if(y) putchar(','); printf("%u",row);
            }
            printf("]}");
        }
        putchar('}');
    }
    puts("]"); free((void*)fonts_blob); free((void*)font_blob); return 0;
}
'''
    for key, value in {'FONT_STRUCT': font_struct, 'FONT_LIST': ',\n'.join(rows),
                       'SLOT_DEFINE': slot, 'FUNCTIONS': '\n'.join(functions),
                       'CODEPOINTS': ','.join(map(str, SELECTED))}.items():
        harness = harness.replace(key, value)
    cfile = directory / 'render.c'
    cfile.write_text(harness)
    (directory / 'fonts.bin').write_bytes(blob)
    compiler = shutil.which('cc') or shutil.which('clang')
    if not compiler:
        raise RuntimeError('A host C compiler is needed to use Hardcore glyph_bits')
    subprocess.run([compiler, '-std=c11', '-O2', str(cfile), '-o', str(directory / 'render')], check=True)
    output = subprocess.check_output([str(directory / 'render'), str(directory / 'fonts.bin'), str(ROOT / 'build/font.bin')])
    return json.loads(output)


def build_data():
    candidates = json.loads((HERE / 'candidates.json').read_text())
    fonts, configs, blob = [], [], bytearray()
    for spec in candidates:
        path = ROOT / spec['path']
        raw = gzip.decompress(path.read_bytes()) if path.suffix == '.gz' else path.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if spec.get('sha256') and spec['sha256'] != digest:
            raise ValueError('Source hash mismatch: ' + spec['id'])
        w, h = spec['width'], spec['height']
        omitted = []
        native = GEN.read_bdf(path, w, h, omit_overhang=True, omitted=omitted)
        assert set(range(33, 127)) <= native.keys(), spec['id']
        cx, cy = box_center(native, w, h)
        completed = dict(native)
        generated = []
        generation_fallback = []
        for cp in range(0x2500, 0x2580):
            if cp in native:
                continue
            pixels = GEN.box_glyph(cp, w, h, cx, cy, [0] * h)
            rows = [int(''.join(map(str, row)), 2) for row in pixels]
            if not any(rows):
                # The existing dash generator needs more room for some 8px
                # cells. Keep the real fallback instead of masking it with a
                # blank glyph; the inspector must still call this fallback.
                generation_fallback.append(cp)
                continue
            completed[cp] = (w, rows)
            generated.append(cp)
        for glyphs in (native, completed):
            configs.append((w, h, len(blob)))
            blob += GEN.table(glyphs)
        font = dict(spec, sha256=digest, generated=generated, generationFallback=generation_fallback, omitted=omitted,
                    nativeCodepoints=[cp for cp in SELECTED if cp in native],
                    center=[cx, cy], nativeBoxes=sum(cp in native for cp in range(0x2500, 0x2580)))
        license_path = ROOT / spec['license_path']
        font['licenseText'] = license_path.read_text()
        fonts.append(font)
    if not (ROOT / 'build/font.bin').exists():
        subprocess.run(['make', 'build/font.bin'], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='hardcore-small-fonts-') as tmp:
        rasters = c_renderer(configs, blob, Path(tmp))
    for i, font in enumerate(fonts):
        font['sourceGlyphs'], font['completedGlyphs'] = rasters[2 * i:2 * i + 2]
        # The I's middle body row excludes the top/bottom serifs. Report actual
        # ink width rather than treating every corner or diagonal as a stem.
        ink = [r for r in font['sourceGlyphs'][str(ord('I'))]['rows'] if r]
        font['iStemPixels'] = ink[len(ink) // 2].bit_count()
        font['capHeight'] = len([r for r in font['sourceGlyphs'][str(ord('H'))]['rows'] if r])
        font['xHeight'] = len([r for r in font['sourceGlyphs'][str(ord('x'))]['rows'] if r])
    return {'fonts': fonts, 'renderer': 'hardcore.c glyph_bits + tools/gen_fonts.py',
            'unifontLicense': (ROOT / 'font/unifont-license.txt').read_text()}


def main():
    data = build_data()
    assets = HERE / 'assets'
    assets.mkdir(exist_ok=True)
    (assets / 'font-data.json').write_text(json.dumps(data, ensure_ascii=False))
    template = (HERE / 'lab.html').read_text()
    packed = json.dumps(data, ensure_ascii=False, separators=(',', ':')).replace('</', '<\\/')
    output = HERE / 'small-font-lab.html'
    output.write_text(template.replace('/* FONT_DATA */ null', packed))
    print(f'{output}: {len(data["fonts"])} original bitmap fonts, C-rendered pixels')
    for font in data['fonts']:
        print(f'{font["name"]}: {font["width"]}x{font["height"]}, I stem {font["iStemPixels"]} px, {font["nativeBoxes"]}/128 native boxes')


if __name__ == '__main__':
    main()
