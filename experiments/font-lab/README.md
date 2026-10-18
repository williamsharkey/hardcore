# Hairline font lab

An isolated bitmap-font experiment for Hardcore. It compares source bitmaps in
the same sample TUI, with an enlarged pixel inspector. It does not change the
application's fonts, preferences, or build.

## What qualifies

“One pixel” means one **device pixel**, not one CSS point or a thin antialiased
outline. The straight stems and bars of Terminus 12×24 are one pixel. Its stock
14×28 and 16×32 letters have two-pixel stems. Hardcore already renders these
fonts at their native device-pixel dimensions.

The useful alternative found in this survey is the original X11 Adobe Courier
bitmap. The 75dpi `courR24` has a 15×24 cell, 14-pixel cap height and 11-pixel
x-height; the 100dpi `courR24` has a 20×29 line cell, 19-pixel cap height and
14-pixel x-height. Both have one-pixel straight stems and bars in `I`, `H`, and
`n`. Courier's serifs, diagonal joins, punctuation, and curves can contain
adjacent pixels. “Hairline” is not a promise that every local cross-section of
every character is one pixel wide. The 100dpi file's `PIXEL_SIZE` metadata is
34; the displayed 29 is its ascent plus descent, not that nominal size.

| Source font | Native cell | Straight stem observation |
| --- | --- | --- |
| Terminus regular | 12×24 | 1 px |
| X11 Adobe Courier, 75dpi | 15×24 | 1 px |
| X11 Adobe Courier, 100dpi | 20×29 line cell | 1 px |
| Terminus regular | 14×28, 16×32 | 2 px |
| Sony Fixed, ETL Fixed | 12×24 | 2 px |
| Spleen | 12×24, 16×32 | 2 px |
| UW ttyp0 | 14×30 | 2 px |
| Neep | 12×24 | 2 px |

This is a sampled source-bitmap inspection, not an exhaustive font catalogue.
The comparison includes the fonts already in Hardcore and the two Courier
candidates. Missing basic light box characters are explicitly identified as
generated framing, not attributed to the source typeface. Native source boxes
are otherwise retained; Hardcore's production build additionally redraws the
larger Terminus box characters, so those previews are not exact production
screenshots.

## Open the lab

```sh
python3 experiments/font-lab/serve.py
```

Open <http://127.0.0.1:50093/>. The server binds only to loopback, rebuilds the
bundle on startup, and needs only Python and the repository sources. It serves
a standalone page with no external scripts or fonts. Use `--port` if necessary;
keep the same origin to retain browser saves. Rebuild the bundle and refresh
when changing the UI or engine while the server is running.

## Interactive experiment

The desktop layout has three columns: canvas and guides, glyph manipulation,
and multiline text with reference comparison and A/B saves. Long text scrolls
inside its preview. Narrow windows fall back to two columns and then one.

All new edits start from **Terminus 12×24**. **Reset 24px** restores its exact
bitmaps, including original boxes. The reference menu also includes the shipped
hairline, Courier, and heavier strikes. Editing never changes installed fonts.

### Fixed canvas and unrestricted guide rows

**Canvas height** is the only sizing input. It accepts any positive whole number,
including 1, every height through 100, and larger values; there is no font-specific
minimum or maximum. As with any browser canvas, extreme dimensions eventually
encounter browser/memory limits. Moving guides or adding/removing arrows never
changes the requested height, adds padding, or makes room automatically.

The height input changes the canvas extent. Existing requested guide positions
are retained, clamped for display when the canvas is smaller, and restored when
space is available again. It does not automatically scale the letter. Adjust its
top and bottom guides to fill the new canvas.

All guides use **pixel row coordinates**, from 0 to height minus one. For example,
on a 24px canvas, set **Top row · caps + d** to 0 and **Bottom row · baseline** to
23 to make `d` touch both edges. Cap-top edits also move the plain lowercase
ascender and numeral guides. Those guides can subsequently be moved independently.
The x-height, dot, crossbar, and descender guides have the same full row range.
Guides can cross; their order is not enforced. The L/R checkboxes control visibility
on each grid. Drag a visible guide or edit its number.

The renderer maps source contour rows between these guides, joining neighboring
pixels to retain sparse stems. Compression merges pixels into the same row;
at one pixel high, all ink is collapsed into that row. Crossed guides can fold
contours. No optical-quality constraints silently override the user's geometry.

### Arrows

Drag **↑↦ Round up** or **↓↦ Round down** onto the source or working glyph to add
an arrow. Drag an existing handle to move it, or drop it in **Bin** to remove it.
Escape or a drop outside the grids cancels a drag. Clicks and keyboard controls
are also available; focused handles support Up/Down and Delete/Backspace. Exact
values are in the collapsed arrow panel.

The arrows redistribute contour spacing **within the fixed guide positions**.
They never increase the canvas height. Up/down chooses which direction to search
for a safe source row that avoids duplicating a horizontal stroke. If no safe row
exists in that direction, the nearest safe row is used and reported. Coarse
cap/x-height/bottom edits suggest more or fewer arrows as the letter span changes;
manual placements are retained where possible. There is no twelve-arrow limit
in fixed-canvas mode. The source grid shows requested rows, and the working grid
shows mapped safe rows. Some requests produce identical bitmaps; the report says
what changed, and keyboard move buttons skip identical results where possible.

### Preview and saves

The multiline text area drives both reference and working previews. Blank lines
and tabs are preserved (tabs become four spaces); long lines wrap. Unsupported
characters render as `?`. Text is limited to 12,000 characters and 256 rendered
rows. At 1×, one bitmap pixel maps to one backing/device pixel; 2× is enlarged.

**Save A/B** stores independent configurations. **Load A/B** edits a working copy
without overwriting the saved slot. **Compare A / B** displays A as reference and
loads B for editing. Current settings and both slots persist in local browser
storage. JSON version 4 stores fixed canvas height, explicit guide rows, arrows,
and visibility. Versions 1–3 still import; legacy snapshots retain their original
rendering as references, and are converted to fixed geometry when opened for editing.
The production generator's legacy recipe remains unchanged.

## Rebuild and verify

From the repository root:

```sh
python3 experiments/font-lab/build.py --bundle experiments/font-lab/hairline-font-lab.html
node --test tests/test_font_variants.mjs
```

The builder reads the repository BDFs and canonical `font/courier/` sources without
network requests. The final fragment embeds bitmap data, the variant engine,
and the UI. Font licenses accompany the data and are embedded in the fragment.
Generated `assets/font-data.json` and `hairline-font-lab.html` are ignored by Git;
the source template, engine, builder, licenses, and tests are versioned.
The tests use the actual shipped Terminus source and check all 441 combinations,
zero-change identity, connected strokes, unchanged baseline/descenders,
one-pixel `I`, balanced `e` growth, exact `f` hook preservation (including
x-height +2 / cap height +4 / cell height 26), 9,300 insertion override scenarios,
unchanged punctuation, source immutability, and basic box joins.

Earlier browser verification covered 736- and 320-pixel content widths at DPR 2,
Courier at both sizes, the heavier Terminus reference, maximum height controls,
and the glyph selector. Canvas dimensions mapped exactly to backing pixels,
origins aligned to the device grid, and the narrow layout did not overflow.
Twenty-eight automated variant tests pass, including global directions, shared suggestions,
36px growth, dot/ascender/descender controls, and crossbar movement. Current browser checks also cover 320px and 1100px layouts, live multiline text,
global direction changes, guide toggles, JSON round-tripping, and persistent A/B
saves. These checks validate the prototype renderer and
bitmap transformations; they do not establish optical quality for a full font.

## Additional survey (2026-10-17)

No additional native 26–36px single-stem bitmap was verified in this pass.
The 27px X11 Lucida Typewriter sample, 32px Spleen, and 30px UW ttyp0 samples
have wider stems; UW ttyp0's next size is 40px and is also heavier. Courier's
20×29 bitmap remains the larger qualifying comparison already in the lab.
The 30/32/36px buttons produce new lab variants instead of relabeling these
heavier fonts as hairlines.

## Sources

- [Terminus Font](https://terminus-font.sourceforge.net/), SIL Open Font License 1.1;
  the existing repository source is `font/terminus/ter-u24n.bdf.gz`.
- [Xorg Adobe 75dpi](https://gitlab.freedesktop.org/xorg/font/adobe-75dpi) and
  [75dpi source mirrored by NetBSD](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/font-adobe-75dpi/dist/courR24.bdf).
- [100dpi Courier source mirrored by NetBSD](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/font-adobe-100dpi/dist/courR24.bdf).
- [Spleen](https://github.com/fcambus/spleen),
  [UW ttyp0](https://people.mpi-inf.mpg.de/~uwe/misc/uw-ttyp0/), and
  [Neep](https://github.com/nikolas/jmk-x11-fonts).
- [Relief SingleLine](https://github.com/isdat-type/Relief-SingleLine) is another
  research route: actual open centerline paths with independently chosen stroke
  weight. It is proportional and is not a ready-made bitmap terminal font.
  Turning it into a one-pixel fixed-cell design would require fitting and
  grid rounding; it is not included as a qualifying bitmap candidate.

The line/arrow revision was checked against rendered browser screenshots: a
move changed 15 inspected `e` ink cells and 192 custom-text preview pixels.
Browser checks also verified numeric x-height growth, automatic arrow creation
while dragging the x-height line, and explicit Insert/Remove operations.

The desktop drag revision was verified at 1280×720 with 36px glyphs and no page
scrolling. Browser checks covered palette-to-grid insertion, row repositioning,
arrow-to-bin deletion, cap-linked `d` growth, and overall resizing. Thirty variant
tests and eight production-font tests pass; the new regressions cover connected
single-pixel ascenders and reversible overall resizing from 24 through 36px.

The fixed-canvas revision adds regressions for all heights 1–100, a 150px canvas,
`d` touching both edges, single-pixel connected stems at large sizes, freely
crossed guides, and 100 arrows without canvas growth. Browser checks also cover
line dragging, palette insertion and bin removal at fixed height, one-pixel and
100px rendering, and version-four JSON persistence.
