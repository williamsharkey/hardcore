# Hairline bitmap family

The Settings → Font → **Hairline** family contains the existing 12×26 design
and eleven optically reviewed companion strikes:

| Cell | Config key | Cap height | x-height | Baseline boundary |
| --- | --- | --- | --- | --- |
| 12×26 (existing) | `H` | 18 | 13 | 21 |
| 12×28 | `h` | 19 | 15 | 23 |
| 12×30 | `i` | 21 | 16 | 25 |
| 12×32 | `j` | 23 | 18 | 27 |
| 12×34 | `k` | 25 | 19 | 29 |
| 13×28 | `l` | 19 | 15 | 23 |
| 14×28 | `m` | 19 | 15 | 23 |
| 15×28 | `n` | 19 | 15 | 23 |
| 16×26 | `o` | 17 | 13 | 21 |
| 13×25 | `r` | 16 | 12 | 20 |
| 13×26 | `s` | 17 | 13 | 21 |
| 13×27 | `u` | 18 | 14 | 22 |

All twelve render at native device pixels, including on Retina. Each character's
advance equals its listed cell width. The existing 26px BDF and its config key are unchanged.

## Compact 13-wide cuts from Terminus 12×24

13×25, 13×26 and 13×27 start directly from the pinned original Terminus
12×24. They add one, two or three sparse continuation rows inside the bodies,
then widen counters/bars by one column without duplicating vertical stems.
Cap/ascender tops remain at row 4; lowercase bodies begin at row 8. The baseline
moves by the added height and the five-row descent is preserved. Native dots,
accents and compact punctuation retain their weight; mathematical signs and
slashes are balanced for each height. Box/block glyphs fill each cell, including
the odd 25- and 27-row strikes.

The same **260 common characters** were individually reviewed at each size,
plus mixed text, code and terminal-box samples. The [compact comparison](proofs/compact-comparison.png)
includes the unmodified Terminus 12×24 source. The BDFs retain 1,375 records;
Unicode outside the proof repertoire has not had the same individual review.

```sh
python3 tools/gen_hairline_compact.py
python3 tools/gen_hairline_compact.py --check
python3 tests/test_hairline_compact.py
python3 tools/proof_hairline_compact.py  # optional Pillow review sheets
```

Tests compare all common glyphs through the production rasterizer and check
source coverage, exact BDF bytes, dimensions, ASCII connectivity/stroke weight,
accent/body alignment, dots, box joins, odd-height blocks and buffer bounds.

## Wider variations of 12×28

The 13×28, 14×28, 15×28, and 16×26 designs are derived from the finished
12×28 cut. The contours widen along counters and horizontal strokes without
repeating vertical stems. Connected diagonal bridges preserve thin paths;
slashes, X/x, V/v and arithmetic signs have explicit balanced paths. Compact
punctuation, dots, copyright/registered marks and stars retain their native
contours. Accented Latin letters share the exact widened plain-letter body.

The 16×26 cut removes two continuation rows inside the lowercase body, moving
the baseline and descenders up two pixels while preserving the cap top, small
upper details, horizontal strokes and detached marks. Box/block elements are
redrawn for each complete cell, including odd widths and quadrant boundaries.
Heavy/double boxes and solid blocks retain their intended weights.

All **260 common characters per size** were visually inspected in enlarged
pixel proofs, followed by mixed text/code and terminal-box samples. See the
[width comparison](proofs/width-comparison.png) and individual `13x28`, `14x28`,
`15x28`, and `16x26` ASCII, extras and text sheets in [proofs](proofs/).
All 1,375 records from 12×28 remain; the rest of the Unicode repertoire is
transformed but has not received the same individual visual review.

```sh
python3 tools/gen_hairline_widths.py
python3 tools/gen_hairline_widths.py --check
python3 tests/test_hairline_widths.py
python3 tools/proof_hairline_widths.py  # optional Pillow review sheets
```

The width tests cover all common glyphs' connected components, ASCII stroke
weight, plain/accented body alignment, native mark size, metric anchors and
full-cell joins. A C harness compares every pixel of all 260 common characters
at all four sizes through the actual production renderer, including device
scales 1–3, buffer canaries and double-width Unicode fallback.

## New 28 / 30 / 32 / 34 designs

The new strikes are reproducible standalone designs from the pinned Terminus
12×24 source, independent of the web lab. `tools/gen_hairline_sizes.py` defines
their optical anchors and character-specific treatment. It inserts isolated
stems and diagonal paths without repeating horizontal bars or scaling the whole
bitmap. Four rows above capitals retain room for the original diacritics; the
five-row descent contains four rows of descender ink plus bottom spacing.

The review covered **every printable ASCII character (94)** and **166 additional
characters** at each size. The saved [proof sheets](proofs/) enumerate them by
Unicode codepoint. Checks included uppercase/lowercase proportions, numeral
alignment, O/0 and I/l/1 distinction, m/rn and v/w distinction, counter openness,
the `e` crossbar, `f` hook, `t` crossbar, i/j dots, descenders, punctuation,
Latin accents, currency, math, arrows, and terminal box/block joins. Mixed text,
code, and box samples were also inspected for each strike.

Optical corrections made during that review:

- Cap tops and `b d h k l` ascenders align; lowercase bowls use separately
  chosen x-heights. The `f` hook and `t` stem keep their small top details.
- i/j dots, periods, colons and commas retain native-width marks; their spacing
  changes with the body rather than making the marks taller.
- Grave, caret, tilde, underscore and curly quotes receive explicit placement.
  The arithmetic signs share a consistent mathematical axis.
- Accented Latin letters share their plain letters' main contours; detached
  accents retain a clear gap. Ligatures, sharp-s and slashed letters are retained.
- Common straight/diagonal arrows, partial derivative, product, sum and integral
  have explicit one-pixel paths, including glyphs absent in this Terminus source.
- Light box strokes remain one pixel and join across full cells. Heavy/double
  box symbols intentionally retain their distinct semantics; blocks and shades
  remain filled/patterned rather than being treated as thin letter strokes.

Each new BDF has **1,375 records**, retaining all 1,356 source glyphs and adding
terminal/symbol coverage. The remaining source Unicode repertoire receives
sparse contour expansion; it has not received the same individual optical review
as the 260 characters above. Missing Unicode still uses Hardcore's normal fallback.

Normal application builds consume the checked-in BDF files without Node or
Pillow. Explicit regeneration and verification:

```sh
python3 tools/gen_hairline_sizes.py
python3 tools/gen_hairline_sizes.py --check
python3 tests/test_hairline_sizes.py
python3 tools/proof_hairline_sizes.py  # optional Pillow review sheets
```

Tests check reproducible BDF payloads, full source coverage, metrics, every ASCII
character's connected components and stroke blocks, dots/operators/accents, and
box/block tiling. A C harness invokes the actual production font lookup and
rasterizer for every printable ASCII character in all four sizes, checking exact
pixels, native device scale, buffer bounds, and wide-character fallback.

## Existing 12×26 design

Hardcore's `H` font is the approved Hairline Lab design: lowercase x-height
**+2**, capital/digit height **+3**, cell height **26**, width **12**. It is drawn
at one device pixel per bitmap pixel, including on Retina displays. Its
baseline boundary is 21, cap height 18, x-height 13, and descent 5.

This is a modified version of Terminus 4.49.1's `ter-u24n.bdf`, originally
Copyright (C) 2020 Dimitar Toshkov Zhekov. The derivative uses the distinct
family name **Hairline** because the upstream OFL reserves “Terminus Font”.
The complete SIL Open Font License 1.1 is in [OFL.txt](OFL.txt) and is embedded
in Hardcore's font data. No new Reserved Font Name is declared.

## Design and coverage

The exact saved [design.json](design.json) can be loaded in Hairline Lab. Its
`d` and `f` arrow placements were retained from the selected design. ASCII
letters and digits come directly from the lab's variant engine, including
the fixed `f` hook and crossbar spacing; strokes are not scaled or emboldened.

All 1,356 source glyphs are retained. Non-ASCII letters, punctuation and
symbols keep their original pixels with two rows of leading added above;
their contours have not been optically adjusted to the enlarged ASCII.
Missing characters use Hardcore's normal Unifont fallback.

Terminal graphics are adapted to fill the 26-row cell:

- All 128 box drawing characters use the existing Hardcore box generator,
  with centre column 5 / row 12. The 15 basic light glyphs match the lab exactly.
  Light lines are one pixel; heavy and double lines retain distinct weights.
- All 32 block elements fill the cell as appropriate. Halves and quadrants
  split into 13-row halves; lower eighths round to the nearest row, half upward.
  Horizontal fraction masks and the two-row shade patterns retain their
  original pixels. Shades tile without a seam at the cell edge.
- Other symbols, including stacked mathematical pieces, keep the original
  baseline-relative bitmaps; they are not extended into the added leading.

This gives 1,366 BDF glyphs, including ten box/block characters absent from the
source. Hardcore omits control/space/private-use records as it does for its
other fonts; spaces render as empty cells.

## Rebuild and verify

Normal `make` builds read `hairline-12x26.bdf.gz` and need only the existing
Python font packer. Regenerating this font requires Node to run the same
variant engine as the interactive lab:

```sh
python3 tools/gen_hairline.py
python3 tools/gen_hairline.py --check
python3 tests/test_hairline_font.py
```

The source's decompressed SHA-256 is pinned in the generator. The gzip file
has a zero timestamp and is reproducible. Regeneration is explicit, so further
lab experiments do not silently change the shipped font. Review the BDF diff
and run the tests before accepting a changed recipe or engine.
