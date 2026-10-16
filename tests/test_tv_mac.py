#!/usr/bin/env python3
"""Native secondary-panel controls, compositing and lifecycle in owned apps.

Every event uses a disposable Hardcore test FIFO. CAF assertions belong only
to that fixture; no existing caf session or user's terminal receives input.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time

from test_chrome_mac import NativeApp, assert_mapping, pixel, wait_for, words


def rect_overlap(a, b):
    return (a["x"] < b["x"] + b["width"] and b["x"] < a["x"] + a["width"] and
            a["y"] < b["y"] + b["height"] and b["y"] < a["y"] + a["height"])


def visible_tvs(c):
    return [tv for tv in c["tvs"] if tv["open"] and tv["placed"]]


def valid_geometry(c):
    rectangles = [c["dock"]]
    for tv in visible_tvs(c):
        r = tv["rect"]
        assert 0 <= r["x"] < r["x"] + r["width"] <= c["cols"], tv
        assert 0 <= r["y"] < r["y"] + r["height"] <= c["rows"], tv
        assert 0 < tv["placement_candidates"] <= 12, tv
        assert all(not rect_overlap(r, previous) for previous in rectangles), (r, rectangles)
        rectangles.append(r)


def blank(app):
    app.send("cmd resize 1001,700")
    c = app.chrome()
    return app.feed([""] * c["rows"])


def header(app, local):
    # Keep call sites readable while sharing the production control table.
    return app.header_click({4:"settings",8:"term",12:"web",16:"files",21:"caf"}[local])


def close_tv(app, name):
    c = app.chrome()
    tv = app.tv(c, name)
    r = tv["rect"]
    return app.click(r["x"] + r["width"] - 2, r["y"])


def test_controls(app):
    c = blank(app)
    primary = c["dock"]
    assert primary["height"] == 3, "primary chrome should contain two header rows and one tab"
    assert not c["settings_open"] and not c["caf_active"]
    c = app.open_settings()
    settings = app.tv(c, "settings")
    assert settings["open"] and settings["placed"]
    assert settings["rect"]["x"] == primary["x"]
    assert settings["rect"]["y"] == primary["y"] + primary["height"] + 2
    assert c["dock"] == primary, "settings expanded the primary tab surface"
    valid_geometry(c)
    c = app.settings_section(1)
    before = c["schemekey"]
    c = app.click(app.settings_x(c) + 2, c["pages"]["settings"]["row"] + 2)
    assert c["schemekey"] == "m" and c["schemekey"] != before
    c = app.settings_section(0)
    button = next(button for button in c["font_buttons"] if button["fontkey"] != c["fontkey"])
    c = app.click(app.settings_x(c) + button["left"], button["row"])
    assert c["fontkey"] == button["fontkey"], "detached font control lost its hit coordinates"
    assert c["cell"] == {"width": button["width"], "height": button["height"]}
    valid_geometry(c)
    c = header(app, 21)
    try:
        assert c["caf_active"] and app.tv(c, "caf")["open"]
        assert c["settings_open"], "CAF replaced the independent settings TV"
        valid_geometry(c)
        sr, cr = app.tv(c, "settings")["rect"], app.tv(c, "caf")["rect"]
        if sr["y"] + sr["height"] + 2 + cr["height"] <= c["rows"]:
            assert cr["x"] == sr["x"] and cr["y"] == sr["y"] + sr["height"] + 2, \
                ("CAF did not prefer the clear space two rows below Settings", sr, cr)
        app.hover(-1, -1)
        app.snap("settings-and-caf")
        c = close_tv(app, "caf")
        assert not app.tv(c, "caf")["open"] and c["caf_active"], "closing CAF details turned off the assertion"
        assert c["settings_open"]
        c = header(app, 21)
        assert not c["caf_active"] and not app.tv(c, "caf")["open"]
        c = close_tv(app, "settings")
        assert not c["settings_open"]
        assert len(app.state()["tabs"]) == 1, "panel controls unexpectedly created or closed a terminal tab"
    finally:
        if app.chrome()["caf_active"]:
            header(app, 21)
    print("PASS: separate settings/CAF controls, font/theme hits and independent close behavior")


def test_header_actions(app):
    blank(app)
    header(app, 8)
    wait_for(lambda: len(app.state()["tabs"]) == 2, description="header plus terminal tab")
    app.send("type sleep 600\\n")
    wait_for(lambda: app.state()["tabs"][-1]["program"] == "sleep", description="quiet new header tab")
    app.feed([""] * app.chrome()["rows"])
    header(app, 12)
    wait_for(lambda: len(app.state()["tabs"]) == 3, description="header W browser tab")
    listed = subprocess.run([str(app.root/"hardcore.app/Contents/MacOS/hardcore"),
                             "browser", "list", "--window", str(app.proc.pid)],
                            capture_output=True, text=True, check=True, timeout=5)
    tabs = json.loads(listed.stdout)["tabs"]
    browser = next(tab for tab in tabs if tab["active"])
    assert browser["kind"] == "browser" and browser["url"] == "hardcore:welcome", browser
    assert app.state()["tabs"][-1]["pid"] == 0, "native browser unexpectedly launched a shell"
    app.snap("header-opened-native-browser")
    # Returning to the owned quiet terminal also checks that header controls
    # remain correctly mapped after the browser tab changed the chrome size.
    app.send("cmd tab 1")
    app.feed([""] * app.chrome()["rows"])
    header(app, 16)
    wait_for(lambda: len(app.state()["tabs"]) == 4, description="header F Files tab")
    state = app.state()
    files_pid = state["tabs"][state["active"]]["pid"]
    assert files_pid > 1
    def files_ready():
        c = app.chrome()
        return c if any("Files" in line for line in c["source"]) else None
    c = wait_for(files_ready, description="real Files application rendered")
    args = subprocess.run(["/bin/ps", "-p", str(files_pid), "-o", "command="],
                          capture_output=True, text=True, check=True, timeout=2).stdout
    assert "files.py" in args and str(app.root) in args, args
    app.snap("header-opened-files")
    assert len(app.state()["tabs"]) == 4
    print("PASS: +term, +www and files header controls open an owned shell, native browser and real Files tab")


def test_pixels(app):
    c = blank(app)
    c = app.settings_section(1)
    app.hover(-1, -1)
    c = app.chrome()
    image = app.snap("tv-rounded-edges-and-partial-column")
    cw, ch = c["cell"]["width"], c["cell"]["height"]
    assert image.width % cw, "fixture requires a partial rightmost terminal cell"
    background = pixel(image, 40*cw, 2*ch)
    for label, r in [("primary", c["dock"]), ("settings", app.tv(c, "settings")["rect"])]:
        assert r["x"] + r["width"] == c["cols"]
        x, y = r["x"]*cw, r["y"]*ch
        radius = min(6, cw//2, ch//3)
        assert pixel(image, x, y) == background, (label, "exposed corner was still square")
        assert pixel(image, x+radius+2, y) != background, (label, "top outline missing")
        edge_y = y + ch//2
        edge_color = pixel(image, c["cols"]*cw-2, edge_y)
        assert edge_color != background, (label, "header fixture has no visible background")
        assert all(pixel(image, xx, edge_y) == edge_color for xx in range(c["cols"]*cw, image.width)), \
            (label, "right partial pixel column was not filled")
        # The exposed side rule is exactly one physical pixel thick.
        edge_y = y + radius + 2
        assert pixel(image, x, edge_y) != pixel(image, x+1, edge_y), label
        assert pixel(image, x+1, edge_y) == pixel(image, x+2, edge_y), label
    print("PASS: native pixels show rounded exposed corners, one-pixel rules and a filled partial right edge")


def test_idle(app):
    c = blank(app)
    c = app.open_settings()
    app.hover(-1, -1)
    primary = c["dock"]
    time.sleep(1.2)
    c = app.chrome()
    tv = app.tv(c, "settings")
    assert 8 <= tv["countdown"] <= 9 and tv["alpha"] == 1, tv
    r = tv["rect"]
    c = app.hover(r["x"] + 10, r["y"])
    remaining = app.tv(c, "settings")["countdown"]
    time.sleep(1.2)
    c = app.chrome()
    assert app.tv(c, "settings")["countdown"] == remaining, "hover failed to pause idle countdown"
    assert app.tv(c, "settings")["alpha"] == 1
    app.hover(-1, -1)
    samples = []
    deadline = time.monotonic() + remaining + 2
    while time.monotonic() < deadline:
        c = app.chrome()
        tv = app.tv(c, "settings")
        samples.append((time.monotonic(), tv))
        if not tv["open"]:
            break
        time.sleep(.025)
    assert not tv["open"] and not c["settings_open"], "idle settings TV did not close"
    fading = [(when, item) for when, item in samples if 0 < item["alpha"] < 1]
    assert fading, "TV disappeared without its fade"
    assert .1 <= samples[-1][0] - fading[0][0] <= .55, "TV fade duration was not approximately 0.3 seconds"
    assert c["dock"] == primary, "secondary TV timeout moved the primary tabs"
    c = app.open_settings()
    assert app.tv(c, "settings")["countdown"] == 10 and app.tv(c, "settings")["alpha"] == 1
    app.snap("settings-reopened-after-idle")
    print("PASS: ten-second independent countdown pauses on hover, fades, closes and resets when reopened")


def test_composed_reflow(app):
    c = blank(app)
    c = app.settings_section(1)
    c = header(app, 21)
    try:
        assert c["caf_active"]
        lines = [""] * c["rows"]
        sentence = "The terminal preserves every word and its original source while separate panels use nearby space. "
        line = (sentence*5)[:c["cols"]-4].rsplit(" ", 1)[0]
        for row in range(2, c["rows"]-2, 3):
            lines[row] = line
        c = app.feed(lines)
        # Per-cell IPC probes span several frames. Hold the layout while
        # checking one snapshot, rather than racing CAF's normal relocation.
        c = app.hover(*app.header_point(c, "move"))
        valid_geometry(c)
        assert [row.rstrip() for row in c["source"]] == lines
        assert words(c["display"]) == words(c["source"]), "multiple surfaces dropped, duplicated or reordered words"
        assert c["reflow_changed"]
        assert len(visible_tvs(c)) == 2
        for r in [c["dock"]] + [tv["rect"] for tv in visible_tvs(c)]:
            for row in range(r["y"], r["y"] + r["height"]):
                assert not c["display"][row][r["x"]:r["x"]+r["width"]].strip(), (r, row, c["display"][row])
        assert_mapping(app, c)
        assert app.chrome()["source"] == c["source"]
        app.snap("primary-settings-caf-composed-reflow")
    finally:
        if app.chrome()["caf_active"]:
            # Clear the fixture before clicking the header; covered controls
            # correctly reject clicks until the terminal no longer intrudes.
            app.feed([""] * app.chrome()["rows"])
            header(app, 21)
    print("PASS: three simultaneous surfaces retain word order, source maps and previously cleared lanes")


def test_motion(app):
    c = blank(app)
    c = app.settings_section(1)
    original = app.tv(c, "settings")["rect"]
    c = app.hover(original["x"] + 10, original["y"])
    lines = [" " * original["x"] + "X" * original["width"] for _ in range(c["rows"])]
    initial = app.feed(lines)
    deadline = time.monotonic() + .8
    while time.monotonic() < deadline:
        c = app.chrome()
        tv = app.tv(c, "settings")
        assert tv["rect"] == original and tv["motion"]["phase"] == 0, "hover allowed TV relocation"
        time.sleep(.03)
    app.hover(-1, -1)
    samples = []
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        c = app.chrome()
        tv = app.tv(c, "settings")
        samples.append((time.monotonic(), tv))
        assert c["source"] == initial["source"] == c["display"]
        if tv["rect"]["x"] == 0 and tv["motion"]["phase"] == 0:
            break
        time.sleep(.02)
    assert tv["rect"]["x"] == 0, "blocked Settings TV did not choose the clear opposite edge"
    phases = {item["motion"]["phase"] for _, item in samples}
    assert {1, 2}.issubset(phases), phases
    geometries = [item["rect"] for _, item in samples]
    assert sum(a != b for a, b in zip(geometries, geometries[1:])) == 1, "TV moved in multiple layout steps"
    assert all(item["rect"] == original for _, item in samples if item["motion"]["phase"] == 1)
    first_fade = next(when for when, item in samples if item["motion"]["phase"] == 1)
    assert .45 <= samples[-1][0] - first_fade <= .95, "TV relocation did not finish in about 0.6 seconds"
    valid_geometry(c)
    app.snap("settings-relocated-to-clear-edge")
    final = tv["rect"]
    app.feed(["X" * c["cols"]] * c["rows"])
    deadline = time.monotonic() + 2.6
    while time.monotonic() < deadline:
        c = app.chrome()
        tv = app.tv(c, "settings")
        assert tv["rect"] == final and tv["motion"]["phase"] == 0, "TV chased an equally blocked location"
        time.sleep(.04)
    print("PASS: TV hover freezes relocation, a clearer edge commits once through a 0.6-second fade, and no-fit layouts stay put")


def pointer_move(app, c, col, row, action="move"):
    """Real event path: this hook deliberately does not invalidate afterward."""
    x = int((col+.5)*c["cell"]["width"])
    y = int((row+.5)*c["cell"]["height"])
    app.send(f"cmd pointer {action} {x},{y}")


def resize_grid(app, cols, rows):
    c = app.chrome()
    app.send(f"cmd resize {cols*c['cell']['width']//c['scale']},"
             f"{rows*c['cell']['height']//c['scale']}")
    c = app.chrome()
    assert (c["cols"], c["rows"]) == (cols, rows), c
    return c


def test_pointer_exit(app, browser=False):
    blank(app)
    app.quiet_tabs(11)
    if browser:
        from test_tab_preview_extended_mac import browser_command
        html = app.root / "tv-exit.html"
        html.write_text("<!doctype html><title>TV exit fixture</title><body></body>")
        identity = browser_command(app, "open", str(html), "--mode", "native")["id"]
        wait_for(lambda: any(t["id"] == identity and t["title"] == "TV exit fixture"
                             for t in browser_command(app, "list")["tabs"]),
                 description="quiet native browser fixture")
    c = resize_grid(app, 100, 14)
    if not browser:
        c = app.feed([""]*c["rows"])
    c = app.settings_section(1)
    settings = app.tv(c, "settings")
    assert settings["rect"]["x"] == 0 < c["dock"]["x"], settings
    action = "monitor" if browser else "move"
    pointer_move(app, c, 2, c["pages"]["settings"]["row"]+2, action)
    def preview_ready():
        c = app.chrome()
        return c if c["shown_schemekey"] == "m" else None
    c = wait_for(preview_ready, description="opposite-edge Monokai hover preview")
    committed = c["schemekey"]
    assert committed != c["shown_schemekey"] and not c["hovered"]
    before = app.tv(c, "settings")["countdown"]
    time.sleep(1.1)
    c = app.chrome()
    assert app.tv(c, "settings")["countdown"] == before, "real pointer hover did not pause Settings"
    # Let the last query's queued paint finish while the pointer is still in
    # the panel. No polling, snapshot or forced redraw may mask the exit bug.
    time.sleep(.15)
    pointer_move(app, c, 45, 10, action)
    time.sleep(1.3)
    c = app.chrome()
    assert c["shown_schemekey"] == committed, "leaving opposite-edge Settings retained the hover theme"
    assert app.tv(c, "settings")["countdown"] < before, "Settings countdown stayed paused after real pointer exit"
    close_tv(app, "settings")
    c = header(app, 21)
    try:
        caf = app.tv(c, "caf")
        assert c["caf_active"] and caf["rect"]["x"] == 0 < c["dock"]["x"], caf
        pointer_move(app, c, 10, caf["rect"]["y"]+2, action)
        time.sleep(.1)
        c = app.chrome()
        before = app.tv(c, "caf")["countdown"]
        time.sleep(1.1)
        c = app.chrome()
        assert app.tv(c, "caf")["countdown"] == before, "real pointer hover did not pause CAF"
        time.sleep(.15)
        pointer_move(app, c, 45, 10, action)
        time.sleep(1.3)
        c = app.chrome()
        assert app.tv(c, "caf")["countdown"] < before, "CAF countdown stayed paused after real pointer exit"
        assert c["caf_active"], "leaving CAF turned off its assertion"
        app.snap("native-browser-pointer-exit" if browser else "terminal-pointer-exit")
    finally:
        if app.chrome()["caf_active"]:
            header(app, 21)
    print(f"PASS: {'native WK monitor' if browser else 'HCView'} real pointer exit restores theme and resumes opposite-edge Settings/CAF countdowns")


def test_opposite_hover(app):
    blank(app)
    app.quiet_tabs(11)
    c = resize_grid(app, 100, 14)
    app.feed([""]*c["rows"])
    c = app.settings_section(1)
    tv = app.tv(c, "settings")
    assert tv["rect"]["x"] == 0 < c["dock"]["x"]
    pointer_move(app, c, 45, 10)
    app.chrome()
    before = app.snap("opposite-tv-before-hover")
    # Hover the Font/Theme selector: it shares a row with a primary tab but
    # does not preview a theme or a font, so an exact pixel comparison is valid.
    row = c["pages"]["section_row"]
    pointer_move(app, c, 4, row)
    c = app.chrome()
    after = app.snap("opposite-tv-after-hover")
    assert not c["hovered"]
    x, y = c["dock"]["x"]*c["cell"]["width"], row*c["cell"]["height"]
    width, height = c["dock"]["width"]*c["cell"]["width"], c["cell"]["height"]
    assert before.crop(x, y, width, height) == after.crop(x, y, width, height), \
        "hovering the opposite TV highlighted or drew a close glyph on the primary tab"
    print("PASS: opposite-edge TV hover leaves the same-row primary tab pixels unchanged")


def test_joined_pixels(app):
    blank(app)
    app.quiet_tabs(7)
    c = resize_grid(app, 50, 24)
    c = app.feed([""]*c["rows"])
    cw, ch = c["cell"]["width"], c["cell"]["height"]
    x = int((c["dock"]["x"]+1.5)*cw)
    app.send(f"cmd pointer down {x},{ch//2}")
    target_x = int((c["cols"]-1.5)*cw)
    app.send(f"cmd pointer drag {target_x},{3*ch+ch//2}")
    app.send(f"cmd pointer up {target_x},{3*ch+ch//2}")
    c = app.settings_section(1)
    pointer_move(app, c, 10, 20)
    c = app.chrome()
    primary, secondary = c["dock"], app.tv(c, "settings")["rect"]
    assert primary["x"] == secondary["x"] and primary["y"]+primary["height"] == secondary["y"], (primary, secondary)
    image = app.snap("joined-primary-settings-seam")
    x, y = secondary["x"]*cw, secondary["y"]*ch
    # A point beyond the labels avoids glyphs and compares the actual rule to
    # the uniform body pixels on either side. The lower header has no top rule.
    inner = x+20*cw
    assert pixel(image, inner, y-1) != pixel(image, inner, y-2), "joined seam disappeared"
    assert pixel(image, inner, y) == pixel(image, inner, y+1), "joined seam is two pixels thick"
    assert pixel(image, inner, y) != pixel(image, inner, y-1), "lower panel repeated the upper border"
    background = pixel(image, 10*cw, y)
    assert pixel(image, x, y-1) != background and pixel(image, x, y) != background, \
        "touching menu corners remained rounded and left a hole"
    assert pixel(image, x, primary["y"]*ch) == background, "exposed primary corner lost its rounding"
    assert pixel(image, x, (secondary["y"]+secondary["height"])*ch-1) == background, \
        "exposed settings corner lost its rounding"
    print("PASS: touching native panels share one pixel seam and square joined corners while exposed corners stay rounded")


def main():
    cases = {"controls": test_controls, "actions": test_header_actions, "pixels": test_pixels, "idle": test_idle,
             "reflow": test_composed_reflow, "motion": test_motion,
             "pointer-exit": test_pointer_exit, "browser-pointer-exit": lambda app: test_pointer_exit(app, True),
             "opposite-hover": test_opposite_hover, "joined-pixels": test_joined_pixels}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=cases)
    parser.add_argument("--artifacts", type=Path, default=Path("build/tv-native"))
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="hc-tv-native-") as directory:
        for name, check in cases.items():
            if args.case and args.case != name:
                continue
            with NativeApp(Path(directory)/name, 1, args.artifacts.resolve()) as app:
                try:
                    check(app)
                except AssertionError as error:
                    failures.append(name)
                    app.snap(name + "-failure")
                    print(f"FAIL: {name}: {error}", flush=True)
    if failures:
        raise SystemExit("FAILED: " + ", ".join(failures))
    print(f"PASS: native TVs; screenshots: {args.artifacts.resolve()}")


if __name__ == "__main__":
    main()
