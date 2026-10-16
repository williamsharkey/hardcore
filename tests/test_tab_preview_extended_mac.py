#!/usr/bin/env python3
"""Owned PTY/background unread and browser coverage for native hover preview.

Workers independently log SIGWINCH, tty size, and input. Browser fixtures load
only a local HTML file. All commands/events target a disposable Hardcore app.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

from test_chrome_mac import NativeApp, wait_for

WORKER = r'''
import fcntl,json,os,select,signal,struct,sys,termios,tty
from pathlib import Path
root=Path(sys.argv[1]);label=sys.argv[2];color=sys.argv[3]
winsizes=[];received=bytearray();emitted=0
def size():return list(struct.unpack("HHHH",fcntl.ioctl(0,termios.TIOCGWINSZ,bytes(8)))[:2])
def winch(signum,frame):winsizes.append(size())
signal.signal(signal.SIGWINCH,winch)
tty.setraw(0)
def record():
    temp=root.with_suffix(".tmp")
    temp.write_text(json.dumps(dict(pid=os.getpid(),winch=winsizes,size=size(),input=received.hex(),emitted=emitted)))
    temp.replace(root.with_suffix(".json"))
record()
while True:
    if not emitted and root.with_suffix(".emit").exists():
        rows,cols=size();out="\x1bc\x1b[?25l\x1b[2J"
        for row in range(rows):
            text=(label+" "+str(row)+" ").ljust(cols,"X")[:cols]
            out+=f"\x1b[{row+1};1H\x1b[48;2;{color}m"+text
        os.write(1,(out+"\x1b[0m\x1b[1;1H\a").encode());emitted=1
    if select.select([0],[],[],.02)[0]:
        data=os.read(0,4096)
        if not data:break
        received.extend(data)
    record()
'''


def tab(state, identity):
    return next(item for item in state["tabs"] if item["id"] == identity)


def selected(state):
    return state["tabs"][state["active"]]["id"]


def select_tab(app, identity):
    index = next(i for i, item in enumerate(app.state()["tabs"]) if item["id"] == identity)
    app.send(f"cmd tab {index}")
    return app.chrome()


def hover_tab(app, identity):
    state, c = app.state(), app.chrome()
    index = next(i for i, item in enumerate(state["tabs"]) if item["id"] == identity)
    y = c["tab_rows"][index]
    assert y >= 0, "fixture tab must be visible"
    x = int((c["dock"]["x"] + 8.5) * c["cell"]["width"])
    app.send(f"cmd hover {x},{int(y + c['cell']['height']*.5)}")
    return app.chrome()


def shown(c, active, display):
    assert c["preview"] == {"active_id": active, "display_id": display,
                            "preview_id": display if display != active else 0}, c["preview"]


def geometry(c):
    return c["dock"], c["tab_rows"], c["pages"]["tabs"], c["cell"]


def raised(c):
    assert c["hovered"] and c["motion"]["alpha"] == 1, "preview menu lost pointer priority or opacity"
    for row in c["menu_rows"]:
        assert row["map"] == list(range(c["dock"]["width"])), ("preview menu retained cutouts", row)


def worker_data(path):
    try:
        return json.loads(path.with_suffix(".json").read_text())
    except FileNotFoundError:
        return None


def setup_workers(app):
    app.send("cmd resize 1001,700")
    app.send("cmd focus 1")
    active = selected(app.state())
    c = app.chrome()
    source = app.feed(["SELECTED-TAB-KEEPS-INPUT"] + [""]*(c["rows"]-1))["source"]
    script = app.root / "preview_worker.py"
    script.write_text(WORKER)
    workers = []
    for index, color in enumerate(("120;20;20", "20;40;140"), 1):
        app.send("cmd newtab")
        identity = selected(app.state())
        path = app.root / f"worker-{index}"
        command = " ".join(shlex.quote(value) for value in (sys.executable, "-u", str(script), str(path), f"PREVIEW-{index}", color))
        app.send("type " + command + r"\n")
        ready = wait_for(lambda: worker_data(path), description=f"owned preview worker {index}")
        app.pids.add(ready["pid"])
        workers.append((identity, path))
    select_tab(app, active)
    app.hover(-1, -1)
    for identity, path in workers:
        path.with_suffix(".emit").touch()
    wait_for(lambda: all(worker_data(path)["emitted"] for _, path in workers), description="background worker output")
    def unread_ready():
        state = app.state()
        return state if all(tab(state, identity)["activity"] and tab(state, identity)["attention"] for identity, _ in workers) else None
    state = wait_for(unread_ready, description="background unread and bell markers")
    assert selected(state) == active and app.chrome()["source"] == source
    return active, source, workers, state


def test_workers(app):
    active, source, workers, state = setup_workers(app)
    focus = state["first_responder"], state["key_window"]
    fixed = geometry(app.chrome())
    before = {identity: (tab(state, identity), worker_data(path)) for identity, path in workers}
    sources, headers = {}, []
    for round_number in range(3):
        for identity, path in workers + workers[::-1]:
            c = hover_tab(app, identity)
            shown(c, active, identity)
            assert geometry(c) == fixed, "preview moved or repaginated menu"
            raised(c)
            if identity in sources:
                assert c["source"] == sources[identity], "preview changed original terminal cells"
            else:
                sources[identity] = c["source"]
                assert any("PREVIEW-" in line for line in c["source"])
            if round_number == 0:
                image = app.snap(f"worker-{identity}-hover")
                x = (c["dock"]["x"]+3)*c["cell"]["width"]
                y = c["dock"]["y"]*c["cell"]["height"]+2
                headers.append(image.crop(x, y, 22*c["cell"]["width"], c["cell"]["height"]-4))
            state = app.state()
            assert selected(state) == active
            assert (state["first_responder"], state["key_window"]) == focus, "hover changed native focus"
            for other, worker_path in workers:
                original, old_log = before[other]
                current = tab(state, other)
                for key in ("id", "pid", "fd", "cols", "rows", "cx", "cy", "view", "hist", "activity", "attention"):
                    assert current[key] == original[key], (other, key, original[key], current[key])
                assert worker_data(worker_path) == old_log, ("hover wrote input or resized background PTY", other)
    assert all(image == headers[0] for image in headers), "terminal background leaked through opaque preview menu"
    c = app.hover(-1, -1)
    shown(c, active, active)
    assert c["source"] == source
    print("PASS: 12 immediate traversals retain background unread, source, PTY size/WINCH/input and opaque pinned menu")


def test_lifecycle(app):
    active, source, workers, state = setup_workers(app)
    target = workers[0][0]
    cases = [("leave-list", None), ("leave-window", "cmd preview-event exit"),
             ("focus-loss", "cmd focus 0"), ("resign-key", "cmd preview-event resign"),
             ("minimize", "cmd preview-event minimize"), ("keyboard", "key 123 0 - -"),
             ("native-key-monitor", "cmd preview-event monitor-key"),
             ("resize", "cmd resize 1003,702")]
    for name, command in cases:
        app.send("cmd focus 1")
        c = hover_tab(app, target)
        shown(c, active, target)
        if command:
            app.send(command)
            c = app.chrome()
        else:
            c = app.hover(c["dock"]["x"]-2, c["dock"]["y"]+1)
        shown(c, active, active)
        assert selected(app.state()) == active, name
    app.send("cmd focus 0")
    shown(hover_tab(app, target), active, active)
    print("PASS: list/window exit, focus/resign/minimize, keyboard and resize cancel without activation")


def browser_command(app, *arguments):
    executable = app.root / "hardcore.app/Contents/MacOS/hardcore"
    result = subprocess.run([str(executable), "browser", *arguments, "--window", str(app.proc.pid)],
                            capture_output=True, text=True, check=True, timeout=5)
    return json.loads(result.stdout)


def test_browsers(app):
    app.send("cmd resize 1001,700")
    app.send("cmd focus 1")
    active = selected(app.state())
    c = app.chrome()
    source = app.feed(["TERMINAL-OWNS-INPUT"] + [""]*(c["rows"]-1))["source"]
    html = app.root / "preview.html"
    html.write_text("<!doctype html><meta charset=utf-8><title>Owned Browser Preview</title>"
                    "<h1>OWNED BROWSER PREVIEW</h1><p>This local page stays in its original mode while hovered.</p>"
                    "<p><b>Bold text</b> and a <a href='#fixture'>local link</a>.</p><input value='preserved draft'>")
    identities = []
    for mode in ("hardcore", "native"):
        identity = browser_command(app, "open", str(html), "--mode", mode)["id"]
        wait_for(lambda: any(item["id"] == identity and item["title"] == "Owned Browser Preview"
                             for item in browser_command(app, "list")["tabs"]), description=f"{mode} local browser load")
        identities.append(identity)
    select_tab(app, active)
    app.hover(-1, -1)
    fixed = geometry(app.chrome())
    modes = {item["id"]: item["mode"] for item in browser_command(app, "list")["tabs"]}
    initial = app.state()
    for identity in identities:
        shown(hover_tab(app, identity), active, identity)
        app.snap(f"browser-{modes[identity]}-projected-preview")
        def projected():
            c = app.chrome()
            return c if any("OWNED BROWSER PREVIEW" in line for line in c["source"]) else None
        c = wait_for(projected, description="cached browser grid rendered")
        shown(c, active, identity)
        assert geometry(c) == fixed
        raised(c)
        assert selected(app.state()) == active
        assert {item["id"]: item["mode"] for item in browser_command(app, "list")["tabs"]} == modes
        state = app.state()
        assert (state["first_responder"], state["key_window"]) == (initial["first_responder"], initial["key_window"])
        c = app.hover(-1, -1)
        shown(c, active, active)
        assert c["source"] == source
    native = identities[1]
    select_tab(app, native)
    initial = app.state()
    c = hover_tab(app, active)
    shown(c, native, active)
    assert c["source"] == source
    state = app.state()
    assert (state["first_responder"], state["key_window"]) == (initial["first_responder"], initial["key_window"])
    app.send("cmd preview-event monitor-key")
    c = app.chrome()
    shown(c, native, native)
    state = app.state()
    assert (state["first_responder"], state["key_window"]) == (initial["first_responder"], initial["key_window"])
    assert {item["id"]: item["mode"] for item in browser_command(app, "list")["tabs"]} == modes
    print("PASS: native and Hardcore browser previews preserve selected tab, modes and responder")


def main():
    cases = {"workers": test_workers, "lifecycle": test_lifecycle, "browsers": test_browsers}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=cases)
    parser.add_argument("--artifacts", type=Path, default=Path("build/tab-preview-extended"))
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="hc-preview-extended-") as directory:
        for name, check in cases.items():
            if args.case and args.case != name:
                continue
            with NativeApp(Path(directory)/name, 1, args.artifacts.resolve()) as app:
                try:
                    check(app)
                except (AssertionError, subprocess.SubprocessError) as error:
                    failures.append(name)
                    app.snap(name+"-failure")
                    print(f"FAIL: {name}: {error}", flush=True)
    if failures:
        raise SystemExit("FAILED: " + ", ".join(failures))
    print(f"PASS: extended native previews; screenshots: {args.artifacts.resolve()}")


if __name__ == "__main__":
    main()
