#!/usr/bin/env python3
"""Native regression tests for menu cutouts and stable, atomic relocation.

All input goes through an isolated test application's FIFO, never the user's
focused terminal. Native screenshots are saved under build/chrome-adaptive/.
"""
import argparse
from pathlib import Path
import tempfile
import time

from test_chrome_mac import NativeApp, pixel, wait_for


def theme_menu(app):
    app.settings_section(1)
    app.hover(-1, -1)
    return app.chrome()


def test_notch(app):
    c = theme_menu(app)
    app.feed([""] * c["rows"])
    clean = app.snap("clean-menu")
    c = app.chrome()
    dock, cell = app.tv(c, "settings")["rect"], c["cell"]
    row = c["pages"]["settings"]["row"] + 2  # Monokai
    lines = [""] * c["rows"]
    # Keep an ambiguous literal row: no semantic reflow can change the test.
    # Its internal spaces must cut one continuous hole through the menu.
    lines = ["=" * (dock["x"] - 2) + "  c"] * c["rows"]
    before = app.state()
    app.feed(lines)
    notched = app.snap("monokai-notch")
    c = app.chrome()
    assert c["source"] == c["display"], "literal intrusion unexpectedly reflowed"
    mapping = next(item for item in app.tv(c, "settings")["menu_rows"] if item["row"] == row)
    assert mapping["left"] == dock["x"] and mapping["right"] == dock["x"] + 1, mapping
    # Label originally begins one column in, so use a second intrusion that
    # covers its M as in the report. The complete word must shift by one.
    lines = [line + "c" for line in lines]
    app.feed(lines)
    notched = app.snap("monokai-shifted")
    c = app.chrome()
    mapping = next(item for item in app.tv(c, "settings")["menu_rows"] if item["row"] == row)
    positions = [mapping["map"].index(source) for source in range(1, 8)]
    assert positions == list(range(2, 9)), mapping
    cw, ch = cell["width"], cell["height"]
    # Exclude the new one-pixel edge; all glyph pixels and backgrounds keep
    # their original colors and opacity after the translation.
    for source, dest in zip(range(1, 8), positions):
        for dy in range(2, ch - 1):
            for dx in range(2, cw - 1):
                assert pixel(clean, (dock["x"] + source) * cw + dx, row * ch + dy) == pixel(
                    notched, (dock["x"] + dest) * cw + dx, row * ch + dy), (source, dest, dx, dy)
    edge = (dock["x"] + 2) * cw
    assert pixel(notched, edge, row * ch + ch // 2) != pixel(notched, edge + 1, row * ch + ch // 2)
    # All text, including spaces between terminal words, owns its full span.
    lines = ["=" * (dock["x"] + 2) + "     Z"] * c["rows"]
    app.feed(lines)
    span = app.snap("continuous-intrusion")
    c = app.chrome()
    mapping = next(item for item in app.tv(c, "settings")["menu_rows"] if item["row"] == row)
    assert mapping["right"] == dock["x"] + 8, mapping
    assert mapping["map"][:8] == [-1] * 8, mapping
    empty = span.crop((dock["x"] + 2) * cw, row * ch + 2, 5 * cw, ch - 4)
    assert len(set(empty[i:i + 4] for i in range(0, len(empty), 4))) == 1, "menu leaked into terminal spaces"
    before_theme = c["schemekey"]
    c = app.click(dock["x"] + 1, row)
    assert c["schemekey"] == before_theme, "covered control remained clickable"
    lines = ["=" * (dock["x"] - 2) + "  cc"] * c["rows"]
    app.feed(lines)
    app.snap("shifted-control")
    c = app.click(dock["x"] + 2, row)
    assert c["schemekey"] == "m", "shifted word did not retain its theme control"
    assert [tab["pid"] for tab in app.state()["tabs"]] == [tab["pid"] for tab in before["tabs"]]
    print("PASS: complete menu words shift around continuous cutouts with opaque styles and one-pixel edges")


def observe(app, duration):
    end = time.monotonic() + duration
    samples = []
    while time.monotonic() < end:
        samples.append((time.monotonic(), app.chrome()))
        time.sleep(.015)
    return samples


def test_motion(app):
    c = app.quiet_tabs(10)
    app.hover(-1, -1)
    dock, rows = c["dock"], c["rows"]
    height = dock["height"]
    gap = min(rows - height - 2, height + 3)
    assert gap > dock["y"] + 2 and gap + height < rows, c
    lines = ["X" * c["cols"] for _ in range(rows)]
    lines[gap:gap + height + 1] = [""] * (height + 1)
    # Hover blocks both search and movement, even after the screen settles.
    app.hover(dock["x"] + 4, dock["y"])
    initial = app.feed(lines)
    held = observe(app, .8)
    assert all(s["dock"] == dock and s["motion"]["phase"] == 0 for _, s in held)
    app.hover(-1, -1)
    samples = observe(app, 4.0)
    final = samples[-1][1]
    assert final["mode"] == initial["mode"], "equally clear same-side location was ignored"
    assert gap <= final["dock"]["y"] <= gap + 1, (gap, final)
    assert final["dock"]["height"] == height, "movement shrank menu capacity"
    assert final["motion"]["commits"] == initial["motion"]["commits"] + 1, final["motion"]
    assert final["motion"]["phase"] == 0 and final["motion"]["alpha"] == 1, final["motion"]
    phases = {s["motion"]["phase"] for _, s in samples}
    assert {1, 2}.issubset(phases), phases
    geometry = [(s["mode"], s["dock"]["y"]) for _, s in samples]
    assert sum(a != b for a, b in zip(geometry, geometry[1:])) == 1, geometry
    first_fade = next(t for t, s in samples if s["motion"]["phase"] == 1)
    last_fade = next(t for t, s in samples if t > first_fade and s["motion"]["phase"] == 0)
    assert .45 <= last_fade - first_fade <= .9, last_fade - first_fade
    assert all(s["source"] == initial["source"] for _, s in samples), "motion changed terminal cells"
    app.snap("relocated-menu")
    # Without a better position, the menu remains stable and text wins.
    c = app.feed(["X" * c["cols"] for _ in range(rows)])
    no_fit = observe(app, 2.6)
    assert all(s["dock"] == c["dock"] for _, s in no_fit), "menu chased equally blocked positions"
    assert all(s["motion"]["commits"] == final["motion"]["commits"] for _, s in no_fit)
    app.snap("no-fit-text-priority")
    print("PASS: hover freezes motion; a settled layout moves once on the same side in ~0.6s; no-fit layout stays stable")


def test_opposite_side(app):
    c = app.quiet_tabs(8)
    app.hover(-1, -1)
    # Every right-side position is blocked by a literal word, but the left
    # side is empty. A clear other side must beat all partial same-side fits.
    lines = [" " * c["dock"]["x"] + "X" * c["dock"]["width"] for _ in range(c["rows"])]
    initial = app.feed(lines)
    final = wait_for(lambda: (s if (s := app.chrome())["mode"] == 2 and
                              s["motion"]["phase"] == 0 else None),
                     timeout=6, description="clear opposite-side menu placement")
    assert final["source"] == initial["source"]
    assert final["dock"]["height"] == initial["dock"]["height"]
    assert final["motion"]["commits"] == initial["motion"]["commits"] + 1
    app.snap("opposite-side-fallback")
    print("PASS: a clear opposite-side position wins when every same-side position is obstructed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, default=Path("build/chrome-adaptive"))
    args = parser.parse_args()
    artifacts = args.artifacts.resolve()
    with tempfile.TemporaryDirectory(prefix="hc-chrome-adaptive-") as directory:
        root = Path(directory)
        with NativeApp(root / "notch", 1, artifacts) as app:
            test_notch(app)
        with NativeApp(root / "motion", 1, artifacts, adaptive=True) as app:
            test_motion(app)
        with NativeApp(root / "opposite", 1, artifacts, adaptive=True) as app:
            test_opposite_side(app)
    print(f"PASS: native adaptive menu; screenshots: {artifacts}")


if __name__ == "__main__":
    main()
