#!/usr/bin/env python3
"""Three-row header hit geometry and native cursor ownership in an owned app."""
import argparse
from collections import Counter
from pathlib import Path
import tempfile
import time

from test_chrome_mac import NativeApp
from test_tab_preview_extended_mac import selected


def event(app, kind, x, y):
    app.send(f"cmd pointer {kind} {x},{y}")
    return app.chrome()


def test_header(app):
    c=app.quiet_tabs(2)
    app.send("cmd focus 1")
    c=app.feed([""]*c["rows"])
    app.hover(-1,-1)
    c=app.chrome()
    controls={item["name"]:item for item in c["header_controls"]}
    expected={"move":(0,1,7,"↕ move"),"settings":(0,9,8,"⚙config"),"camera":(0,18,4,"cam"),"caf":(0,24,3,"caf"),
              "term":(1,1,6,"+term"),"web":(1,8,5,"+www"),"files":(1,14,6,"files"),"music":(1,21,6,"music"),
              "grid":(2,1,7,"Grid")}
    assert set(controls)==set(expected),controls
    assert c["pages"]["tabs"]["row"]==c["dock"]["y"]+3
    occupied=set()
    assert len(c["header_hits"])==3 and all(len(row)==c["dock"]["width"] for row in c["header_hits"])
    for name,(row,col,width,label) in expected.items():
        control=controls[name]
        assert (control["row"],control["col"],control["width"],control["label"])==(row,col,width,label),control
        for x in range(col,col+width):
            assert (x,row) not in occupied,"header controls overlap"
            assert c["header_hits"][row][x]==control["hit"],(name,x,"rendered label and native action disagree")
            occupied.add((x,row))
    gaps={c["header_hits"][row][col] for row in range(3) for col in range(c["dock"]["width"])
          if (col,row) not in occupied}
    assert len(gaps)==1 and not gaps.intersection({0,4}|{control["hit"] for control in controls.values()}), \
        ("header gap is not inert or falls through to terminal/window drag",gaps)
    image=app.snap("three-row-header-labels")
    cw,ch=c["cell"]["width"],c["cell"]["height"]
    for name,control in controls.items():
        x,y=app.header_point(c,name)
        for offset,char in enumerate(control["label"]):
            if char.isspace():continue
            pixels=image.crop((x+offset)*cw,y*ch+3,cw,ch-4)
            colors=Counter(pixels[i:i+4] for i in range(0,len(pixels),4))
            assert len(colors)>1,(name,char,"header character has no visible ink")
    initial=app.state()
    selected_id=selected(initial)
    dock=dict(c["dock"])
    # Every spacer cell is tested through native pointer down/drag/up. A
    # spacer must not turn into the old window-drag or shell-input fallback.
    for row in range(3):
        for col in range(1,dock["width"]-1):
            if (col,row) in occupied:continue
            x=int((dock["x"]+col+.5)*cw);y=int((dock["y"]+row+.5)*ch)
            c=event(app,"move",x,y)
            assert c["pointer_cursor"]=="arrow",(col,row,c["pointer_cursor"])
            event(app,"down",x,y)
            event(app,"drag",x+2*cw,y+2*ch)
            c=event(app,"up",x+2*cw,y+2*ch)
            state=app.state()
            assert c["dock"]==dock and state["frame"]==initial["frame"],"header spacer initiated a drag"
            assert len(state["tabs"])==2 and selected(state)==selected_id
            assert not c["settings_open"] and not c["caf_active"]
            assert not c["titlebar"]["selection_on"] and not c["titlebar"]["selection_drag"]
            assert not c["preview"]["preview_id"] and c["input_tab_id"]==selected_id
            assert state["first_responder"]==initial["first_responder"]
    app.hover(-1,-1)
    print("PASS: three-row header labels and nonoverlapping controls render; every interior gap stays inert without moving window/menu or changing input")


def test_cursor(app):
    c=app.quiet_tabs(2)
    app.send("cmd tab 0")
    app.send("cmd focus 1")
    c=app.feed([""]*c["rows"])
    app.hover(-1,-1)
    initial=app.state();main=selected(initial)
    cw,ch=c["cell"]["width"],c["cell"]["height"]
    grip_col,grip_row=app.header_point(c,"move")
    settings_col,settings_row=app.header_point(c,"settings")
    regions=(("top content",40*cw,ch//2,"ibeam"),
             ("terminal",40*cw,10*ch,"ibeam"),
             ("move",int((grip_col+.5)*cw),int((grip_row+.5)*ch),"open-hand"),
             ("settings",int((settings_col+.5)*cw),int((settings_row+.5)*ch),"pointing-hand"))
    # Seed a real preview first, so entering title/header/terminal regions
    # proves cursor ownership does not accidentally retain a hovered tab.
    second=initial["tabs"][1]["id"]
    c=app.hover(c["dock"]["x"]+9,c["pages"]["tabs"]["row"]+1)
    assert c["preview"]["preview_id"]==second and selected(app.state())==main
    for name,x,y,kind in regions:
        for route in ("move","cursor","flags","monitor","cursor","move"):
            c=event(app,route,x,y)
            assert c["pointer_cursor"]==kind,(name,route,c["pointer_cursor"])
            assert not c["preview"]["preview_id"] and c["input_tab_id"]==main,(name,route,c["preview"])
            state=app.state()
            assert selected(state)==main and state["first_responder"]==initial["first_responder"]
            assert state["frame"]==initial["frame"]
        time.sleep(.08)
        assert app.chrome()["pointer_cursor"]==kind,name+" cursor was overwritten after the event"
    x=int((grip_col+.5)*cw);y=int((grip_row+.5)*ch)
    event(app,"move",x,y)
    c=event(app,"down",x,y)
    assert c["pointer_cursor"]=="closed-hand",c["pointer_cursor"]
    c=event(app,"up",x,y)
    assert c["pointer_cursor"]=="open-hand",c["pointer_cursor"]
    app.hover(-1,-1)
    app.snap("header-cursor-regression-complete")
    print("PASS: title arrow, terminal I-beam and menu hands remain stable through native move/cursorUpdate/flags/monitor without preview or focus leakage")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case",choices=("header","cursor"))
    parser.add_argument("--artifacts",type=Path,default=Path("build/header-cursor-native"))
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hc-header-cursor-") as root:
        for name,check in (("header",test_header),("cursor",test_cursor)):
            if args.case and args.case!=name:continue
            with NativeApp(Path(root)/name,1,args.artifacts.resolve()) as app:
                try:check(app)
                except BaseException:
                    app.snap(name+"-failure")
                    raise
    print(f"PASS: native header/cursor; screenshots: {args.artifacts.resolve()}")


if __name__=="__main__":
    main()
