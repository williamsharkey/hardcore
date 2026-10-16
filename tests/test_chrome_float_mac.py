#!/usr/bin/env python3
"""Render a styled right-aligned status beside the primary menu and a TV."""
from pathlib import Path
import tempfile

from test_chrome_mac import NativeApp


def check(app, obstacle, name):
    c = app.chrome()
    cols, rows = c["cols"], c["rows"]
    text = "⚠ weekly limit: 6% left · /status"
    start = cols - len(text) - 1
    row = obstacle["y"] + 1
    target = obstacle["x"] - 2 - len(text)
    assert target > 0 and row < rows - 1
    lines = [""] * rows
    # A second identical span outside the panel supplies a native pixel
    # reference, with exactly the same truecolor, bold and hyperlink styles.
    style = "\x1b[1;38;2;211;160;66;48;2;45;53;69m\x1b]8;;https://example.com/status\x1b\\"
    reset = "\x1b]8;;\x1b\\\x1b[0m"
    lines[row] = " " * start + style + text + reset
    reference_row = rows - 2
    lines[reference_row] = " " * target + style + text + reset
    c = app.feed(lines)
    app.hover(-1, -1)
    c = app.chrome()
    assert c["source"][row].strip() == text
    assert c["display"][row].index(text) == target, (obstacle, c["display"][row])
    assert c["display"][row][target+len(text):].isspace()
    for y in range(rows):
        if y != row:
            assert c["display"][y] == c["source"][y], "status shift moved another row"
    for offset in (0, 2, 15, len(text)-1):
        mapped = app.map(target+offset, row)
        assert (mapped["row"], mapped["col"]) == (row, start+offset), mapped
    image = app.snap(name)
    cw, ch = c["cell"]["width"], c["cell"]["height"]
    assert image.crop(target*cw, row*ch, len(text)*cw, ch) == \
        image.crop(target*cw, reference_row*ch, len(text)*cw, ch), \
        "horizontal shift changed rendered glyphs, styles or backgrounds"
    assert app.chrome()["display"] == c["display"], "status drifted between paints"


def main():
    artifacts = Path("build/chrome-float-native").resolve()
    with tempfile.TemporaryDirectory(prefix="hc-float-native-") as root:
        with NativeApp(Path(root), 1, artifacts) as app:
            c = app.chrome()
            check(app, c["dock"], "status-beside-menu")
            app.feed([""] * c["rows"])
            c = app.open_settings()
            tv = app.tv(c, "settings")
            assert tv["placed"] and tv["rect"]["x"] > 0
            check(app, tv["rect"], "status-beside-settings-tv")
    print("PASS: native menu/TV status shifts preserve exact pixels, source mapping and other rows")


if __name__ == "__main__":
    main()
