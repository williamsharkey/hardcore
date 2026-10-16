#!/usr/bin/env python3
"""Live Cutie terminal panels, driven only through disposable native apps.

Owned raw PTY workers independently record input, tty sizes and SIGWINCH.
They emit deterministic styled frames on private file requests, so assertions
do not confuse an unchanged tab index with correctly routed terminal input.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time

from test_chrome_mac import NativeApp, wait_for
from test_tab_preview_extended_mac import selected, select_tab, tab
from test_tv_mac import header, rect_overlap


WORKER = r'''
import fcntl,json,os,select,signal,struct,sys,termios,tty
from pathlib import Path
path=Path(sys.argv[1]);label=sys.argv[2]
received=bytearray();winches=[];generation=0
def size():return list(struct.unpack("HHHH",fcntl.ioctl(0,termios.TIOCGWINSZ,bytes(8)))[:2])
def resized(signum,frame):winches.append(size())
signal.signal(signal.SIGWINCH,resized)
tty.setraw(0)
def record():
    temp=path.with_suffix(".tmp")
    temp.write_text(json.dumps(dict(pid=os.getpid(),input=received.hex(),winch=winches,
                                   size=size(),generation=generation)))
    temp.replace(path.with_suffix(".json"))
record()
while True:
    try:request=json.loads(path.with_suffix(".render").read_text())
    except FileNotFoundError:request={}
    if request.get("generation",0)>generation:
        generation=request["generation"];rows,cols=size()
        color=request.get("color",[35,55,115]);out="\x1bc\x1b[2J\x1b[3J"
        if request.get("sparse"):
            out+=f"\x1b[21;1H{label} SELECTED INPUT"
        else:
            for row in range(rows):
                text=(f"{label} gen{generation} row{row:02d} "+"abcdef ")*20
                if row==1:text="bold and underline "+"STYLE "*80
                if row==2:text="Unicode: café é 🚀 界 "+"wide "*80
                style="\x1b[1;4m" if row==1 else "\x1b[22;24m"
                out+=f"\x1b[{row+1};1H\x1b[48;2;{color[0]};{color[1]};{color[2]}m\x1b[38;2;225;215;205m"+style+text[:cols]
        if request.get("mouse"):out+="\x1b[?1000h\x1b[?1006h"
        if request.get("focus"):out+="\x1b[?1004h"
        col,row=request.get("cursor",[12,5])
        out+=f"\x1b[0m\x1b[{row+1};{col+1}H"+("\x1b[?25h" if request.get("cursor_visible",True) else "\x1b[?25l")
        if request.get("bell"):out+="\a"
        os.write(1,out.encode())
    if select.select([0],[],[],.02)[0]:
        data=os.read(0,4096)
        if not data:break
        received.extend(data)
    record()
'''


def data(path):
    try:
        return json.loads(path.with_suffix(".json").read_text())
    except FileNotFoundError:
        return None


def request_frame(path, **values):
    current = data(path)
    generation = current["generation"]+1
    request = dict(generation=generation, **values)
    temporary = path.with_suffix(".render-tmp")
    temporary.write_text(json.dumps(request))
    temporary.replace(path.with_suffix(".render"))
    wait_for(lambda: data(path)["generation"] == generation, description="owned Cutie worker frame")
    return generation


def setup_workers(app, count=2):
    app.send("cmd resize 1001,700")
    app.send("cmd focus 1")
    # Replace the fixture's sleep with a raw worker so input to the selected
    # main tab is independently observable, too.
    app.send("type \x03")
    wait_for(lambda: app.state()["tabs"][0]["program"] != "sleep", description="owned fixture shell")
    script = app.root / "cutie_worker.py"
    script.write_text(WORKER)
    workers = []
    for index in range(count):
        if index:
            app.send("cmd newtab")
        identity = selected(app.state())
        path = app.root / f"worker-{index}"
        command = "exec " + " ".join(shlex.quote(value) for value in
            (sys.executable, "-u", str(script), str(path), f"CUTIE{index}"))
        app.send("type " + command + r"\n")
        ready = wait_for(lambda: data(path), description=f"owned Cutie worker {index}")
        app.pids.add(ready["pid"])
        workers.append((identity, path))
        request_frame(path, sparse=not index, cursor_visible=False)
    main = workers[0][0]
    select_tab(app, main)
    app.hover(-1, -1)
    return main, workers


def actual_click(app, c, col, row):
    x, y = int((col+.5)*c["cell"]["width"]), int((row+.5)*c["cell"]["height"])
    app.send(f"cmd pointer down {x},{y}")
    app.send(f"cmd pointer up {x},{y}")
    return app.chrome()


def cuties(c):
    return [tv for tv in c["tvs"] if tv.get("source_tab_id", 0) and tv["open"]]


def cutie(c, identity):
    return next(tv for tv in cuties(c) if tv["source_tab_id"] == identity)


def open_cutie(app, identity):
    state, c = app.state(), app.chrome()
    index = next(i for i, item in enumerate(state["tabs"]) if item["id"] == identity)
    row = c["tab_rows"][index]//c["cell"]["height"]
    assert row >= 0, "fixture Cutie source tab is not visible"
    icon = c["dock"]["x"]+c["dock"]["width"]-4
    c = app.hover(icon, row)
    c = actual_click(app, c, icon, row)
    app.hover(-1, -1)
    return app.chrome()


def close_cutie(app, identity):
    c = app.chrome()
    r = cutie(c, identity)["rect"]
    c = app.hover(r["x"]+r["width"]-2, r["y"])
    return actual_click(app, c, r["x"]+r["width"]-2, r["y"])


def stable_pty(before, after):
    for key in ("pid", "size", "winch", "input"):
        assert before[key] == after[key], (key, before, after)


def panel_pixels(app, c, identity, label):
    tv = cutie(c, identity)
    r = tv["body"]
    image = app.snap(label)
    cw, ch = c["cell"]["width"], c["cell"]["height"]
    return image.crop(r["x"]*cw, r["y"]*ch, r["width"]*cw, r["height"]*ch)


def check_geometry(c):
    occupied = [c["dock"]]
    for tv in c["tvs"]:
        if not tv["open"] or not tv["placed"]:
            continue
        r = tv["rect"]
        assert 0 <= r["x"] < r["x"]+r["width"] <= c["cols"], tv
        assert 0 <= r["y"] < r["y"]+r["height"] <= c["rows"], tv
        assert all(not rect_overlap(r, prior) for prior in occupied), (r, occupied)
        occupied.append(r)


def test_live(app):
    main, workers = setup_workers(app)
    source, path = workers[1]
    request_frame(path, bell=True, cursor_visible=False)
    def unread():
        s = app.state()
        return s if tab(s, source)["activity"] and tab(s, source)["attention"] else None
    before = wait_for(unread, description="Cutie source unread and bell flags")
    logs = {identity: data(p) for identity, p in workers}
    original = app.chrome()["source"]
    c = open_cutie(app, source)
    tv = cutie(c, source)
    assert tv["placed"] and selected(app.state()) == main
    assert c["source"] == original, "opening Cutie changed the main terminal source"
    after = app.state()
    assert (before["first_responder"], before["key_window"]) == (after["first_responder"], after["key_window"]), \
        "opening a Cutie changed native focus"
    for key in ("activity", "attention", "cols", "rows", "hist", "view", "cx", "cy"):
        assert tab(before, source)[key] == tab(after, source)[key], key
    for identity, p in workers:
        stable_pty(logs[identity], data(p))
    first = panel_pixels(app, c, source, "cutie-live-before")
    assert first.count(bytes((115,55,35,255))) > len(first)//12, \
        "Cutie body lost its opaque source truecolor background"
    assert bytes((205,215,225,255)) in first, "Cutie body lost the source truecolor glyphs"
    request_frame(path, color=[105,30,45], cursor_visible=False)
    def changed():
        c = app.chrome()
        content = panel_pixels(app, c, source, "cutie-live-after")
        return c if content != first else None
    c = wait_for(changed, description="live Cutie Metal output", timeout=5)
    current = panel_pixels(app, c, source, "cutie-live-updated-colors")
    assert current.count(bytes((45,30,105,255))) > len(current)//12, \
        "new source background was flattened or blended in the Cutie"
    assert selected(app.state()) == main and c["source"] == original
    assert tab(app.state(), source)["activity"], "live Cutie cleared unread just by displaying output"
    stable_pty(logs[source], data(path))
    c = open_cutie(app, source)
    assert len(cuties(c)) == 1, "reopening one source created a duplicate Cutie"
    c = close_cutie(app, source)
    assert not cuties(c) and selected(app.state()) == main
    assert any(t["id"] == source for t in app.state()["tabs"]), "Cutie close removed its source tab"
    os.kill(data(path)["pid"], 0)
    stable_pty(logs[source], data(path))
    print("PASS: Cutie icon opens one live styled panel, preserving selected tab, unread, PTY size and source process")


def type_key(app, text):
    app.send(f"key 0 0 {text} {text}")


def wait_input(path, expected):
    return wait_for(lambda: bytes.fromhex(data(path)["input"]) == expected,
                    description=f"exact input {expected!r} in owned PTY", timeout=5)


def focus_cutie(app, identity):
    c = app.chrome()
    r = cutie(c, identity)["body"]
    return actual_click(app, c, r["x"]+2, r["y"]+2)


def test_input(app):
    main, workers = setup_workers(app)
    source, path = workers[1]
    mainpath = workers[0][1]
    c = open_cutie(app, source)
    type_key(app, "a")
    wait_input(mainpath, b"a")
    assert data(path)["input"] == "", "opening a Cutie stole keyboard input"
    c = focus_cutie(app, source)
    assert selected(app.state()) == main and cutie(c, source)["focused"]
    type_key(app, "b")
    app.send("key 123 0 - -")
    wait_input(path, b"b\x1b[D")
    wait_input(mainpath, b"a")
    main_view = tab(app.state(), main)["view"]
    app.send("key 116 20000 - -")
    c = wait_for(lambda: (c if cutie(c := app.chrome(), source)["scroll"] > 0 else None),
                 description="Shift PageUp scrolls focused Cutie")
    assert tab(app.state(), main)["view"] == main_view
    app.send("key 119 20000 - -")
    c = wait_for(lambda: (c if cutie(c := app.chrome(), source)["scroll"] == 0 else None),
                 description="Shift End returns Cutie to live view")
    assert tab(app.state(), main)["view"] == main_view
    wait_input(path, b"b\x1b[D")
    app.send(r"key 53 0 \x1b \x1b")
    type_key(app, "c")
    wait_input(mainpath, b"ac")
    assert bytes.fromhex(data(path)["input"]) == b"b\x1b[D", "focus Escape leaked into source app"
    c = app.chrome()
    assert not cutie(c, source)["focused"] and selected(app.state()) == main
    focus_cutie(app, source)
    c = actual_click(app, app.chrome(), 45, 30)
    type_key(app, "d")
    wait_input(mainpath, b"acd")
    assert not cutie(c, source)["focused"]
    focus_cutie(app, source)
    app.send("cmd focus 0")
    app.send("cmd focus 1")
    type_key(app, "e")
    wait_input(mainpath, b"acde")
    assert not cutie(app.chrome(), source)["focused"]
    assert bytes.fromhex(data(path)["input"]) == b"b\x1b[D"
    focus_cutie(app, source)
    app.send("cmd preview-event minimize")
    app.send("cmd focus 1")
    type_key(app, "f")
    wait_input(mainpath, b"acdef")
    assert not cutie(app.chrome(), source)["focused"]
    focus_cutie(app, source)
    close_cutie(app, source)
    type_key(app, "g")
    wait_input(mainpath, b"acdefg")
    os.kill(data(path)["pid"], 0)
    assert bytes.fromhex(data(path)["input"]) == b"b\x1b[D"
    app.snap("cutie-input-returned-to-main")
    print("PASS: body click routes keys/arrows to source; Escape, canvas click, focus loss, minimize and close return input to main")


def test_capacity(app):
    main, workers = setup_workers(app, 6)
    logs = {identity: data(path) for identity, path in workers}
    c = app.settings_section(1)
    c = header(app, 21)
    try:
        for identity, path in workers[1:5]:
            c = open_cutie(app, identity)
            check_geometry(c)
        panels = cuties(c)
        assert len(panels) == 4 and all(tv["placed"] for tv in panels)
        assert c["settings_open"] and c["caf_active"]
        assert sum(tv["open"] and tv["placed"] for tv in c["tvs"]) == 6
        identities = {tv["source_tab_id"] for tv in panels}
        c = open_cutie(app, workers[5][0])
        assert {tv["source_tab_id"] for tv in cuties(c)} == identities, "fifth Cutie evicted an existing panel"
        assert len(app.state()["tabs"]) == len(workers) and selected(app.state()) == main
        app.hover(-1, -1)
        app.snap("four-cuties-settings-and-caf")
        # Cuties persist independently of the transient Settings/CAF timeout.
        time.sleep(10.5)
        c = app.chrome()
        assert {tv["source_tab_id"] for tv in cuties(c)} == identities
        assert all(tv["placed"] and tv["alpha"] == 1 for tv in cuties(c))
        for identity, path in workers:
            stable_pty(logs[identity], data(path))
            os.kill(data(path)["pid"], 0)
        check_geometry(c)
        app.snap("cuties-persist-after-idle")
    finally:
        if app.chrome()["caf_active"]:
            header(app, 21)
    print("PASS: four bounded Cuties coexist with Settings/CAF, reject a fifth without eviction and persist beyond idle timeout")


def test_focus_reports(app):
    main, workers = setup_workers(app)
    source, path = workers[1]
    mainpath = workers[0][1]
    request_frame(mainpath, sparse=True, focus=True, cursor_visible=False)
    request_frame(path, focus=True, cursor_visible=False)
    open_cutie(app, source)
    assert data(mainpath)["input"] == data(path)["input"] == "", "opening Cutie sent unsolicited focus reports"
    focus_cutie(app, source)
    wait_input(mainpath, b"\x1b[O")
    wait_input(path, b"\x1b[I")
    app.send(r"key 53 0 \x1b \x1b")
    wait_input(mainpath, b"\x1b[O\x1b[I")
    wait_input(path, b"\x1b[I\x1b[O")
    focus_cutie(app, source)
    app.send("cmd focus 0")
    wait_input(mainpath, b"\x1b[O\x1b[I\x1b[O")
    wait_input(path, b"\x1b[I\x1b[O\x1b[I\x1b[O")
    app.send("cmd focus 1")
    wait_input(mainpath, b"\x1b[O\x1b[I\x1b[O\x1b[I")
    assert bytes.fromhex(data(path)["input"]) == b"\x1b[I\x1b[O\x1b[I\x1b[O", "Cutie received duplicate focus-out on window loss"
    assert selected(app.state()) == main and app.chrome()["input_tab_id"] == main
    print("PASS: DEC1004 focus ownership transfers once between main/Cutie and returns cleanly after window loss")


def test_mouse_cursor(app):
    main, workers = setup_workers(app)
    source, path = workers[1]
    request_frame(workers[0][1], sparse=True, mouse=True, cursor_visible=False)
    request_frame(path, mouse=True, cursor=[12, 2])
    open_cutie(app, source)
    assert data(workers[0][1])["input"] == "", "Cutie icon leaked an unpaired mouse release into selected PTY"
    def cursor_ready():
        c = app.chrome()
        tv = cutie(c, source)
        return c if tv["cursor"] >= 0 else None
    c = wait_for(cursor_ready, description="live cursor in compact Cutie view")
    tv, state = cutie(c, source), tab(app.state(), source)
    mapped = tv["source_map"][tv["cursor"]]
    source_row = tv["source_first_line"]+mapped//tv["source_cols"]-tv["source_hcount"]
    assert (mapped%tv["source_cols"], source_row) == (state["cx"], state["cy"]) == (12, 2)
    cells = tv["content"]
    assert any(cell["wide"] == 1 for cell in cells), "fixture failed to retain its Unicode wide glyphs"
    for at, cell in enumerate(cells):
        if cell["wide"] == 1:
            assert at%tv["body"]["width"]+1 < tv["body"]["width"] and cells[at+1]["wide"] == 2
        if cell["wide"] == 2:
            assert at%tv["body"]["width"] > 0 and cells[at-1]["wide"] == 1
    assert any(cell["attr"] for cell in cells), "compact Cutie lost bold/underline attributes"
    # Choose a source column beyond the compact panel's width: translating by
    # body coordinates alone would send this click to the wrong TUI control.
    at = next(i for i, original in enumerate(tv["source_map"])
              if original >= 0 and original%tv["source_cols"] > tv["body"]["width"])
    original = tv["source_map"][at]
    sx, sy = original%tv["source_cols"], tv["source_first_line"]+original//tv["source_cols"]-tv["source_hcount"]
    r = tv["body"]
    app.snap("cutie-cursor-and-wide-glyphs")
    actual_click(app, c, r["x"]+at%r["width"], r["y"]+at//r["width"])
    expected = f"\x1b[<0;{sx+1};{sy+1}M\x1b[<0;{sx+1};{sy+1}m".encode()
    wait_input(path, expected)
    c = app.chrome()
    assert c["input_tab_id"] == source and selected(app.state()) == main
    assert data(workers[0][1])["input"] == "", "mapped Cutie mouse event leaked to selected PTY"
    tv = cutie(c, source)
    padding = next(i for i, original in enumerate(tv["source_map"]) if original < 0)
    r = tv["body"]
    actual_click(app, c, r["x"]+padding%r["width"], r["y"]+padding//r["width"])
    time.sleep(.1)
    assert bytes.fromhex(data(path)["input"]) == expected, "unmapped projection padding clicked a source cell"
    x = int((r["x"]+2.5)*c["cell"]["width"])
    y = int((r["y"]+2.5)*c["cell"]["height"])
    for button in ("right", "middle"):
        app.send(f"cmd pointer {button}-down {x},{y}")
        app.send(f"cmd pointer {button}-up {x},{y}")
        time.sleep(.1)
        assert bytes.fromhex(data(path)["input"]) == expected, f"{button} Cutie click sent an unpaired source mouse event"
        assert data(workers[0][1])["input"] == "", f"{button} Cutie click leaked into main mouse-enabled PTY"
    print("PASS: compact cursor/wide styles retain source mapping; SGR mouse coordinates follow wrapped source cells and padding stays inert")


def test_source_close(app):
    main, workers = setup_workers(app, 3)
    for identity, path in workers[1:]:
        open_cutie(app, identity)
    source = workers[1][0]
    focus_cutie(app, source)
    select_tab(app, source)
    app.send("cmd closetab")
    def retired():
        c = app.chrome()
        return c if not any(tv["source_tab_id"] == source for tv in cuties(c)) else None
    c = wait_for(retired, description="closed source retires Cutie")
    assert len(cuties(c)) == 1 and cuties(c)[0]["source_tab_id"] == workers[2][0]
    assert not any(t["id"] == source for t in app.state()["tabs"])
    select_tab(app, main)
    type_key(app, "z")
    wait_input(workers[0][1], b"z")
    assert data(workers[2][1])["input"] == ""
    print("PASS: source-tab close retires only its Cutie and leaves input routed to a valid live tab")


def content_text(tv):
    return "".join(chr(cell["ch"] or 32) for cell in tv["content"])


def browser_text(app, identity, text):
    def ready():
        c = app.chrome()
        return c if text in content_text(cutie(c, identity)) else None
    return wait_for(ready, description=f"browser Cutie text {text!r}", timeout=10)


def click_browser_text(app, identity, text):
    c = browser_text(app, identity, text)
    tv = cutie(c, identity)
    at = content_text(tv).index(text)
    assert tv["content"][at]["link"] > 0, (text, tv)
    r = tv["body"]
    # Paint the snapshot carrying this DOM action before issuing the event.
    app.snap("browser-cutie-before-action")
    return actual_click(app, c, r["x"]+at%r["width"], r["y"]+at//r["width"])


def wheel_cutie(app, c, identity, delta):
    r = cutie(c, identity)["body"]
    x, y = int((r["x"]+2.5)*c["cell"]["width"]), int((r["y"]+2.5)*c["cell"]["height"])
    app.send(f"cmd wheel {x},{y},{delta}")
    return app.chrome()


def test_browser(app):
    from test_tab_preview_extended_mac import browser_command
    main, workers = setup_workers(app)
    original_source = app.chrome()["source"]
    assert any("CUTIE0 SELECTED INPUT" in line for line in original_source)
    main_image = app.snap("browser-cutie-original-terminal")
    html, destination = app.root/"cutie-browser.html", app.root/"cutie-next.html"
    destination.write_text("<!doctype html><meta charset=utf-8><title>Cutie Destination</title>"
                           "<p>Destination reached.</p><textarea>KEEP_DRAFT</textarea>")
    html.write_text("<!doctype html><meta charset=utf-8><title>Cutie Browser</title>"
                    "<p><button onclick=\"this.textContent='Clicked'\">Tap</button> "
                    "<a href='cutie-next.html' target='_blank'>Next</a></p>"+
                    "".join(f"<p>Independent row {index:02d}.</p>" for index in range(30)))
    identity = browser_command(app, "open", str(html), "--mode", "native")["id"]
    wait_for(lambda: any(t["id"] == identity and t["title"] == "Cutie Browser"
                         for t in browser_command(app, "list")["tabs"]), description="local Cutie browser load")
    select_tab(app, main)
    assert app.chrome()["source"] == original_source, "opening a browser changed the main terminal source"
    native_before = app.state()
    original_scroll = tab(native_before, identity)["browser_scroll"]
    c = open_cutie(app, identity)
    c = browser_text(app, identity, "Tap")
    state = app.state()
    assert selected(state) == main and c["input_tab_id"] == main
    assert c["source"] == original_source, "opening a browser Cutie changed main terminal cells"
    assert (state["first_responder"], state["key_window"]) == (native_before["first_responder"], native_before["key_window"])
    assert any(cell["link"] for cell in cutie(c, identity)["content"])
    image = app.snap("browser-cutie-cached-links")
    cw, ch = c["cell"]["width"], c["cell"]["height"]
    assert main_image.crop(0, 20*ch, 25*cw, ch) == image.crop(0, 20*ch, 25*cw, ch), \
        "browser Cutie changed pixels of the main terminal away from every panel"
    click_browser_text(app, identity, "Tap")
    c = browser_text(app, identity, "Clicked")
    assert selected(app.state()) == main and c["input_tab_id"] == main, "simple browser button stole selected/input tab"
    initial_text = content_text(cutie(c, identity))
    wheel_cutie(app, c, identity, -3)
    def scrolled():
        c = app.chrome()
        tv = cutie(c, identity)
        return c if tv["scroll"] > 0 and content_text(tv) != initial_text else None
    c = wait_for(scrolled, description="independent browser Cutie wheel scroll")
    assert tab(app.state(), identity)["browser_scroll"] == original_scroll, "Cutie scroll changed normal browser scroll"
    assert selected(app.state()) == main
    wheel_cutie(app, c, identity, 20)
    browser_text(app, identity, "Next")
    count = len(app.state()["tabs"])
    click_browser_text(app, identity, "Next")
    wait_for(lambda: any(t["id"] == identity and t["title"] == "Cutie Destination"
                         for t in browser_command(app, "list")["tabs"]), description="Cutie link navigates source browser")
    c = browser_text(app, identity, "KEEP_DRAFT")
    assert selected(app.state()) == main and len(app.state()["tabs"]) == count, \
        "simple target=_blank Cutie link activated or created a different browser tab"
    click_browser_text(app, identity, "KEEP_DRAFT")
    wait_for(lambda: selected(app.state()) == identity, description="textarea expands to its full browser workspace")
    assert next(t for t in browser_command(app, "list")["tabs"] if t["id"] == identity)["mode"] == "native"
    app.snap("browser-cutie-explicit-form-expansion")

    # A separate untouched native page owns the main surface while a terminal
    # Cutie takes temporary input. Escape must restore that exact WK responder.
    page = app.root/"native-main.html"
    page.write_text("<!doctype html><title>Native Main</title><p>Native keyboard owner</p>")
    browser_main = browser_command(app, "open", str(page), "--mode", "native")["id"]
    wait_for(lambda: any(t["id"] == browser_main and t["title"] == "Native Main"
                         for t in browser_command(app, "list")["tabs"]), description="native main keyboard fixture")
    before = app.state()
    assert "WK" in before["first_responder"], before["first_responder"]
    terminal, path = workers[1]
    open_cutie(app, terminal)
    c = focus_cutie(app, terminal)
    assert selected(app.state()) == browser_main and c["input_tab_id"] == terminal
    type_key(app, "n")
    wait_input(path, b"n")
    app.send(r"key 53 0 \x1b \x1b")
    c, after = app.chrome(), app.state()
    assert selected(after) == browser_main and c["input_tab_id"] == browser_main
    assert after["first_responder"] == before["first_responder"], \
        ("Escape did not restore native browser responder", before["first_responder"], after["first_responder"])
    assert data(workers[0][1])["input"] == "" and bytes.fromhex(data(path)["input"]) == b"n"
    focus_cutie(app, terminal)
    app.send("cmd focus 0")
    app.send("cmd focus 1")
    c, after = app.chrome(), app.state()
    assert c["input_tab_id"] == browser_main and after["first_responder"] == before["first_responder"], \
        "native WK responder was not restored after Cutie focus loss/regain"
    app.snap("native-browser-restored-after-terminal-cutie")
    print("PASS: cached browser Cuties scroll independently, run links/buttons in source, expand form edits and return terminal input focus to WK")


def main():
    cases = {"live": test_live, "input": test_input, "mouse-cursor": test_mouse_cursor,
             "focus-reports": test_focus_reports, "capacity": test_capacity,
             "source-close": test_source_close, "browser": test_browser}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=cases)
    parser.add_argument("--artifacts", type=Path, default=Path("build/cutie-native"))
    args = parser.parse_args()
    failures = []
    with tempfile.TemporaryDirectory(prefix="hc-cutie-native-") as directory:
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
        raise SystemExit("FAILED: "+", ".join(failures))
    print(f"PASS: native Cuties; screenshots: {args.artifacts.resolve()}")


if __name__ == "__main__":
    main()
