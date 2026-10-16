#!/usr/bin/env python3
"""Native browser image orientation, size and relayout in a disposable app.

Fixtures are local data-URI PNGs; no network page, user tab, or input is used.
The real Metal screenshot must show all four source quadrants in order, even
in image columns beyond the old four-bit tile-coordinate limit.
"""
import argparse
import base64
from pathlib import Path
import struct
import tempfile
import zlib

from test_chrome_mac import NativeApp, wait_for
from test_tab_preview_extended_mac import browser_command, selected


def quadrant_png(width, height):
    colors = ((197,15,31),(19,161,14),(0,55,218),(193,156,0))
    rows = []
    for y in range(height):
        row = bytearray(b"\0")
        for x in range(width):
            row.extend(colors[(2 if y >= height//2 else 0)+(x >= width//2)])
        rows.append(row)
    def chunk(kind,data):
        return struct.pack(">I",len(data))+kind+data+struct.pack(">I",zlib.crc32(kind+data))
    header = struct.pack(">IIBBBBB",width,height,8,2,0,0,0)
    return b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",header)+chunk(b"IDAT",zlib.compress(b"".join(rows)))+chunk(b"IEND",b"")


def image_rect(chrome, identity):
    points = []
    for y,line in enumerate(chrome["source"]):
        for x,char in enumerate(line):
            value=ord(char)
            width=(value>>8)&255;height=(value>>16)&255
            if value&255 == identity and 1<=width<=24 and 1<=height<=8:
                points.append((x,y,width,height))
    if not points:
        return None
    width,height=points[0][2:]
    left=min(point[0] for point in points);top=min(point[1] for point in points)
    assert all(point[2:]==(width,height) for point in points), "one image has mixed tile dimensions"
    assert {(x,y) for x,y,_,_ in points} == {(x,y) for y in range(top,top+height) for x in range(left,left+width)}, \
        "browser image tile grid has missing or misplaced cells"
    return dict(x=left,y=top,width=width,height=height)


def expected_size(width,height,chrome):
    cw=chrome["cell"]["width"]//chrome["scale"]
    ch=chrome["cell"]["height"]//chrome["scale"]
    scale=min(1,24*cw/width,8*ch/height)
    return max(1,int(width*scale/cw+.5)),max(1,int(height*scale/ch+.5))


def inspect_images(app,label):
    def ready():
        c=app.chrome()
        if all(image_rect(c,identity) for identity in (1,2)):
            return c
    c=wait_for(ready,description="both native browser image tile grids")
    # Capturing through the app's renderer also waits for its pending frame.
    image=app.snap(label)
    for identity,dimensions in ((1,(384,256)),(2,(12,24))):
        r=image_rect(c,identity)
        assert (r["width"],r["height"]) == expected_size(*dimensions,c), (identity,r,c["cell"])
        cw,ch=c["cell"]["width"],c["cell"]["height"]
        for quadrant,name in enumerate(("red top-left","green top-right","blue bottom-left","yellow bottom-right")):
            x=r["x"]*cw+(r["width"]*cw*(3 if quadrant&1 else 1))//4
            y=r["y"]*ch+(r["height"]*ch*(3 if quadrant&2 else 1))//4
            pixels=image.crop(x-1,y-1,3,3)
            blue,green,red=(sum(pixels[channel::4])/9 for channel in (0,1,2))
            valid = (red>green+45 and red>blue+45) if quadrant==0 else \
                    (green>red+45 and green>blue+45) if quadrant==1 else \
                    (blue>red+45 and blue>green+45) if quadrant==2 else \
                    (red>blue+45 and green>blue+45)
            assert valid, (label,identity,name,(red,green,blue),r)
    return c


def choose_font(app,key=None,different_from=None):
    c=app.settings_section(0)
    candidates=[button for button in c["font_buttons"]
                if (key is None or button["fontkey"]==key)
                and (different_from is None or (button["width"],button["height"])!=different_from)]
    assert candidates, (key,different_from,c["font_buttons"])
    button=min(candidates,key=lambda item:item["width"]*item["height"])
    c=app.click(app.settings_x(c)+button["left"],button["row"])
    assert c["fontkey"] == button["fontkey"]
    app.toggle_settings()
    app.hover(-1,-1)
    return app.chrome()


def test_images(app):
    app.send("cmd resize 1001,700")
    app.send("cmd focus 1")
    images=[]
    for width,height in ((384,256),(12,24)):
        uri="data:image/png;base64,"+base64.b64encode(quadrant_png(width,height)).decode()
        images.append(f'<p><img alt="owned image {width} by {height}" width="{width}" height="{height}" src="{uri}"></p>')
    page=app.root/"owned-quadrant-images.html"
    page.write_text('<!doctype html><meta charset="utf-8"><title>Owned Image Quadrants</title>'
                    '<h1>Owned image quadrants</h1><p>Four colors retain their original orientation.</p>'
                    +"".join(images))
    identity=browser_command(app,"open",str(page),"--mode","hardcore")["id"]
    wait_for(lambda:any(tab["id"]==identity and tab["title"]=="Owned Image Quadrants"
                        for tab in browser_command(app,"list")["tabs"]),description="local image fixture browser")
    c=inspect_images(app,"browser-images-original-size-and-orientation")
    assert c["fontkey"] == "t" and c["cell"] == {"width":12,"height":24}
    assert (image_rect(c,1)["width"],image_rect(c,1)["height"]) == (24,8)
    assert (image_rect(c,2)["width"],image_rect(c,2)["height"]) == (2,2)
    app.send("cmd resize 801,620")
    c=inspect_images(app,"browser-images-window-relayout")
    assert selected(app.state())==identity
    choose_font(app,different_from=(12,24))
    c=inspect_images(app,"browser-images-font-relayout")
    assert c["cell"] != {"width":12,"height":24}
    choose_font(app,key="t")
    c=inspect_images(app,"browser-images-restored-font")
    assert c["cell"] == {"width":12,"height":24} and selected(app.state())==identity
    print("PASS: native image quadrants stay upright across all24 columns; original24×8/2×2 sizes survive window/font relayout")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts",type=Path,default=Path("build/browser-images-native"))
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hc-browser-images-") as root:
        with NativeApp(Path(root),1,args.artifacts.resolve()) as app:
            try:test_images(app)
            except BaseException:
                app.snap("browser-images-failure")
                raise
    print(f"PASS: native browser images; screenshots: {args.artifacts.resolve()}")


if __name__ == "__main__":
    main()
