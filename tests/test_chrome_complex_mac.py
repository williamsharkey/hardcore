#!/usr/bin/env python3
"""Native fixtures for status labels, compact diffs, tables and shaded inputs.

Uses only a disposable Hardcore process and its explicit test FIFO. Every
fixture positions rows itself, and checks the original terminal grid, source
mapping, and local placement. No input goes to a user's terminal session.
"""
import argparse
from pathlib import Path
import re
import tempfile
import textwrap

from test_chrome_mac import NativeApp, assert_mapping, pixel


def arrange(app, side, top=5):
    app.send("cmd resize 1001,700")
    # The primary surface is now independent of Settings. Use real owned
    # tabs to retain the original tall obstruction band for these fixtures.
    c = app.quiet_tabs(12)
    c = app.feed([""] * c["rows"])
    cw, ch = c["cell"]["width"], c["cell"]["height"]
    x = int((c["dock"]["x"] + 1.5) * cw)
    y = int((c["dock"]["y"] + .5) * ch)
    target_x = int((1.5 if side == 2 else c["cols"] - 1.5) * cw)
    target_y = int((top + .5) * ch)
    app.send(f"cmd pointer down {x},{y}")
    app.send(f"cmd pointer drag {target_x},{target_y}")
    app.send(f"cmd pointer up {target_x},{target_y}")
    app.hover(-1, -1)
    c = app.chrome()
    assert c["mode"] == side and c["dock"]["y"] == top, c["dock"]
    return c


def characters(lines):
    return "".join(char for line in lines for char in line if not char.isspace())


def source_grid(lines, cols):
    """Expected source grid for these one-cell fixtures, including softwrap."""
    grid = [[" "] * cols for _ in lines]
    for row, text in enumerate(lines):
        text = re.sub(r"\x1b\[[0-9;]*m", "", text)
        for offset, char in enumerate(text):
            y, x = row + offset // cols, offset % cols
            assert y < len(grid), "fixture would scroll the terminal"
            grid[y][x] = char
    return ["".join(row) for row in grid]


def conserved(app, c, source, cursor_before=None):
    assert c["source"] == source, "display layout changed original terminal rows"
    assert characters(c["display"]) == characters(source), "display dropped, duplicated or reordered text"
    assert_mapping(app, c)
    assert app.chrome()["source"] == source, "mapping queries changed original terminal rows"
    if cursor_before is not None:
        state = app.state()
        t = state["tabs"][state["active"]]
        assert [t["cx"], t["cy"]] == cursor_before, "layout changed PTY cursor coordinates"


def clear_lane(c, first, last, padding=2):
    d = c["dock"]
    for row in range(max(first, d["y"]), min(last, d["y"] + d["height"])):
        line = c["display"][row]
        cut = line[d["x"] - padding:] if c["mode"] == 1 else line[:d["width"] + padding]
        assert not cut.strip(), ("text still intrudes", row, line, d)


def test_status_labels(app, side):
    c = arrange(app, side)
    if side == 1:
        # This fixture targets the reported left-side short-label failure.
        return
    first = c["dock"]["y"] + 1
    statuses = ["• Started `/root/parse_state`", "• Completed `/root/native_probe`",
                "• Interacted with `/root/test_runner`"]
    lines = [""] * c["rows"]
    lines[first - 2] = "UNCHANGED-UPPER"
    lines[first + 7] = "UNCHANGED-LOWER"
    for i, label in enumerate(statuses):
        lines[first + 2*i] = "\x1b[38;2;110;160;220m" + label + "\x1b[0m"
    c = app.feed(lines)
    source = source_grid(lines, c["cols"])
    assert c["reflow_changed"] >= 3, "short status records stayed protected"
    clear_lane(c, first, first + 6)
    for i, label in enumerate(statuses):
        assert c["display"][first + 2*i].strip() == label, "short status changed its row or wording"
    assert c["display"][first - 2] == source[first - 2]
    assert c["display"][first + 7] == source[first + 7]
    conserved(app, c, source)
    app.snap("left-status-labels")
    print("PASS: short Started/Completed/Interacted records shift intact beside left menu")


def test_compact_diff(app, side):
    c = arrange(app, side)
    first = c["dock"]["y"] + 2
    lines = [""] * c["rows"]
    lines[first - 1] = "UNCHANGED-UPPER"
    lines[first + 6] = "UNCHANGED-LOWER"
    body = ('const char *name = "alpha"; if (ready) update(name); ' * 6)[:c["cols"] - 13].rsplit(" ", 1)[0]
    for i in range(3):
        row = f"  {i + 3} +{body}"
        assert len(row) < c["cols"]
        lines[first + i] = "\x1b[48;2;40;70;50m" + row.ljust(c["cols"]) + "\x1b[0m"
    c = app.feed(lines)
    source = source_grid(lines, c["cols"])
    assert c["reflow_changed"], "three-row compact diff did not reflow"
    clear_lane(c, first, first + 6)
    assert c["display"][first - 1] == source[first - 1]
    assert c["display"][first + 6] == source[first + 6]
    for number in range(3, 6):
        assert sum(f"{number} +const" in row for row in c["display"]) == 1, "diff gutter duplicated or lost"
    conserved(app, c, source)
    image = app.snap(f"side-{side}-compact-three-row-diff")
    # Every continuation retains the original green row style, not merely
    # the foreground glyphs. Its hanging indentation supplies clear pixels.
    cw, ch = c["cell"]["width"], c["cell"]["height"]
    for y in range(first, first + 6):
        text = c["display"][y]
        if not text.strip():
            continue
        start = len(text) - len(text.lstrip())
        assert start > (c["dock"]["width"] + 2 if side == 2 else 0)
        assert pixel(image, (start - 1)*cw + cw//2, y*ch + ch//2) == bytes((50, 70, 40, 255)), (y, text)
    print(f"PASS: side {side} compact diff retains three identities, styles and bounded blank-row borrowing")


def test_process_table(app, side):
    c = arrange(app, side)
    first = c["dock"]["y"] + 2
    lines = [""] * c["rows"]
    lines[first - 1] = "UNCHANGED-UPPER"
    lines[first + 6] = "UNCHANGED-LOWER"
    command_col = 65
    prefix = "   PID   PPID   PGID  TPGID TTY       STARTED".ljust(command_col)
    lines[first] = prefix + "COMMAND"
    command = "/Applications/hardcore.app/Contents/MacOS/hardcore --terminal-bridge /dev/ttys011 67202"
    command = command[:c["cols"] - command_col - 1].rsplit(" ", 1)[0]
    lines[first + 1] = " 27140  51978  27140  27140 ttys020   Fri Oct 16 09:10:23 2026".ljust(command_col) + command
    lines[first + 2] = " 27146  51978  27146  27146 ttys021   Fri Oct 16 09:10:24 2026".ljust(command_col) + command
    assert all(len(row) < c["cols"] for row in lines), (command_col, c["cols"])
    c = app.feed(lines)
    source = source_grid(lines, c["cols"])
    assert c["reflow_changed"], "recognized process COMMAND table remained protected"
    clear_lane(c, first, first + 6)
    assert c["display"][first - 1] == source[first - 1]
    assert c["display"][first + 6] == source[first + 6]
    for pid in ("27140", "27146"):
        assert sum(row.count(pid) for row in c["display"]) == sum(row.count(pid) for row in source)
    conserved(app, c, source)
    app.snap(f"side-{side}-process-command-column")
    print(f"PASS: side {side} process table retains metadata and command text without moving unrelated rows")


def test_hyphen_softwrap(app, side):
    c = arrange(app, side)
    first = c["dock"]["y"] + 1
    # Generate an actual VT softwrap, rather than a hard newline that merely
    # looks similar. The hyphen is the final cell of the first physical row.
    lead = "The terminal supports "
    filler = "ordinary text " * c["cols"]
    prefix = (lead + filler)[:c["cols"] - 5]
    if prefix.endswith(" "):
        prefix = prefix[:-1] + "x"
    prefix += " 256-"
    assert len(prefix) == c["cols"]
    text = prefix + "color output with unchanged character order and no invented word separator."
    lines = [""] * c["rows"]
    lines[first] = text
    lines[first + 5] = "UNCHANGED-LOWER"
    c = app.feed(lines)
    source = source_grid(lines, c["cols"])
    assert source[first].endswith("256-") and source[first + 1].startswith("color"), source[first:first + 2]
    assert c["reflow_changed"], "safe physical hyphen softwrap stayed protected"
    clear_lane(c, first, first + 5)
    assert c["display"][first + 5] == source[first + 5]
    conserved(app, c, source)
    # Source-address adjacency distinguishes a visual break inside the token
    # from an invented space. Check the two boundary characters explicitly.
    before = next((x, y) for y, row in enumerate(c["display"]) for x in range(len(row)-3)
                  if row[x:x+4] == "256-")
    boundary = app.map(before[0] + 3, before[1])
    assert boundary == {"col": c["cols"] - 1, "row": first}, boundary
    app.snap(f"side-{side}-hyphen-softwrap")
    print(f"PASS: side {side} actual 256-/color softwrap reflows without changing the token")


def test_shaded_panel(app, side, cursor_kind="word"):
    c = arrange(app, side)
    first, height = c["dock"]["y"] + 2, 6
    lines = [""] * c["rows"]
    toolbar = "↓ Back to bottom · esc"
    lines[first - 1] = " " * ((c["cols"] - len(toolbar)) // 2) + "\x1b[48;2;55;55;50m" + toolbar + "\x1b[0m"
    message = ("Keep this unsent request and every word exactly as entered while making the menu readable. "
               "A shaded input panel has its own blank padding and must keep the cursor attached to its original character. "
               "Continue the paragraph without moving the toolbar or the surrounding output.")
    wrapped = textwrap.wrap(message, width=c["cols"] - 2, initial_indent="❯ ", subsequent_indent="  ")
    assert len(wrapped) <= height - 2
    for y in range(first, first + height):
        text = wrapped[y-first-1] if 1 <= y-first <= len(wrapped) else ""
        lines[y] = "\x1b[48;2;55;55;50m" + text.ljust(c["cols"]) + "\x1b[0m"
    lines[first + height] = "UNCHANGED-LOWER"
    cursor_row = first + 1
    cursor_col = wrapped[0].index("unsent") + 3
    if side == 1:
        cursor_col = len(wrapped[0]) - 3
        while wrapped[0][cursor_col].isspace():
            cursor_col -= 1
        assert cursor_col >= c["dock"]["x"]
    if cursor_kind == "blank":
        cursor_row = first + len(wrapped)
        cursor_col = len(wrapped[-1]) + 1
        assert cursor_col < c["cols"]
    suffix = "" if cursor_kind == "historical" else f"\x1b[{cursor_row+1};{cursor_col+1}H\x1b[?25h"
    c = app.feed(lines, suffix)
    source = source_grid(lines, c["cols"])
    assert c["reflow_changed"], "shaded live input panel stayed protected"
    clear_lane(c, first, first + height)
    assert c["display"][first - 1] == source[first - 1], "toolbar moved with input panel"
    assert c["display"][first + height] == source[first + height]
    conserved(app, c, source, None if cursor_kind == "historical" else [cursor_col, cursor_row])
    if cursor_kind != "historical":
        assert c["cursor"]["source"] == [cursor_col, cursor_row] and c["cursor"]["visible"], c["cursor"]
        displayed = c["cursor"]["display"]
        assert app.map(*displayed) == {"col": cursor_col, "row": cursor_row}, c["cursor"]
    shown = app.snap(f"side-{side}-shaded-panel-{cursor_kind}")
    if cursor_kind == "word":
        # Inspect actual Metal cursor pixels too: truthful telemetry alone
        # does not prove the cursor draw call uses its relocated coordinates.
        hidden = app.feed(lines, f"\x1b[{cursor_row+1};{cursor_col+1}H\x1b[?25l")
        assert hidden["display"] == c["display"], "hiding a middle-word cursor changed panel layout"
        without_cursor = app.snap(f"side-{side}-shaded-panel-cursor-hidden")
        cw, ch = c["cell"]["width"], c["cell"]["height"]
        box = (displayed[0]*cw, displayed[1]*ch, cw, ch)
        assert shown.crop(*box) != without_cursor.crop(*box), "cursor was not painted at its mapped display cell"
    print(f"PASS: side {side} shaded {cursor_kind} panel reflows inside its own padding with stable source/cursor")


CASES = {"status": test_status_labels, "diff": test_compact_diff, "process": test_process_table,
         "softwrap": test_hyphen_softwrap, "panel": test_shaded_panel,
         "panel-blank": lambda app, side: test_shaded_panel(app, side, "blank"),
         "panel-history": lambda app, side: test_shaded_panel(app, side, "historical")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, default=Path("build/chrome-complex"))
    parser.add_argument("--case", choices=CASES)
    parser.add_argument("--side", type=int, choices=(1, 2))
    args = parser.parse_args()
    artifacts = args.artifacts.resolve()
    failures = []
    with tempfile.TemporaryDirectory(prefix="hc-chrome-complex-") as tmp:
        for side in ([args.side] if args.side else [1, 2]):
            for name, case in CASES.items():
                if args.case and name != args.case:
                    continue
                with NativeApp(Path(tmp) / f"{side}-{name}", side, artifacts) as app:
                    try:
                        case(app, side)
                    except AssertionError as exc:
                        failures.append((side, name, str(exc)))
                        app.snap(f"FAIL-side-{side}-{name}")
                        print(f"FAIL: side {side} {name}: {exc}")
    if failures:
        raise SystemExit(f"FAILED: {len(failures)} complex native fixtures; artifacts: {artifacts}")
    print(f"PASS: complex native fixtures; screenshots: {artifacts}")


if __name__ == "__main__":
    main()
