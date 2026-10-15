# Small fonts at native device size

An interactive comparison for lower-DPI Windows displays, focused on 17–20 px
cell heights and single-pixel straight stems. It opens as a local, self-contained
HTML file and does not change Hardcore's installed fonts or preferences.
All eleven comparison cuts, including the heavier Fixed 10×20 control, are
also available in Hardcore's Font settings, grouped by family with individual
size choices. The lab and application read the same canonical BDFs under
`font/`; see the [production font notes](../../font/small-fonts.md).

Build from the repository root:

```sh
python3 experiments/small-font-lab/build.py
```

Open `experiments/small-font-lab/small-font-lab.html` in a browser. The default
view places the existing Terminus 12×24 reference next to smaller candidates.
Choose **All candidates** to include the 16 px and 22 px options and the heavier
X11 Fixed 10×20 comparison. The page has terminal/TUI, box-join, and code samples,
dark and light backgrounds, an optional 2× view, and a clickable glyph inspector.

## Candidates

| Face | Cell | Cap height | I body | Native box characters |
| --- | --- | --- | --- | --- |
| UW ttyp0 | 9×17 | 11 px | 1 px | 128/128 |
| UW ttyp0 | 9×18 | 11 px | 1 px | 128/128 |
| Terminus | 10×18 | 12 px | 1 px | 120/128 |
| Terminus | 10×20 | 13 px | 1 px | 120/128 |
| X11 Fixed | 9×18 | 10 px | 1 px | 128/128 |
| Tamzen | 10×20 | 10 px | 1 px | 0/128 |
| Terminus reference | 12×24 | 15 px | 1 px | 120/128 |

Cell height includes leading and descent. Tamzen 20 has a smaller capital body
than Terminus 18; the nominal cell height alone does not establish reading size.
The I measurement excludes its horizontal top/bottom bars. Curves, diagonals,
serifs, and heavy box characters can have adjacent ink pixels even in a face
whose straight stems are one pixel wide. See [source notes](README.sources.md)
for all 11 candidates, exact versions, licenses, and acquisition details.

## Rendering and provenance

`build.py` reads original BDFs with `tools/gen_fonts.py`, refuses to clip
overhanging ink, and packs the same glyph-table format used in Hardcore. It
compiles the unchanged `font_record`, `font_by_key`, and `glyph_bits` functions
from `hardcore.c` into a temporary host-side renderer and exports its bitmap
results. No font outlines, browser fonts, interpolation, or synthetic thinning
are involved. The compiler and any temporary binary are build tools; the final
HTML embeds the pixel data and full font licenses.

Each candidate has two views of its glyph table:

- Original glyphs with Hardcore's ordinary Unifont fallback for missing glyphs.
- The same originals with absent U+2500–U+257F characters completed by Hardcore's
  existing `box_glyph` generator. The centers are inferred from the font's own
  light horizontal and vertical lines. Tamzen has neither, so its generated
  boxes use the integer cell center.

Native glyphs are never replaced by the box-completion option. If the existing
generator cannot fit a nonblank dashed glyph into a narrow cell (Tamzen 8×16's
horizontal quadruple dashes), the candidate retains Unifont fallback. The inspector
identifies original pixels, generated boxes, and Unifont fallback separately.
The fallback itself may have thicker pixels after fitting it to a cell. The
box-completed view matches the configurations shipped in Hardcore for all
eleven cuts. The original view remains useful for distinguishing the font's
own coverage from terminal-generated and fallback glyphs.

At 1×, canvas backing dimensions equal the original bitmap dimensions. CSS
dimensions are divided by `devicePixelRatio`, and canvas origins are snapped to
the device grid. Moving the window between displays, resizing, changing browser
zoom, and scrolling update that alignment. A narrow window scrolls a sample
horizontally rather than shrinking its letters. Native glyphs are regular
weight; the preview does not apply Hardcore's extra-pixel bold treatment.

## Verification

```sh
python3 experiments/small-font-lab/test_data.py
python3 tests/test_small_fonts.py
node experiments/small-font-lab/test_browser.cjs
```

The browser checks require Playwright Core and a Chromium executable; use the
environment overrides documented in the test script if they are not discovered
locally. Build the HTML before running browser checks. The renderer checks are
offline and do not require an installed browser.
The production regression suite also checks every native packed glyph against
its source BDF and every displayed sample glyph against the approved lab
rendering, including Fixed 20's two-pixel stems and narrow Tamzen dash fallback.

For verification on a real Windows display, launch a separate Chrome/Edge
profile with a local remote-debugging port and open the generated page. Then
run with a **Windows** Node runtime:

```sh
node experiments/small-font-lab/verify-windows.cjs http://127.0.0.1:9237
```

The helper attaches only to that preview, records the actual DPR and physical
canvas alignment, checks rendered pixels, and saves a native screenshot and
JSON report under `build/small-font-lab/`. It does not emulate a monitor, resize
the viewport, launch a browser, or close the preview. VS Code's bundled
Playwright Core is discovered automatically; `HARDCORE_PLAYWRIGHT_CORE` can
specify another installation.

The initial Windows 11 check on this machine used its 1920×1200 display at
96 DPI / 100% scale. Chrome reported DPR 1.00. This validates actual-size
sampling on this display; visual comfort still depends on the reader and panel.

Eight independent data tests passed, including source pixel placement, original
Unifont fallback, one-pixel stems, box-edge joins, and immutable source/license
files. The browser suite passed 32 scenarios at DPR 1, 1.25, 1.5, and 2, including
narrow windows, scrolling, both backgrounds, and provenance toggles. The native
Windows check additionally verified the visible browser without display emulation.
