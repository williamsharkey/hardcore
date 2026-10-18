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
    def __init__(self, root, mode, artifacts, adaptive=False):
        self.root = root
        self.mode = 2 if mode == 0 else mode
        self.artifacts = artifacts
        self.sequence = 0
        self.pids = set()
        cfg = root / "config"
        cfg.mkdir(parents=True)
        (cfg / "config.ini").write_text(
            f"font=t\ntheme=h\ntab_underlay={mode}\nchrome_reflow=0\nchrome_debug=1\n")
        self.fifo = root / "control"
        self.log = open(root / "app.log", "w+")
        install = root / "hardcore.app"
        shutil.copytree(ROOT / "hardcore.app", install)
        env = dict(os.environ, HARDCORE_CTL=str(self.fifo), HARDCORE_NO_ACTIVATE="1",
                   HARDCORE_TEST_CHROME_MOTION="1" if adaptive else "0",
                   HARDCORE_TEST_CONFIG=str(cfg), HARDCORE_SHELL="/bin/zsh")
        self.proc = subprocess.Popen([str(install / "Contents/MacOS/hardcore"), "--cd", str(root)],
                                     env=env, stdout=self.log, stderr=self.log)
        try:
            wait_for(self.fifo.exists, description="isolated control FIFO")
            self.send("cmd resize 1001,591")
            self.send("type sleep 600\\n")
            wait_for(lambda: self.state()["tabs"][0]["program"] == "sleep", description="quiet PTY")
            # The header now shares row zero with the terminal. Remove this
            # fixture shell's prompt before exercising its controls; covered
            # controls correctly refuse clicks in the ordinary underlay.
            self.feed([""] * self.chrome()["rows"])
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
        # Native browser tabs have no child process. PID zero would signal
        # the fixture runner's entire process group during cleanup.
        self.pids.update(tab["pid"] for tab in result["tabs"] if tab["pid"] > 1)
        return result

    def chrome(self):
        return json.loads(self.output("cmd chrome").read_text())

    @staticmethod
    def tv(chrome, name):
        return next(item for item in chrome.get("tvs", []) if item["name"] == name)

    @staticmethod
    def settings_x(chrome):
        return chrome["pages"].get("settings_x", chrome["dock"]["x"])

    @staticmethod
    def header_point(chrome, name):
        control = next(item for item in chrome["header_controls"] if item["name"] == name)
        return chrome["dock"]["x"] + control["col"], chrome["dock"]["y"] + control["row"]

    def header_click(self, name):
        c = self.chrome()
        return self.click(*self.header_point(c, name))

    def open_settings(self):
        c = self.chrome()
        if not c["settings_open"]:
            c = self.header_click("settings")
        assert c["settings_open"], "settings header control did not open its panel"
        return c

    def settings_section(self, section):
        c = self.open_settings()
        if c["settings_tab"] != section:
            c = self.click(self.settings_x(c) + (20 if section else 3), c["pages"]["section_row"])
        assert c["settings_tab"] == section
        return c

    def toggle_settings(self):
        return self.header_click("settings")

    def quiet_tabs(self, count):
        """Size the primary menu using owned PTYs, independently of Settings."""
        existing = len(self.state()["tabs"])
        for _ in range(existing, count):
            self.send("cmd newtab")
            self.send("type sleep 600\\n")
        wait_for(lambda: (len((s := self.state())["tabs"]) >= count and
                         all(tab["program"] == "sleep" for tab in s["tabs"])),
                 description="owned quiet menu tabs")
        return self.chrome()

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
            assert len(line) <= dock["x"] - 2, (dock, line)
        else:
            assert len(line) - len(line.lstrip()) == dock["width"] + 4, (dock, line)
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
    # Meaningful terminal backgrounds and cursor rows protect prose. Mouse
    # reporting still permits safe reflow and uses mapped source coordinates.
    styled = lines.copy()
    styled[start] = "\x1b[48;2;22;33;44m" + styled[start] + "\x1b[0m"
    c = app.feed(styled)
    assert c["source"] == c["display"], "styled background paragraph reflowed content"
    c = app.feed(lines, "\x1b[?1000h\x1b[?1006h")
    assert c["reflow_changed"], "mouse-reporting mode disabled prose reflow"
    assert_mapping(app, c)
    c = app.feed(lines, f"\x1b[{start + 1};3H\x1b[?25h")
    assert c["source"] == c["display"], "cursor paragraph reflowed content"
    print("PASS: structure/backgrounds/cursor rows stay fixed; mouse-reporting prose reflows with source mapping")

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
    c = app.toggle_settings()
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
    origin = c["pages"].get("settings_x", c["dock"]["x"]) if kind == "settings" else c["dock"]["x"]
    return origin + local, page["row"]


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
    c = app.toggle_settings()
    assert c["settings_open"] and c["settings_tab"] == 0, c
    assert c["pages"]["settings"]["capacity"] == c["pages"]["settings"]["total"], "roomy font list unnecessarily paginated"
    c = app.click(app.settings_x(c) + 20, c["pages"]["section_row"])
    assert c["pages"]["settings"]["capacity"] == c["pages"]["settings"]["total"], "roomy theme list unnecessarily paginated"
    app.snap("sidebar-full-settings")
    c = app.click(app.settings_x(c) + 3, c["pages"]["section_row"])
    for expected in range(2, 12):
        app.send("cmd newtab")
        wait_for(lambda: len(app.state()["tabs"]) == expected, description="new test tab")
        app.send("type sleep 600\\n")
    # Settings has the opposite edge available independently of the tab list;
    # shrink the actual viewport enough that each separate list must paginate.
    app.send("cmd resize 1001,100")
    c = walk_pages(app, "settings")
    before_font = c["fontkey"]
    button = next(button for button in c["font_buttons"] if button["fontkey"] != before_font)
    c = app.click(app.settings_x(c) + button["left"], button["row"])
    assert c["fontkey"] == button["fontkey"], "font size button selected the wrong font"
    assert c["cell"] == {"width": button["width"], "height": button["height"]}, c
    font_offset = c["pages"]["settings"]["offset"]
    app.snap("sidebar-font-last-page")
    c = app.click(app.settings_x(c) + 20, c["pages"]["section_row"])
    assert c["settings_tab"] == 1, c
    c = walk_pages(app, "settings")
    before_scheme = c["schemekey"]
    c = app.click(app.settings_x(c) + 8, c["pages"]["settings"]["row"])
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

    # Reload preserves independent Font and Theme page positions.
    c = app.click(app.settings_x(c) + 3, c["pages"]["section_row"])
    font_offset = c["pages"]["settings"]["offset"]
    c = app.click(app.settings_x(c) + 20, c["pages"]["section_row"])
    theme_offset = c["pages"]["settings"]["offset"]
    c = app.click(app.settings_x(c) + 20, c["pages"]["section_row"])
    keys = ("mode", "settings_open", "settings_tab", "fontkey", "schemekey", "reflow_enabled", "dock", "pages")
    before = {key: c[key] for key in keys}
    result = subprocess.run([str(app.root / "hardcore.app/Contents/MacOS/hardcore"), "--reload"],
                            capture_output=True, text=True, timeout=40)
    assert result.returncode == 0, result.stdout + result.stderr
    assert app.state()["pid"] == app.proc.pid, "reload changed app PID"
    c = app.chrome()
    assert {key: c[key] for key in keys} == before, (before, c)
    c = app.click(app.settings_x(c) + 3, c["pages"]["section_row"])
    assert c["settings_tab"] == 0 and c["pages"]["settings"]["offset"] == font_offset, c
    c = app.click(app.settings_x(c) + 20, c["pages"]["section_row"])
    assert c["settings_tab"] == 1 and c["pages"]["settings"]["offset"] == theme_offset, c
    print("PASS: in-place reload preserves dock settings, independent Font/Theme pages and app PID")


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


def test_codex_records(app):
    """The two styled records from the reported screenshot, at its grid width."""
    c = app.chrome()
    app.send(f"cmd resize {120 * c['cell']['width'] // c['scale']},591")
    c = app.chrome()
    assert c["cols"] == 120, c
    # A tall primary menu places the output block in the obstruction band;
    # Settings is now a separately placed surface with its own lifetime.
    c = app.quiet_tabs(10)
    start, boundary = c["dock"]["y"] + 2, c["dock"]["y"] + 8
    assert boundary <= c["dock"]["y"] + c["dock"]["height"] < c["rows"], c
    path = "/Users/wm/Applications/hardcore.app"
    records = "/var/folders/k9/z44_w2td7p19nrwm8gztmcc80000gn/T/hc-update-verification-8rcflsus"

    def link(value):
        return f"\x1b]8;;file://{value}\x1b\\{value}\x1b]8;;\x1b\\"

    fixtures = {
        "command": [
            "\x1b[32m•\x1b[0m Ran \x1b[36mhardcore\x1b[0m \x1b[31m--version\x1b[0m && codesign --verify --deep --strict "
            + link(path) + " && git status --sho…",
            "\x1b[2m  └ ## main...origin/main\x1b[0m",
            "\x1b[2m    db1eefd120ebb6a3b3090ab988593954a9d37ae9\x1b[0m",
            "\x1b[2m    db1eefd120ebb6a3b3090ab988593954a9d37ae9\x1b[0m",
            "\x1b[2m    + 1 line (⌃t to expand)\x1b[0m",
        ],
        "verification": [
            "\x1b[32m•\x1b[0m Ran \x1b[36mpython3\x1b[0m - <<'PY' …",
            "\x1b[2m  └ Installed executable inode: 215350339\x1b[0m",
            "\x1b[2m    Verification records: " + link(records) + "\x1b[0m",
            "\x1b[2m    843:    g_mac_image = [mac_hash(@(g_mac_exe_path)) retain];\x1b[0m",
            "\x1b[2m    + 3 lines (⌃t to expand)\x1b[0m",
        ],
    }
    ansi = re.compile(r"\x1b\]8;[^\x1b]*\x1b\\|\x1b\[[0-?]*[ -/]*[@-~]")
    try:
        for name, block in fixtures.items():
            lines = [""] * c["rows"]
            lines[start:start + len(block)] = block
            lines[boundary] = "123"
            lines[-2] = "456"
            c = app.feed(lines, "\x1b[?1003h\x1b[?1006h")
            expected = [ansi.sub("", line) for line in lines]
            assert [line.rstrip() for line in c["source"]] == expected, name
            assert c["reflow_changed"], (name, "styled record did not reflow")
            # A path may split after a slash; no nonblank character may be lost,
            # repeated, or reordered, including either expansion annotation.
            assert "".join("".join(c["display"]).split()) == "".join("".join(c["source"]).split()), name
            footer = expected[start + len(block) - 1].strip()
            assert c["display"][boundary - 1].strip() == footer, (name, c["display"])
            assert c["display"][boundary].rstrip() == "123" and c["display"][-2].rstrip() == "456", name
            for row in range(start, boundary):
                line, dock = c["display"][row], c["dock"]
                gap = line[dock["x"] - 2:] if app.mode == 1 else line[:dock["x"] + dock["width"] + 2]
                assert not gap.strip(), (name, row, "missing two-cell menu gap", line)
            assert_mapping(app, c)
            app.hover(-1, -1)
            app.snap(f"mode-{app.mode}-codex-{name}-records")
        print(f"PASS: dock {app.mode} reflows styled Codex commands and paths with two-cell padding, retaining footers, boundaries and source coordinates")
    finally:
        c = app.chrome()
        while len(app.state()["tabs"]) > 1:
            app.send("cmd closetab")
            app.state()
        app.send("cmd resize 1001,591")


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
    # Rounded exposed corners intentionally reveal terminal background. The
    # entire remaining menu interior must still be opaque and unchanged.
    radius = min(6, cell["width"]//2, cell["height"]//3)
    interior_box = (box[0]+radius, box[1], box[2]-radius, box[3])
    assert shaded.crop(*interior_box) == dim.crop(*interior_box), "clear menu row is not fully opaque over highlighted blank space"
    interior = shaded.crop(box[0] + cell["width"], box[1] + 2, box[2] - 3*cell["width"], box[3] - 4)
    assert len({interior[i:i+3] for i in range(0, len(interior), 4)}) > 1, "highlighted blank cells still hide the menu text"
    # Terminal text owns its whole cell even over the colored background.
    lines[row] = "\x1b[48;2;60;70;80m" + "X" * c["cols"] + "\x1b[0m"
    app.feed(lines)
    occupied = app.snap("codex-highlighted-text-over-menu")
    assert occupied.crop((d["x"] + 8)*cell["width"], box[1], cell["width"], cell["height"]) == occupied.crop(40*cell["width"], box[1], cell["width"], cell["height"]), "underlay draws over highlighted terminal text"

    c = app.feed([""] * c["rows"])
    c = app.toggle_settings()
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
    c = app.toggle_settings()
    app.hover(-1, -1)
    print("PASS: invisible styled spaces reveal chrome; borders collapse to one pixel, settings omit row rules, right edge is open")


def test_native_titlebar(app):
    c=app.feed(['X'*70]*4)
    c=app.hover(-1,-1)
    title=c['titlebar'];cw,ch=c['cell']['width'],c['cell']['height']
    assert title['height']>0 and title['buttons_visible'] and not title['title_hidden'] and title['proxy_visible'],title
    assert not title['full_size_content'] and not title['transparent'] and not title['hovered'],title
    for button in title['button_rects']:
        assert button['width']>0 and button['height']>0 and button['y']+button['height']<=0,button
    before=app.snap('native-titlebar-content-before-hover')
    app.hover(40,0)
    c=app.chrome();after=app.snap('native-titlebar-content-after-hover')
    assert c['titlebar']['buttons_visible'] and not c['titlebar']['hovered']
    assert before.crop(30*cw,0,40*cw,4*ch)==after.crop(30*cw,0,40*cw,4*ch),'top hover changed terminal pixels'
    app.feed(['']*c['rows'],'history line\r\n'*(c['rows']*2))
    x=40*cw
    app.send(f'cmd pointer down {x},{3*ch}')
    app.send(f'cmd pointer drag {x},2')
    c=app.chrome()
    assert c['titlebar']['selection_drag'] and c['titlebar']['buttons_visible']
    app.send(f'cmd pointer drag {x},-5')
    assert app.state()['tabs'][0]['view']>0
    app.send(f'cmd pointer up {x},-5')
    assert app.chrome()['titlebar']['buttons_visible']
    app.hover(-1,-1)
    print('PASS: native folder title bar and visible traffic lights outside content; no hover painting; upward selection works')


def test_drag_and_quiet_tabs(app):
    c = app.chrome()
    if c["settings_open"]:
        c = app.toggle_settings()
    app.send("cmd newtab")
    wait_for(lambda: len(app.state()["tabs"]) == 2, description="second drag-test tab")
    app.send("type sleep 600\\n")
    wait_for(lambda: app.state()["tabs"][1]["program"] == "sleep", description="second quiet PTY")
    c = app.feed([""] * app.chrome()["rows"])
    cols = c["cols"]
    for side, target_col, target_row in ((2, 1, 5), (1, cols - 2, 0)):
        cw, ch = c["cell"]["width"], c["cell"]["height"]
        handle_x = int((c["dock"]["x"] + 1.5) * cw)
        handle_y = int((c["dock"]["y"] + .5) * ch)
        target_x, target_y = int((target_col + .5) * cw), int((target_row + .5) * ch)
        app.send(f"cmd pointer down {handle_x},{handle_y}")
        app.send(f"cmd pointer drag {target_x},{target_y}")
        app.send(f"cmd pointer up {target_x},{target_y}")
        c = app.chrome()
        assert c["mode"] == side and c["dock"]["y"] == target_row and c["cols"] == cols, c
        assert not c["reflow_markers"] and c["reflow_enabled"], c
        app.snap(f"drag-dock-{side}-row-{target_row}")
    config = (app.root / "config/config.ini").read_text()
    assert "tab_underlay=1" in config and "dock_row=0" in config, config
    assert "chrome_reflow=" not in config and "chrome_debug=" not in config, config

    row = c["pages"]["tabs"]["row"]
    app.hover(-1, -1)
    quiet = app.snap("tabs-without-close-or-separators")
    close_x = (c["dock"]["x"] + c["dock"]["width"] - 2) * cw
    close_box = (close_x, row * ch, cw, ch)
    pixels = quiet.crop(*close_box)
    # First tab has a top section rule, so inspect the glyph interior only.
    interior = quiet.crop(close_x, row * ch + 2, cw, ch - 4)
    assert len({interior[i:i+3] for i in range(0, len(interior), 4)}) == 1, "close glyph visible without hover"
    boundary = (row + 1) * ch
    x = (c["dock"]["x"] + c["dock"]["width"]) * cw - 2
    assert pixel(quiet, x, boundary) == pixel(quiet, x, boundary + 1), "tab separator remains"
    c = app.hover(c["dock"]["x"] + 6, row)
    assert not c["titlebar"]["hovered"], "top titlebar obscures hovered tab"
    pointed = app.snap("hovered-tab-close")
    assert pointed.crop(*close_box) != pixels, "close glyph did not appear"
    x, y = int((c["dock"]["x"] + 6.5) * cw), int((row + .5) * ch)
    app.send(f"cmd pointer down {x},{y}")
    app.send(f"cmd pointer up {x},{y}")
    assert app.state()["active"] == 0, "titlebar intercepted tab selection"
    print("PASS: native handle snaps to both edges and row zero; tabs hide close buttons and separators; top tab remains clickable")


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
                assert c["mode"] == app.mode and c["reflow_enabled"] and not c["reflow_markers"], c
                columns[mode] = c["cols"]
                if mode:
                    test_reflow(app)
                    test_codex_prose(app)
                    test_codex_records(app)
                    if mode == 1:
                        test_visible_whitespace_and_rules(app)
                        test_native_titlebar(app)
                    test_hover(app)
                    if mode == 1:
                        test_drag_and_quiet_tabs(app)
                else:
                    app.snap("sidebar")
                    test_pagination(app)
        assert columns[1] == columns[2] == columns[0], columns
        print("PASS: both underlays and migrated sidebar settings use the same full terminal width")
    print(f"Native chrome screenshots: {args.artifacts}")


if __name__ == "__main__":
    main()
