# X11 Courier bitmap fonts

These original Adobe / Digital Equipment Corporation BDFs come from X.Org's
`font-adobe-75dpi` and `font-adobe-100dpi` packages, via the official NetBSD
source mirror. Complete copyright and permission terms are in [COPYING](COPYING).
The build also embeds that notice in `fonts.bin`, so it remains in the executable.

| Config key | Source | Cell in device pixels | Baseline / descent |
| --- | --- | --- | --- |
| `C` | `courier-75dpi-24.bdf.gz` | 15 × 24 | 19 / 5 |
| `R` | `courier-100dpi-24.bdf.gz` | 20 × 29 | 23 / 6 |

Both appear as **Courier** in the Font settings, with their dimensions beside
the name. Their pixels map 1:1 to device pixels, including Retina displays.
The `I`, `H` and `n` straight stems are one pixel wide. Curved joins and serifs
can contain adjacent pixels; this does not make every horizontal pixel run
one pixel long.

The 100dpi BDF has `PIXEL_SIZE 34` metadata. The 29-pixel line cell is its actual
`FONT_ASCENT 23` plus `FONT_DESCENT 6`; it is not a scaled-down 34-pixel drawing.
All printable ASCII and native box drawing are kept at their original pixels.
Some other original glyphs overhang the nominal line cell. The build omits those
whole glyphs and reports the count; Hardcore then uses its ordinary Unifont
fallback instead of showing a cropped Courier glyph. Missing glyphs, including
CJK, use that fallback too. Fallback shapes do not promise one-pixel strokes.

Source URLs:

- [75dpi original](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/font-adobe-75dpi/dist/courR24.bdf)
- [100dpi original](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/font-adobe-100dpi/dist/courR24.bdf)
- [X.Org 75dpi upstream](https://gitlab.freedesktop.org/xorg/font/adobe-75dpi)
- [X.Org 100dpi upstream](https://gitlab.freedesktop.org/xorg/font/adobe-100dpi)

SHA-256 of the uncompressed original files:

```text
01cef9a0e179b7d23ade8cd111ea3e205ffda8ebf02d5d430652e59d94f85181  courier-75dpi-24.bdf
336ce852c46f0bab88cd90deb249a266e624f975f96454f8f5f9a59beddb7664  courier-100dpi-24.bdf
```
