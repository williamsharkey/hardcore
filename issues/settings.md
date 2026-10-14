# Settings: a font and a color scheme (done on branch `settings`, 2026-10-14)

## What was asked

- Under the tabs, a row with a gray **s** on the left and **+** on the right.
  Hovering **s** says "settings"; clicking it opens the settings below, in the
  tab column.
- A grid of fonts, a box with a letter each, grouped by cell size. Hovering
  one tries it on the terminal; clicking sets it for every window. No
  per-tab or per-window settings.
- Same for color schemes: popular dark and light ones, a square each.
- A single space around every option.
- The tab column on the character grid, like the terminal. When the
  traffic lights are taller than one row, tabs start on the third row, not
  the second.
- A letter for every setting, so two letters (`mT`) describe a setup. No
  command-line flag yet.

## What's there

- **Fonts** (`tools/gen_fonts.py` → `build/fonts.bin`, own glyphs only;
  Unifont fills the rest, squeezed to the cell):
  `U` Unifont, `Z` Tamzen 8×15, `F` misc-fixed 7×13 (all scaled by the
  display, 2×2 per pixel on Retina); `t` Terminus 24, `Y` Sony 12×24,
  `E` ETL 24, `M` Terminus 28, `T` Terminus 32 (1:1 device pixels).
  Sizes in the grid are device pixels, so the scaled ones show doubled.
- **Box lines 1 device pixel thin.** Scaled fonts at scale 2: each lit pixel
  of a box-drawing glyph becomes a point joined to its neighbours (the
  font's own shapes stay). Terminus 28 and 32: box drawing redrawn from the
  Unicode names on Terminus's centre lines (their own is 2 px).
- **Schemes**: `h` hardcore (default), `d` Dracula, `m` Monokai, `n` Nord,
  `g`/`G` Gruvbox, `s`/`S` Solarized, `o`/`O` One Dark/Light, `c`/`C`
  Catppuccin Mocha/Latte, `k`/`K` Tokyo Night/Day, `r`/`R` Rosé Pine/Dawn.
  Lowercase dark, uppercase the light twin. Palettes from
  iTerm2-Color-Schemes (MIT); four selection colors replaced because the
  originals hid selected text (Nord, Gruvbox Light, both Catppuccins).
- **Hover** tries a font on the terminal only. The tab column keeps the font
  you picked, so the grid doesn't move under the mouse. A scheme is tried on
  the whole window. **Click** saves `font=` / `theme=` in config.ini and tells
  the other windows (Windows: a posted message to every `hardcore` window
  class; macOS: a distributed notification). Each rereads config.ini.
- The default (Unifont, hardcore scheme) draws the terminal pixel for pixel
  as before, except box drawing at scale 2, which is now 1 px. At scale 1
  (the ThinkPad) nothing in the terminal changes. The tab column is now on
  the grid: one row per tab, no 10 px gaps.

## Tested

- macOS: `make test` (unit, xterm.js 71/71, selftest). Pixel diff against
  `main` with the defaults: only the three box-drawing rows differ. Hover
  preview, revert, pick, config.ini written, a second window following the
  pick, a light scheme (traffic lights switch to light), every font with a
  TUI mockup of frames, bars, braille and dashes.
- Windows: cross-compiles with mingw (no new warnings). **Not run.** The
  Direct3D atlas changed size (4096², 32 px slots) and the tab column layout
  changed. Run it on the ThinkPad: tabs, s/+, the panel, picking a font,
  picking a scheme with two windows open, the caption buttons on a light
  scheme, the low-disk warning.

## Open

- `hardcore -mT` (the two letters as a flag) isn't implemented yet.
- The 1:1 fonts on a 1× display are 1.5–2× bigger than Unifont there; on a
  3× display they're small. Fine for now.
