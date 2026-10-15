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

## Interactive experiment

The modified design is named **Hairline Lab**, derived from Terminus 12×24.
The source cell has baseline boundary 19, cap top 4, x-height top 8, and five
pixels of descent. Rows are zero-indexed; ordinary baseline ink ends at row 18.

- **Lowercase x-height, +0 to +6:** raises the lowercase body. Plain ascenders remain
  fixed through +2, then rise enough to retain a two-pixel lead over the body.
  The `f` hook moves with its crossbar, preserving the original curve and all
  three stem rows between them. The dots on `i` and `j` move with their bodies
  to retain separation. These protected features stay intact at every arrow position.
- **Cap height / digits, +0 to +6:** raises capital and numeral contours.
- **Cell / line height, 24 to 32:** adds leading above the glyph. This does not
  enlarge the letters by itself and retains the five-pixel descent. When the
  taller letters need more headroom, the effective cell grows automatically
  (up to a 28-pixel minimum at +6); the control shows requested → actual height.
- **Expansion arrows:** select any ASCII letter or digit, then select an
  insertion arrow and move it up or down. Positions snap to the next safe
  one-pixel stroke boundary, avoiding horizontal bars. Each glyph keeps its
  own overrides. **Automatic placement** restores only the selected glyph.
- **Extend box strokes:** redraws 15 basic light straight lines, corners, tees,
  crosses and half-lines to meet the changed cell boundaries. It does not
  redesign all Unicode box drawing.

The engine distributes insertions through each glyph, then moves the upper
contours and inserts sparse connector rows through matching one-pixel strokes.
For lowercase `e` at +2, source boundaries 11 and 15 put one addition above
the crossbar at row 13 and one below it. It never duplicates an entire
horizontal bar or scales a bitmap. Only ASCII `a-z`, `A-Z`, and `0-9` contours change. Punctuation
and other glyphs are translated with the cell's leading but not redesigned.

Tuning survives a page reload where the browser host supports local storage.
The **Save or load tuning** section exports a versioned JSON design for copying
and sharing; paste a saved design to restore its global controls and per-glyph
positions. It is a set of design parameters, not an installable font file.

These are prototypes: longer diagonal steps and the relationship between
lowercase bodies, ascenders and dots still deserve optical adjustment. Full
contour adjustments for Unicode, punctuation, and bold remain future work.
Wider cells are also a separate design decision.

The approved **+2 lowercase / +3 capitals / 26px cell** design now ships as
Hardcore's **H · Hairline · 12×26** option. Its [saved recipe](../../font/hairline/design.json)
uses this engine, including the corrected `f` hook; its [production BDF](../../font/hairline/README.md)
also preserves the source character coverage and adapts the full box/block
ranges to the taller cell. Further lab edits do not change that font until
it is explicitly regenerated.

At **1× device pixels**, a bitmap pixel maps to one browser backing pixel; CSS
dimensions divide by the reported device-pixel ratio. The canvases also snap
their origins to that grid. At DPR 2, a 24-pixel cell therefore occupies 12 CSS
pixels. The **2× enlarged** preview and pixel inspector help inspect shapes,
but should not be used to judge native hairline weight. Browser zoom and an OS
scaled display mode can still affect final physical display sampling.

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

Browser verification covered 736- and 320-pixel content widths at DPR 2,
Courier at both sizes, the heavier Terminus reference, maximum height controls,
and the glyph selector. Canvas dimensions mapped exactly to backing pixels,
origins aligned to the device grid, and the narrow layout did not overflow.
Fifteen automated variant tests passed. These checks validate the prototype renderer and
bitmap transformations; they do not establish optical quality for a full font.

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
