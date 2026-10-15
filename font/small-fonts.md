# Small bitmap fonts

Hardcore includes every candidate from the small-font comparison. These are
native bitmap cuts: one font pixel is one device pixel at every display scale.
The dimensions below are cell width × cell height, including line spacing.
Existing fonts and their saved preference keys remain unchanged.

| Family | Cell | Preference key | Ordinary I/H/n stem |
| --- | --- | --- | --- |
| Terminus | 8×16 | `a` | 1 px |
| Terminus | 10×18 | `b` | 1 px |
| Terminus | 10×20 | `c` | 1 px |
| Terminus | 11×22 | `d` | 1 px |
| Terminus, existing reference | 12×24 | `t` | 1 px |
| Tamzen | 8×16 | `z` | 1 px |
| Tamzen | 10×20 | `x` | 1 px |
| Fixed | 9×18 | `f` | 1 px |
| Fixed, heavier comparison | 10×20 | `g` | 2 px |
| UW ttyp0 | 9×17 | `p` | 1 px |
| UW ttyp0 | 9×18 | `q` | 1 px |

The source BDFs are copied without changing their contents. Normal builds
need no downloads or installed fonts, and use only assets below `font/`.
`tests/test_small_fonts.py` compares every native packed glyph with an
independent source decoder, and the actual C renderer with all eleven approved
demo cuts. It also checks box joins, native stroke weights, device scale,
glyph-buffer boundaries, fallback behavior, and embedded notices.

Terminus retains its 120 native box glyphs. The eight missing dashed box
characters are generated using the existing native line centers. Tamzen has
no native Unicode box glyphs, so Hardcore generates them at the cell center.
The existing generator cannot fit four horizontal dashes into Tamzen's 8-pixel
cell; U+2508 and U+2509 therefore retain useful Unifont fallback instead of
becoming blank. Missing block and other Unicode characters also use Unifont.
Fixed and UW ttyp0 retain all 128 native box and 32 native block glyphs.
UW ttyp0 receives no additional font glyphs or design changes; characters
absent from it are handled separately by the terminal's fallback renderer.

## Sources and licenses

The complete applicable notices are kept beside their BDFs and embedded in
the generated font data included in each executable. Exact source hashes
and the original acquisition details remain in
[`experiments/small-font-lab/candidates.json`](../experiments/small-font-lab/candidates.json)
and [`README.sources.md`](../experiments/small-font-lab/README.sources.md).

- **Terminus 4.49.1:** `terminus/ter-u16n.bdf.gz`, `ter-u18n.bdf.gz`,
  `ter-u20n.bdf.gz`, and `ter-u22n.bdf.gz`, from the
  [original release archive](https://downloads.sourceforge.net/project/terminus-font/terminus-font-4.49/terminus-font-4.49.1.tar.gz).
  SIL Open Font License 1.1: [`terminus/OFL.TXT`](terminus/OFL.TXT).
- **Tamzen 1.11.6:** `tamzen/Tamzen8x16r.bdf.gz` and
  `Tamzen10x20r.bdf.gz`, from the
  [pinned upstream release](https://github.com/sunaku/tamzen-font/tree/Tamzen-1.11.6).
  Original Tamsyn/Tamzen permission notice: [`tamzen/LICENSE`](tamzen/LICENSE).
- **X11 Misc Fixed:** `misc-fixed/9x18.bdf.gz` and `10x20.bdf.gz`,
  from the [maintainer's Unicode-font archive](https://www.cl.cam.ac.uk/~mgk25/download/ucs-fonts.tar.gz),
  acquired 2026-10-15. Public domain notice: [`misc-fixed/COPYING`](misc-fixed/COPYING).
- **UW ttyp0 2.1:** `uw-ttyp0/t0-17-uni.bdf.gz` and
  `t0-18-uni.bdf.gz`, generated with the unmodified upstream build rules
  and default variants from the
  [original archive](https://people.mpi-inf.mpg.de/~uwe/misc/uw-ttyp0/uw-ttyp0-2.1.tar.gz).
  The 17-pixel cut uses the author's native 17-pixel overlay on the 18-pixel
  source, with no resampling. Original intermediate sources are retained as
  `t0-17-source.bdf.gz` and `t0-18-source.bdf.gz`; their encoding values are
  internal and these files must not be loaded as Unicode fonts. The upstream
  generation command is
  `make -f Makefile.in genbdf/t0-17-uni.bdf genbdf/t0-18-uni.bdf`.
  The ttyp0 License and upstream utility notice are preserved in
  [`uw-ttyp0/LICENSE`](uw-ttyp0/LICENSE).
