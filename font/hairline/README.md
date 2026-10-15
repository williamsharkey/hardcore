# Hairline 12×26

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
