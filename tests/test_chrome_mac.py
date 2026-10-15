#!/usr/bin/env python3
"""Native chrome/reflow integration through the real macOS GUI and Metal renderer.

Run after ``make`` on a logged-in Mac: python3 tests/test_chrome_mac.py.
Uses an isolated app/config, never types into the focused window, and saves
representative native screenshots under build/chrome-native/. No extra modules.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]


def wait_for(check, timeout=20, description="native chrome response"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.05)
    raise AssertionError(f"timed out waiting for {description}")


class Bitmap:
    """The test channel writes uncompressed 32-bit top-down BMP files."""
    def __init__(self, path):
        self.data = path.read_bytes()
        assert self.data[:2] == b"BM", path
        self.offset = struct.unpack_from("<I", self.data, 10)[0]
        self.width, height = struct.unpack_from("<ii", self.data, 18)
        bits = struct.unpack_from("<H", self.data, 28)[0]
        assert bits == 32 and height < 0, (bits, height)
        self.height = -height
        assert len(self.data) >= self.offset + self.width * self.height * 4

    def crop(self, x, y, width, height):
        assert 0 <= x <= x + width <= self.width, (x, width, self.width)
        assert 0 <= y <= y + height <= self.height, (y, height, self.height)
        return b"".join(self.data[self.offset + (row * self.width + x) * 4:
                                  self.offset + (row * self.width + x + width) * 4]
                        for row in range(y, y + height))


    def png(self, path):
        """A lossless PNG companion for reviewing native frames in browsers."""
        bgra = self.data[self.offset:self.offset + self.width * self.height * 4]
        rgba = bytearray(len(bgra))
        rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4] = bgra[2::4], bgra[1::4], bgra[0::4], bgra[3::4]
        scanlines = b"".join(b"\0" + rgba[row * self.width * 4:(row + 1) * self.width * 4]
                             for row in range(self.height))
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        header = struct.pack(">IIBBBBB", self.width, self.height, 8, 6, 0, 0, 0)
        path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                         + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


class NativeApp:
    def __init__(self, root, mode, artifacts):
        self.root = root
        self.mode = mode
        self.artifacts = artifacts
        self.sequence = 0
        self.pids = set()
        cfg = root / "config"
        cfg.mkdir(parents=True)
        (cfg / "config.ini").write_text(
            f"font=t\ntab_underlay={mode}\nchrome_reflow=1\nchrome_debug=1\n")
        self.fifo = root / "control"
        self.log = open(root / "app.log", "w+")
        install = root / "hardcore.app"
        shutil.copytree(ROOT / "hardcore.app", install)
        env = dict(os.environ, HARDCORE_CTL=str(self.fifo), HARDCORE_NO_ACTIVATE="1",
                   HARDCORE_TEST_CONFIG=str(cfg), HARDCORE_SHELL="/bin/zsh")
        self.proc = subprocess.Popen([str(install / "Contents/MacOS/hardcore"), "--cd", str(root)],
                                     env=env, stdout=self.log, stderr=self.log)
        try:
            wait_for(self.fifo.exists, description="isolated control FIFO")
            self.send("cmd resize 1001,591")
            self.send("type sleep 600\\n")
            wait_for(lambda: self.state()["tabs"][0]["program"] == "sleep", description="quiet PTY")
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        if exc_type:
            self.log.flush()
            self.log.seek(0)
            print(self.log.read(), file=sys.stderr)
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        for pid in self.pids:
            try:
                os.kill(pid, signal.SIGHUP)
            except ProcessLookupError:
                pass
        Path(f"/tmp/hardcore-{os.getuid()}/{self.proc.pid}.sock").unlink(missing_ok=True)
        self.log.close()

    def send(self, command):
        assert "\n" not in command, command
        assert self.proc.poll() is None, "isolated app exited"
        fd = os.open(self.fifo, os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(fd, (command + "\n").encode())
        finally:
            os.close(fd)

    def output(self, command, suffix="json"):
        self.sequence += 1
        path = self.root / f"response-{self.sequence}.{suffix}"
        self.send(f"{command} {path}")
        wait_for(lambda: path.exists() and path.stat().st_size, description=command)
        return path

    def state(self):
        result = json.loads(self.output("cmd state").read_text())
        self.pids.update(tab["pid"] for tab in result["tabs"])
        return result

    def chrome(self):
        return json.loads(self.output("cmd chrome").read_text())

    def map(self, col, row):
        return json.loads(self.output(f"cmd map {col},{row}").read_text())

    def feed(self, lines, suffix=""):
        self.sequence += 1
        path = self.root / f"fixture-{self.sequence}.txt"
        # Explicit cursor positioning preserves intentional hard row boundaries.
        # Put the hidden cursor far below the paragraph being tested.
        stream = "\x1bc\x1b[2J\x1b[3J\x1b[?25l"
        for row, text in enumerate(lines):
            if text:
                stream += f"\x1b[{row + 1};1H{text}"
        stream += f"\x1b[{len(lines)};1H" + suffix
        path.write_text(stream)
        self.send(f"cmd feed {path}")
        return self.chrome()

    def hover(self, col, row):
        c = self.chrome()
        self.send(f"cmd hover {int((col + .5) * c['cell']['width'])},"
                  f"{int((row + .5) * c['cell']['height'])}")
        return self.chrome()

    def click(self, col, row):
        c = self.chrome()
        self.send(f"cmd click {int((col + .5) * c['cell']['width'])},"
                  f"{int((row + .5) * c['cell']['height'])}")
        return self.chrome()

    def snap(self, name):
        path = self.output("snap", "bmp")
        # write_bmp writes header before pixel data; wait for the complete frame.
        def complete():
            try:
                return Bitmap(path)
            except (AssertionError, struct.error):
                return None
        bitmap = wait_for(complete, description="complete Metal snapshot")
        self.artifacts.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, self.artifacts / f"{name}.bmp")
        bitmap.png(self.artifacts / f"{name}.png")
        return bitmap


def words(lines):
    return re.findall(r"\S+", " ".join(lines))


def assert_mapping(app, c):
    moved = []
    for row, text in enumerate(c["display"]):
        for col, char in enumerate(text):
            if char.isspace():
                continue
            source = c["source"][row]
            if col >= len(source) or source[col] != char:
                moved.append((col, row, char))
    assert moved, "fixture did not expose any relocated characters"
    # Check across multiple output rows and within both ends of words.
    sample = moved[::max(1, len(moved) // 12)] + [moved[-1]]
    for col, row, char in sample:
        mapped = app.map(col, row)
        assert c["source"][mapped["row"]][mapped["col"]] == char, (col, row, char, mapped)


def test_reflow(app):
    c = app.chrome()
    assert c["mode"] == app.mode and c["reflow_enabled"], c
    cols, rows = c["cols"], c["rows"]
    dock = c["dock"]
    start = dock["y"]
    assert dock["height"] >= 2 and rows > start + 5, c
    # A near-full first row and a short last row: no blank row is required.
    first = "  " + " ".join(["word"] * ((cols - 4) // 5))
    second = "  the final words share existing space"
    assert len(first) + len(second) < 2 * (cols - dock["width"] - 2), c
    lines = [""] * rows
    lines[start:start + 3] = [first, second, "123"]
    lines[-2] = "456"
    c = app.feed(lines)
    assert c["reflow_changed"], c
    assert [line.rstrip() for line in c["source"]] == lines, c
    assert words(c["display"]) == words(c["source"]), c
    assert c["display"][start + 2].rstrip() == "123", c
    assert c["display"][-2].rstrip() == "456", c
    for row in (start, start + 1):
        line = c["display"][row].rstrip()
        if app.mode == 1:
            assert len(line) <= dock["x"], (dock, line)
        else:
            assert len(line) - len(line.lstrip()) == dock["width"] + 2, (dock, line)
    assert_mapping(app, c)
    after = app.chrome()
    assert after["source"] == c["source"] and after["display"] == c["display"], "render mutated terminal or drifted"
    app.snap("right-reflow" if app.mode == 1 else "left-reflow")
    print(f"PASS: {'right' if app.mode == 1 else 'left'} display reflow uses existing rows, preserves source/words/anchors, maps relocated glyphs")

    # Structured content is deliberately left alone even where it overlaps.
    protected = [""] * rows
    protected[start] = "  const message = \"" + "x" * max(4, cols - 27) + "\";"
    protected[start + 1] = "  | alpha | beta | gamma |"
    protected[start + 2] = "  first   second   aligned"
    c = app.feed(protected)
    assert c["source"] == c["display"], c
    # Meaningful terminal backgrounds, cursor rows, and mouse reporting must
    # keep the application's native geometry intact.
    styled = lines.copy()
    styled[start] = "\x1b[48;2;22;33;44m" + styled[start] + "\x1b[0m"
    c = app.feed(styled)
    assert c["source"] == c["display"], "styled background paragraph reflowed content"
    c = app.feed(lines, "\x1b[?1000h\x1b[?1006h")
    assert c["source"] == c["display"], "mouse-reporting mode reflowed content"
    c = app.feed(lines, f"\x1b[{start + 1};3H\x1b[?25h")
    assert c["source"] == c["display"], "cursor paragraph reflowed content"
    print("PASS: structured/styled output, mouse-reporting apps and cursor paragraphs keep source geometry")

    # Natural VT autowrap may split a word at a physical row boundary. It is
    # not a word separator, so guessing a prose join must never insert a space.
    paragraph = "word " * ((cols - 6) // 5) + "interoperability needs to retain its exact spelling"
    stream = f"\x1b[{start + 1};1H" + paragraph + f"\x1b[{rows};1H"
    c = app.feed([""] * rows, stream)
    first = c["source"][start]
    following = c["source"][start + 1]
    assert first[-1].isalpha() and following[0].isalpha(), "fixture did not split a word"
    assert first + following.rstrip() == paragraph, c
    assert c["source"] == c["display"], "VT softwrap inserted or removed a word separator"
    print("PASS: natural VT softwrap preserves a word split between physical rows")


def test_hover(app):
    c = app.chrome()
    c = app.click(c["dock"]["x"] + 6, c["pages"]["settings_row"])
    assert c["settings_open"], c
    dock, cell = c["dock"], c["cell"]
    # One long literal word cannot reflow; every menu row overlaps text.
    lines = [""] * c["rows"]
    for row in range(dock["y"], dock["y"] + dock["height"]):
        lines[row] = "X" * c["cols"]
    c = app.feed(lines)
    assert c["source"] == c["display"], c
    app.hover(-1, -1)
    back = app.snap(f"mode-{app.mode}-underlay")
    # Point at the first row and verify a distant row changes too.
    c = app.hover(dock["x"] + dock["width"] // 2, c["pages"]["tabs"]["row"])
    assert c["hovered"], c
    front = app.snap(f"mode-{app.mode}-hover-menu")
    for row in {dock["y"], dock["y"] + dock["height"] - 1}:
        box = (dock["x"] * cell["width"], row * cell["height"],
               dock["width"] * cell["width"], cell["height"])
        assert back.crop(*box) != front.crop(*box), ("menu row failed to rise", row)
    c = app.hover(-1, -1)
    assert not c["hovered"] and [line.rstrip() for line in c["source"]] == lines, c
    print(f"PASS: mode {app.mode} raises the entire obstructed menu on hover (actual Metal pixels)")


def page_button(c, kind, direction):
    page = c["pages"][kind]
    count = (page["total"] + page["capacity"] - 1) // page["capacity"]
    counter = f"{page['offset'] // page['capacity'] + 1}/{count}"
    local = 2 if direction < 0 else c["dock"]["width"] - (3 if kind == "tabs" else 1) - len(counter) - 3
    return c["dock"]["x"] + local, page["row"]


def walk_pages(app, kind):
    c = app.chrome()
    page = c["pages"][kind]
    capacity, total = page["capacity"], page["total"]
    assert 0 < capacity < total, (kind, page)
    font, scheme = c["fontkey"], c["schemekey"]
    while c["pages"][kind]["offset"]:
        before = c["pages"][kind]["offset"]
        c = app.click(*page_button(c, kind, -1))
        assert c["pages"][kind]["offset"] == max(0, before - capacity), c
    seen = set()
    while True:
        page = c["pages"][kind]
        assert page["capacity"] == capacity, (kind, page, capacity)
        assert (c["fontkey"], c["schemekey"]) == (font, scheme), "page arrow selected an item"
        seen.update(range(page["offset"], min(total, page["offset"] + capacity)))
        if page["offset"] + capacity >= total:
            break
        before = page["offset"]
        c = app.click(*page_button(c, kind, 1))
        assert c["pages"][kind]["offset"] == before + capacity, c
    assert seen == set(range(total)), (kind, seen, total)
    last = c["pages"][kind]["offset"]
    c = app.click(*page_button(c, kind, 1))
    assert c["pages"][kind]["offset"] == last, "next moved beyond the last page"
    c = app.click(*page_button(c, kind, -1))
    assert c["pages"][kind]["offset"] == max(0, last - capacity), "last partial page cannot return"
    c = app.click(*page_button(c, kind, 1))
    assert c["pages"][kind]["offset"] == last, "last partial page became unreachable"
    return c


def test_pagination(app):
    c = app.chrome()
    c = app.click(c["dock"]["x"] + 6, c["pages"]["settings_row"])
    assert c["settings_open"] and c["settings_tab"] == 0, c
    assert c["pages"]["settings"]["capacity"] == c["pages"]["settings"]["total"], "roomy font list unnecessarily paginated"
    c = app.click(c["dock"]["x"] + 10, c["pages"]["section_row"])
    assert c["pages"]["settings"]["capacity"] == c["pages"]["settings"]["total"], "roomy theme list unnecessarily paginated"
    app.snap("sidebar-full-settings")
    c = app.click(c["dock"]["x"] + 3, c["pages"]["section_row"])
    for expected in range(2, 12):
        app.send("cmd newtab")
        wait_for(lambda: len(app.state()["tabs"]) == expected, description="new test tab")
        app.send("type sleep 600\\n")
    app.send("cmd resize 1001,160")
    c = walk_pages(app, "settings")
    before_font = c["fontkey"]
    button = next(button for button in c["font_buttons"] if button["fontkey"] != before_font)
    c = app.click(c["dock"]["x"] + button["left"], button["row"])
    assert c["fontkey"] == button["fontkey"], "font size button selected the wrong font"
    assert c["cell"] == {"width": button["width"], "height": button["height"]}, c
    font_offset = c["pages"]["settings"]["offset"]
    app.snap("sidebar-font-last-page")
    c = app.click(c["dock"]["x"] + 10, c["pages"]["section_row"])
    assert c["settings_tab"] == 1, c
    c = walk_pages(app, "settings")
    before_scheme = c["schemekey"]
    c = app.click(c["dock"]["x"] + 8, c["pages"]["settings"]["row"])
    assert c["schemekey"] != before_scheme, "first paginated option row did not select its theme"
    theme_offset = c["pages"]["settings"]["offset"]
    app.snap("sidebar-theme-last-page")
    # The constrained viewport gives both lists independent pages.
    c = walk_pages(app, "tabs")
    visible = [row for row in c["tab_rows"] if row >= 0]
    assert visible and len(visible) < len(c["tab_rows"]), c
    assert all(b - a == c["cell"]["height"] for a, b in zip(visible, visible[1:])), visible
    app.snap("sidebar-tab-last-page")
    print("PASS: font/theme/tab pages reach every option and return from partial last pages; option rows remain selectable and tabs occupy one row")

    # Exercise the native Layout controls, including actual mode changes.
    c = app.click(c["dock"]["x"] + 20, c["pages"]["section_row"])
    assert c["settings_tab"] == 2, c
    widths = {}
    for expected in (1, 2, 0):
        c = app.click(c["dock"]["x"] + 8, c["pages"]["settings"]["row"])
        assert c["mode"] == expected, c
        widths[expected] = c["cols"]
    assert widths[1] == widths[2] > widths[0], widths
    c = app.click(c["dock"]["x"] + 8, c["pages"]["settings"]["row"] + 1)
    assert not c["reflow_enabled"], c
    c = app.click(c["dock"]["x"] + 8, c["pages"]["settings"]["row"] + 1)
    assert c["reflow_enabled"], c
    c = app.click(c["dock"]["x"] + 8, c["pages"]["settings"]["row"])
    assert c["mode"] == 1, c
    top = c["dock"]["y"]
    c = app.click(c["dock"]["x"] + c["dock"]["width"] - 3, c["pages"]["settings"]["row"] + 3)
    assert c["dock"]["y"] == top + 1, c
    config = (app.root / "config/config.ini").read_text()
    assert "tab_underlay=1" in config and "chrome_reflow=1" in config and f"dock_row={top + 1}" in config, config
    app.snap("right-settings-layout")
    print("PASS: Layout controls switch all modes, toggle reflow, move the dock by a grid row and persist settings")

    # Reload only this disposable install, preserving the actual process and
    # independent page positions. In particular Layout is index 2, not a bool.
    c = app.click(c["dock"]["x"] + 3, c["pages"]["section_row"])
    font_offset = c["pages"]["settings"]["offset"]
    c = app.click(c["dock"]["x"] + 11, c["pages"]["section_row"])
    theme_offset = c["pages"]["settings"]["offset"]
    c = app.click(c["dock"]["x"] + 20, c["pages"]["section_row"])
    keys = ("mode", "settings_open", "settings_tab", "fontkey", "schemekey", "reflow_enabled", "dock", "pages")
    before = {key: c[key] for key in keys}
    result = subprocess.run([str(app.root / "hardcore.app/Contents/MacOS/hardcore"), "--reload"],
                            capture_output=True, text=True, timeout=40)
    assert result.returncode == 0, result.stdout + result.stderr
    assert app.state()["pid"] == app.proc.pid, "reload changed app PID"
    c = app.chrome()
    assert {key: c[key] for key in keys} == before, (before, c)
    c = app.click(c["dock"]["x"] + 3, c["pages"]["section_row"])
    assert c["settings_tab"] == 0 and c["pages"]["settings"]["offset"] == font_offset, c
    c = app.click(c["dock"]["x"] + 11, c["pages"]["section_row"])
    assert c["settings_tab"] == 1 and c["pages"]["settings"]["offset"] == theme_offset, c
    print("PASS: in-place reload preserves Layout selection, dock settings, independent Font/Theme pages and app PID")


def pixel(bitmap, x, y):
    return bitmap.crop(int(x), int(y), 1, 1)


def test_codex_prose(app):
    import textwrap
    c = app.chrome()
    paragraph = ("All checks are passing: the macOS build, 12 native UI checks, 71 terminal fixtures, "
                 "reflow sanitizer tests, and live-update tests. Windows also passes a syntax check; "
                 "I haven’t run its GUI here. I’m committing the feature, pushing main, and installing the macOS build now.")
    wrapped = textwrap.wrap(paragraph, width=c["cols"], initial_indent="• ", subsequent_indent="  ")
    lines = [""] * c["rows"]
    for row, line in enumerate(wrapped):
        lines[row] = "\x1b[2;48;2;12;12;12m" + line.ljust(c["cols"]) + "\x1b[0m"
    lines[len(wrapped) + 4] = "123"
    c = app.feed(lines)
    assert c["reflow_changed"], "styled Codex bullet paragraph did not reflow"
    assert words(c["display"]) == words(c["source"]), "Codex words changed"
    assert_mapping(app, c)
    for row in range(c["dock"]["y"], c["dock"]["y"] + c["dock"]["height"]):
        text = c["display"][row].rstrip()
        if not text or text == "123":
            continue
        if app.mode == 1:
            assert len(text) <= c["dock"]["x"], (row, text)
        else:
            assert text.startswith(" " * c["dock"]["width"]), (row, text)
    app.hover(-1, -1)
    app.snap(f"mode-{app.mode}-codex-prose")
    print(f"PASS: styled Codex prose, Unicode punctuation and hanging bullet reflow around dock {app.mode}")


def test_visible_whitespace_and_rules(app):
    c = app.chrome()
    row = c["pages"]["tabs"]["row"]
    lines = [""] * c["rows"]
    lines[row] = "  + 46 lines (^t to expand)"
    c = app.feed(lines)
    app.hover(-1, -1)
    plain = app.snap("codex-plain-blank-tail")
    # Codex's harmless attributes and explicitly default background may extend
    # to the row edge, including blank cells after the expand annotation.
    lines[row] = "\x1b[2;3;48;2;12;12;12m" + lines[row].ljust(c["cols"]) + "\x1b[0m"
    c = app.feed(lines)
    dim = app.snap("codex-dim-blank-tail")
    d, cell = c["dock"], c["cell"]
    box = (d["x"] * cell["width"], row * cell["height"], d["width"] * cell["width"], cell["height"])
    assert plain.crop(*box) == dim.crop(*box), "invisible styled spaces still obscure the menu"
    lines[row] = "\x1b[48;2;60;70;80m" + " " * c["cols"] + "\x1b[0m"
    app.feed(lines)
    shaded = app.snap("codex-meaningful-background")
    assert shaded.crop(*box) != dim.crop(*box), "meaningful terminal background stopped masking menu"

    c = app.feed([""] * c["rows"])
    c = app.click(d["x"] + 6, c["pages"]["settings_row"])
    assert c["pages"]["settings"]["capacity"] == c["pages"]["settings"]["total"], c
    c = app.hover(d["x"] + 6, c["pages"]["tabs"]["row"])
    front = app.snap("right-collapsed-rules-full-settings")
    cw, ch = cell["width"], cell["height"]
    x = (d["x"] + 20) * cw
    boundary = c["pages"]["tabs"]["row"] * ch
    assert pixel(front, x, boundary) != pixel(front, x, boundary - 1), "tab border still occupies preceding device pixel"
    assert pixel(front, x, boundary) != pixel(front, x, boundary + 1), "tab border thicker than one device pixel"
    edge = (d["x"] + d["width"]) * cw - 1
    y = c["pages"]["settings"]["row"] * ch + ch // 2
    assert pixel(front, edge, y) == pixel(front, edge - 1, y), "right dock has a visible outside border"
    y = (c["pages"]["settings"]["row"] + 1) * ch
    assert pixel(front, edge - 2, y) == pixel(front, edge - 2, y - 1) == pixel(front, edge - 2, y + 1), "settings option rows still have separator rules"
    c = app.click(d["x"] + 6, c["pages"]["settings_row"])
    app.hover(-1, -1)
    print("PASS: invisible styled spaces reveal chrome; borders collapse to one pixel, settings omit row rules, right edge is open")


def test_title_underlay(app):
    c = app.chrome()
    app.feed([""] * c["rows"])
    c = app.hover(-1, -1)
    title = c["titlebar"]
    assert 0 < title["height"] < 5 * c["cell"]["height"], title
    assert not title["hovered"] and not title["buttons_visible"], title
    assert min(r for r in c["tab_rows"] if r >= 0) >= title["height"], "title hover obscures a clickable tab"
    back = app.snap("title-dim-underlay")
    b = title["button_rects"][0]
    x, y = int(b["x"] + b["width"] / 2), int(b["y"] + b["height"] / 2)
    p = pixel(back, x, y)
    assert p[0] == p[1] == p[2] and p[0] > 12, ("quiet traffic light is not a visible gray circle", p)
    # Fill the title rows with literal characters. Compare the cell containing
    # the circle with a distant identical cell: circle pixels must be beneath it.
    lines = [""] * c["rows"]
    for row in range((title["height"] + c["cell"]["height"] - 1) // c["cell"]["height"]):
        lines[row] = "X" * c["cols"]
    app.feed(lines)
    text = app.snap("title-text-over-circles")
    cw, ch = c["cell"]["width"], c["cell"]["height"]
    assert text.crop(x // cw * cw, y // ch * ch, cw, ch) == text.crop(40*cw, y // ch * ch, cw, ch), "traffic circle drew above terminal text"
    app.send(f"cmd hover {c['cols'] * cw // 2},{title['height'] // 2}")
    c = app.chrome()
    assert c["titlebar"]["hovered"] and c["titlebar"]["buttons_visible"], c["titlebar"]
    front = app.snap("title-hover-smoke")
    assert front.crop(40*cw, 0, cw, ch) != text.crop(40*cw, 0, cw, ch), "full-width title band did not smoke terminal text"
    app.hover(-1, -1)
    # Create scrollback and drag a real selection upwards into/out of the band.
    app.feed([""] * c["rows"], "history line\r\n" * (c["rows"] * 2))
    x = 40 * cw
    app.send(f"cmd pointer down {x},{title['height'] + 3*ch}")
    app.send(f"cmd pointer drag {x},{title['height'] // 2}")
    c = app.chrome()
    assert c["titlebar"]["selection_drag"] and c["titlebar"]["selection_on"], c["titlebar"]
    assert not c["titlebar"]["hovered"] and not c["titlebar"]["buttons_visible"], c["titlebar"]
    app.send(f"cmd pointer drag {x},-5")
    assert app.state()["tabs"][0]["view"] > 0, "upward selection stopped scrolling"
    app.send(f"cmd pointer up {x},-5")
    c = app.chrome()
    assert not c["titlebar"]["selection_drag"] and not c["titlebar"]["buttons_visible"], c["titlebar"]
    app.hover(-1, -1)
    print("PASS: gray traffic lights sit beneath text; top hover smokes full width; upward selection keeps controls hidden and scrolls")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, default=ROOT / "build/chrome-native")
    args = parser.parse_args()
    if sys.platform != "darwin":
        print("SKIP: macOS GUI required")
        return
    with tempfile.TemporaryDirectory(prefix="hardcore-chrome-test-") as tmp:
        columns = {}
        for mode in (0, 1, 2):
            with NativeApp(Path(tmp) / f"mode-{mode}", mode, args.artifacts) as app:
                c = app.chrome()
                assert c["mode"] == mode, c
                columns[mode] = c["cols"]
                if mode:
                    test_reflow(app)
                    test_codex_prose(app)
                    if mode == 1:
                        test_visible_whitespace_and_rules(app)
                        test_title_underlay(app)
                    test_hover(app)
                else:
                    app.snap("sidebar")
                    test_pagination(app)
        assert columns[1] == columns[2] > columns[0], columns
        print("PASS: both underlays use equal full terminal width; sidebar reserves only its actual grid width")
    print(f"Native chrome screenshots: {args.artifacts}")


if __name__ == "__main__":
    main()
