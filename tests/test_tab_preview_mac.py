#!/usr/bin/env python3
"""Native hover preview paints another owned tab without changing input focus.

Synthetic events target only an isolated app's test FIFO, never the focused
user application. Focus is simulated through the fixture's inspection channel.
"""
import argparse
from pathlib import Path
import tempfile

from test_chrome_mac import NativeApp, wait_for
from test_tab_preview_extended_mac import test_workers, test_lifecycle, test_browsers


def test_preview(app):
    c = app.quiet_tabs(2)
    app.send("cmd focus 1")
    lines = [""] * c["rows"]
    lines[10] = "\x1b[48;2;90;30;20m" + "SECOND TAB PREVIEW ".ljust(c["cols"]) + "\x1b[0m"
    second = app.feed(lines)
    second_id = second["preview"]["active_id"]
    second_image = app.snap("preview-second-baseline")
    app.send("cmd tab 0")
    lines[10] = "\x1b[48;2;20;70;35m" + "ORIGINAL INPUT TAB ".ljust(c["cols"]) + "\x1b[0m"
    first = app.feed(lines)
    first_id = first["preview"]["active_id"]
    first_image = app.snap("preview-original-baseline")
    cw, ch = first["cell"]["width"], first["cell"]["height"]
    box = (40*cw, 10*ch, 10*cw, ch)
    assert first_image.crop(*box) != second_image.crop(*box)
    dock = first["dock"]
    tab_row = first["pages"]["tabs"]["row"] + 1
    c = app.hover(dock["x"] + 7, tab_row)
    assert c["preview"] == {"active_id": first_id, "display_id": second_id, "preview_id": second_id}, c["preview"]
    assert app.state()["active"] == 0, "hover switched the keyboard's selected tab"
    assert c["dock"] == dock, "hover preview moved menu geometry"
    preview = app.snap("second-tab-hover-preview")
    assert preview.crop(*box) == second_image.crop(*box), "preview metadata changed but Metal still drew the original tab"
    c = app.hover(-1, -1)
    assert c["preview"] == {"active_id": first_id, "display_id": first_id, "preview_id": 0}
    restored = app.snap("preview-returned-original")
    assert restored.crop(*box) == first_image.crop(*box)

    app.hover(dock["x"] + 7, tab_row)
    app.send("key 0 0 x x")
    c = app.chrome()
    assert c["preview"]["preview_id"] == 0 and app.state()["active"] == 0, "keyboard input failed to cancel the display preview"
    assert c["preview"]["display_id"] == first_id
    # The owned sleep process leaves normal PTY echo enabled. Verify the
    # actual typed byte reached the selected PTY, not just its active index.
    wait_for(lambda: app.chrome()["source"][-1].startswith("x"), description="keypress echoed in selected PTY")
    app.send("cmd tab 1")
    assert not app.chrome()["source"][-1].strip(), "keypress leaked into the previewed PTY"
    app.send("cmd tab 0")
    app.hover(dock["x"] + 7, tab_row)
    app.send("cmd focus 0")
    c = app.chrome()
    assert c["preview"]["preview_id"] == 0 and c["preview"]["display_id"] == first_id, "focus loss retained a preview"
    c = app.hover(dock["x"] + 7, tab_row)
    assert not c["preview"]["preview_id"], "unfocused window started a hover preview"

    app.send("cmd focus 1")
    app.hover(dock["x"] + 7, tab_row)
    x, y = int((dock["x"] + 7.5)*cw), int((tab_row+.5)*ch)
    app.send(f"cmd pointer down {x},{y}")
    app.send(f"cmd pointer up {x},{y}")
    c = app.chrome()
    assert app.state()["active"] == 1, "clicking the previewed tab did not activate it"
    assert c["preview"] == {"active_id": second_id, "display_id": second_id, "preview_id": 0}
    # Preview the original, then resize. That geometry-changing operation must
    # restore the selected tab rather than leave a stale hover target active.
    app.hover(dock["x"] + 7, first["pages"]["tabs"]["row"])
    app.send("cmd resize 1003,593")
    c = app.chrome()
    assert c["preview"]["preview_id"] == 0 and c["preview"]["display_id"] == second_id
    print("PASS: real Metal hover preview preserves selected/input tab, restores on leave/key/focus/resize, and activates only on click")


def main():
    cases = {"pixels": test_preview, "workers": test_workers, "lifecycle": test_lifecycle, "browsers": test_browsers}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=cases)
    parser.add_argument("--artifacts", type=Path, default=Path("build/tab-preview-native"))
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="hc-tab-preview-") as directory:
        for name, check in cases.items():
            if args.case and name != args.case:
                continue
            with NativeApp(Path(directory)/name, 1, args.artifacts.resolve()) as app:
                try:
                    check(app)
                except AssertionError as error:
                    failures.append(name)
                    app.snap(name+"-failure")
                    print(f"FAIL: {name}: {error}", flush=True)
    if failures:
        raise SystemExit("FAILED: "+", ".join(failures))
    print(f"PASS: native tab preview; screenshots: {args.artifacts.resolve()}")


if __name__ == "__main__":
    main()
