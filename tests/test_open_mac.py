#!/usr/bin/env python3
"""Cmd+hover and Cmd+click on file paths in an isolated native app: the hint
names what will open (hardcore split for a registered type, the default app
otherwise, Option swapping them), a click opens hcmd in a split beside the
tab with the other tabs hidden, Cmd+\\ toggles split and single tab, and
quitting hcmd returns to the single tab. Snapshots go to build/open-native.
Never clicks something that would open another app."""
from pathlib import Path
import tempfile
from test_chrome_mac import NativeApp, ROOT, wait_for

CMD, OPT = 0x100000, 0x80000


def locate(c, text):
    for row, line in enumerate(c["display"]):
        col = line.find(text)
        if col >= 0:
            return col, row
    raise AssertionError(f"{text!r} not on screen: {c['display'][:8]}")


def point(c, col, row):
    return int((col + .5) * c["cell"]["width"]), int((row + .5) * c["cell"]["height"])


def run():
    artifacts = ROOT / "build/open-native"
    with tempfile.TemporaryDirectory(prefix="hardcore-open-") as tmp:
        root = Path(tmp)
        (root / "docs").mkdir()
        (root / "docs/guide.md").write_text("# Guide\n\nOpened by **Cmd+click**.\n\n| a | b |\n|---|---|\n| 1 | 2 |\n")
        (root / "src").mkdir()
        (root / "src/a.c").write_text("".join(f"int line{i};\n" for i in range(1, 60)))
        (root / "report.pdf").write_bytes(b"%PDF-1.4\n")
        with NativeApp(root, 2, artifacts) as app:
            app.quiet_tabs(3)
            app.send("cmd tab 0")
            ids = [t["id"] for t in app.state()["tabs"]]
            c = app.feed(["", "", "", "", "build: see docs/guide.md and src/a.c:42: error", "attached report.pdf here",
                          "nothing/here.md stays plain"])
            col, row = locate(c, "docs/guide.md")
            x, y = point(c, col + 3, row)
            app.send(f"cmd pointer flags {x},{y} {CMD:x}")
            h = app.chrome()["open_hover"]
            assert h["on"] and h["path"].endswith("/docs/guide.md"), h
            assert h["c1"] - h["c0"] == len("docs/guide.md"), h
            assert "open in hardcore split" in h["hint"][1] and h["hint"][1].startswith("⌘-click"), h["hint"]
            print("PASS hover on docs/guide.md:", " | ".join(h["hint"]))
            app.snap("hover-hint")
            app.send(f"cmd pointer flags {x},{y} {CMD | OPT:x}")
            h = app.chrome()["open_hover"]
            assert h["hint"][1].startswith("⌥⌘-click") and "hardcore" not in h["hint"][1], h["hint"]
            print("PASS Option swaps the hint:", h["hint"][1])
            col, row = locate(c, "src/a.c:42")
            app.send("cmd pointer flags {},{} {:x}".format(*point(c, col + 1, row), CMD))
            h = app.chrome()["open_hover"]
            assert h["path"].endswith("/src/a.c") and "a.c:42" in h["hint"][0] and h["c1"] - h["c0"] == len("src/a.c:42:"), h
            print("PASS path:line hover:", h["hint"][0])
            col, row = locate(c, "report.pdf")
            app.send("cmd pointer flags {},{} {:x}".format(*point(c, col + 2, row), CMD))
            h = app.chrome()["open_hover"]
            assert h["on"] and "hardcore" not in h["hint"][1] and ("open in" in h["hint"][1] or "Finder" in h["hint"][1]), h
            print("PASS unregistered type goes to the default app:", h["hint"][1])
            app.snap("hover-default-app")
            col, row = locate(c, "nothing/here.md")
            app.send("cmd pointer flags {},{} {:x}".format(*point(c, col + 2, row), CMD))
            assert not app.chrome()["open_hover"]["on"], "a path that does not exist shows no hint"
            app.send("cmd pointer flags {},{} 0".format(*point(c, col + 2, row)))
            print("PASS no hint for a missing path, or without Cmd")

            # Cmd+click: hcmd in a split beside tab 0, tabs 1 and 2 hidden but alive
            col, row = locate(c, "docs/guide.md")
            x, y = point(c, col + 3, row)
            app.send(f"cmd pointer down {x},{y} {CMD:x}")
            app.send(f"cmd pointer up {x},{y} {CMD:x}")
            c = wait_for(lambda: (s := app.chrome())["grid"]["enabled"] and len(s["grid"]["panes"]) == 2 and s,
                         description="split view with two panes")
            s = app.state()
            opened = [t for t in s["tabs"] if t["id"] not in ids]
            assert len(s["tabs"]) == 4 and len(opened) == 1, s["tabs"]
            panes = [p["id"] for p in c["grid"]["panes"]]
            assert panes == [ids[0], opened[0]["id"]], (panes, ids, opened)
            assert c["split"]["main"] == ids[0] and c["split"]["other"] == opened[0]["id"], c["split"]
            assert c["grid"]["cols"] == 2 and c["grid"]["rows"] == 1, c["grid"]
            wait_for(lambda: any("Opened by" in line for line in app.chrome()["display"]) or
                     any("Opened by" in "".join(p.get("browser") or []) for p in app.chrome()["grid"]["panes"]) or
                     app.state()["tabs"][-1]["program"] == "hcmd", description="hcmd running in the new pane")
            print("PASS Cmd+click opened hcmd in a split; other tabs hidden:", c["grid"]["panes"])
            app.snap("split")

            app.send("cmd split")
            c = app.chrome()
            assert not c["grid"]["enabled"] and c["split"]["main"] == 0 and app.state()["active"] == 0, c["split"]
            app.send("cmd split")
            c = app.chrome()
            assert c["grid"]["enabled"] and [p["id"] for p in c["grid"]["panes"]] == panes, c["grid"]["panes"]
            print("PASS Cmd+\\ toggles single tab and split")
            app.send("cmd split-grid")
            c = app.chrome()
            assert c["grid"]["enabled"] and len(c["grid"]["panes"]) == 4 and c["split"]["main"] == 0, c["grid"]["panes"]
            print("PASS split to full grid shows every tab")
            app.send("cmd grid")
            assert not app.chrome()["grid"]["enabled"]
            app.send("cmd split")
            assert app.chrome()["split"]["main"] == ids[0]

            # quitting hcmd closes its tab: back to the single main tab
            app.send("cmd tab 3")
            app.send("type q")
            wait_for(lambda: len(app.state()["tabs"]) == 3, description="hcmd tab closed")
            c = app.chrome()
            assert not c["grid"]["enabled"] and c["split"]["main"] == 0 and app.state()["active"] == 0, (c["grid"], c["split"])
            print("PASS closing the opened tab returns to the single main tab")
    print("ok open-native")


if __name__ == "__main__":
    run()
