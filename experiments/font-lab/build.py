#!/usr/bin/env python3
"""Rebuild the Hairline Lab's small, auditable font subsets without dependencies.

    python3 experiments/font-lab/build.py
    python3 experiments/font-lab/build.py --bundle

The JSON contains native ASCII and selected native terminal symbols, plus
explicitly marked basic light-box fallbacks when the source lacks them. Native
glyphs are never rescaled or replaced. The optional bundle is an HTML fragment,
with its data, engine and complete font license notices embedded.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ASCII = set(range(32, 127))
SELECTED = ASCII | set(range(0x2500, 0x2580)) | {
    0x00B0, 0x00B1, 0x00B7, 0x00D7, 0x00F7,
    0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193,
    0x2580, 0x2584, 0x2588, 0x258C, 0x2590, 0x2591, 0x2592, 0x2593,
    0x25B2, 0x25B6, 0x25BC, 0x25C0, 0x25CF, 0x2713, 0x2717,
}
LIGHT_ARMS = {
    0x2500: "lr", 0x2502: "ud",
    0x250C: "dr", 0x2510: "dl", 0x2514: "ur", 0x2518: "ul",
    0x251C: "udr", 0x2524: "udl", 0x252C: "dlr", 0x2534: "ulr",
    0x253C: "udlr", 0x2574: "l", 0x2575: "u", 0x2576: "r", 0x2577: "d",
}

FONT_SPECS = [
    dict(id="terminus24", name="Terminus 12×24", width=12, height=24,
         file="font/terminus/ter-u24n.bdf.gz", stemPixels=1, licenseId="terminus",
         source="https://terminus-font.sourceforge.net/",
         note="Native regular bitmap; one-pixel straight stems and bars. The experiment starts here."),
    dict(id="terminus28", name="Terminus 14×28", width=14, height=28,
         file="font/terminus/ter-u28n.bdf.gz", stemPixels=2, licenseId="terminus",
         source="https://terminus-font.sourceforge.net/",
         note="Native regular bitmap; two-pixel vertical stems, with thinner horizontal bars. Original box glyphs are retained."),
    dict(id="terminus32", name="Terminus 16×32", width=16, height=32,
         file="font/terminus/ter-u32n.bdf.gz", stemPixels=2, licenseId="terminus",
         source="https://terminus-font.sourceforge.net/",
         note="Native regular bitmap; two-pixel stems and bars. Original box glyphs are retained."),
    dict(id="sony24", name="Sony Fixed 12×24", width=12, height=24,
         file="font/sony/12x24.bdf.gz", stemPixels=2, licenseId="sony",
         source="https://gitlab.freedesktop.org/xorg/font/sony-misc",
         note="Native X11 serif bitmap; two-pixel I/H/n stems. Missing basic box glyphs use marked one-pixel lab fallbacks."),
    dict(id="etl24", name="ETL Fixed 12×24", width=12, height=24,
         file="font/etl/lt1-24-etl.bdf.gz", stemPixels=2, licenseId="etl",
         source="https://ftp.gnu.org/gnu/intlfonts/",
         note="Native GNU intlfonts Latin-1 bitmap; two-pixel I/H/n stems. Missing basic box glyphs use marked one-pixel lab fallbacks."),
    dict(id="courier24", name="X11 Courier 15×24", width=15, height=24,
         file="font/courier/courier-75dpi-24.bdf.gz", stemPixels=1, licenseId="adobe",
         source="https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/font-adobe-75dpi/dist/courR24.bdf",
         note="Native 75dpi Courier bitmap; one-pixel I/H/n stems and bars, with serifed shapes. Curved joins can have adjacent pixels. Native box glyphs are retained; six overhanging block/shade symbols are omitted, not clipped."),
    dict(id="courier29", name="X11 Courier 20×29", width=20, height=29,
         file="font/courier/courier-100dpi-24.bdf.gz", stemPixels=1, licenseId="adobe",
         source="https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/font-adobe-100dpi/dist/courR24.bdf",
         note="Native 100dpi Courier bitmap: 20×29 line cell from ascent 23 + descent 6; BDF PIXEL_SIZE is 34. One-pixel straight stems; some curve joins are wider. Native boxes are retained; six overhanging block/shade symbols are omitted, not clipped."),
]
SOURCE_SHA256 = {
    "terminus24": "ff640e9e097983355e8f70ed8fb645bd850184a0d590de17f6fc9ceec1bf8eaf",
    "terminus28": "f9e9da69986002a9a3d0b54f5bb1db487914821204a9a75151260fff45642723",
    "terminus32": "5ad01972d58adfee75077e2c65ba5947e260043031664cb8ad8f98b5473b23a4",
    "sony24": "a0b1263d903b0b9bd5b5c39a8cba077f76b9b0294659b75a8104ec0224c3a354",
    "etl24": "19492eb9bb1c905b637f4ec0019731f9c6e67500be1fd877784405648b804cf3",
    "courier24": "01cef9a0e179b7d23ade8cd111ea3e205ffda8ebf02d5d430652e59d94f85181",
    "courier29": "336ce852c46f0bab88cd90deb249a266e624f975f96454f8f5f9a59beddb7664",
}
LICENSE_SPECS = {
    "terminus": ("SIL Open Font License 1.1", "Terminus-OFL.txt", "https://openfontlicense.org/open-font-license-official-text/"),
    "sony": ("Sony permission notice", "Sony-COPYING.txt", "https://gitlab.freedesktop.org/xorg/font/sony-misc"),
    "etl": ("ETL free-use permission notice", "ETL-COPYING.txt", "https://ftp.gnu.org/gnu/intlfonts/"),
    "adobe": ("Adobe / DEC permission notice", "Adobe-DEC-COPYING.txt", "https://gitlab.freedesktop.org/xorg/font/adobe-75dpi"),
}


def parse_bdf(raw: bytes) -> tuple[dict[str, int], dict[int, dict]]:
    """Read just the BDF metrics and encoded bitmap records needed by this lab."""
    text = raw.decode("latin-1")
    props = {}
    for key in ("FONT_ASCENT", "FONT_DESCENT", "PIXEL_SIZE", "CAP_HEIGHT", "X_HEIGHT"):
        match = re.search(rf"^{key} (-?\d+)\s*$", text, re.MULTILINE)
        if match:
            props[key] = int(match[1])
    if not {"FONT_ASCENT", "FONT_DESCENT"} <= props.keys():
        raise ValueError("BDF lacks baseline metrics")
    records = {}
    for block in text.split("STARTCHAR ")[1:]:
        block = block.split("ENDCHAR", 1)[0]
        match = re.search(r"^ENCODING (-?\d+)\s*$", block, re.MULTILINE)
        if not match or int(match[1]) not in SELECTED:
            continue
        cp = int(match[1])
        if cp in records:
            raise ValueError(f"Duplicate glyph U+{cp:04X}")
        width = re.search(r"^DWIDTH (\d+) 0\s*$", block, re.MULTILINE)
        bbox = re.search(r"^BBX (\d+) (\d+) (-?\d+) (-?\d+)\s*$", block, re.MULTILINE)
        if not width or not bbox or "BITMAP\n" not in block:
            raise ValueError(f"Incomplete glyph U+{cp:04X}")
        bb = tuple(map(int, bbox.groups()))
        bitmap = block.split("BITMAP\n", 1)[1].strip().splitlines()
        if len(bitmap) != bb[1] or any(not re.fullmatch(r"[0-9A-Fa-f]*", row) for row in bitmap):
            raise ValueError(f"Malformed bitmap U+{cp:04X}")
        records[cp] = dict(advance=int(width[1]), bbox=bb, bitmap=bitmap)
    return props, records


def cell_rows(record: dict, width: int, height: int, ascent: int) -> list[int]:
    """Place original pixels by BDF baseline; refuse any clipping or scaling."""
    if record["advance"] != width:
        raise ValueError("Glyph advance differs from the monospaced cell")
    bw, bh, bx, by = record["bbox"]
    rows = [0] * height
    for y, line in enumerate(record["bitmap"]):
        if len(line) * 4 < bw:
            raise ValueError("BDF row is narrower than BBX")
        value = int(line, 16) if line else 0
        yy = ascent - (by + bh) + y
        for x in range(bw):
            if value & (1 << (len(line) * 4 - 1 - x)):
                xx = bx + x
                if not (0 <= xx < width and 0 <= yy < height):
                    raise ValueError(f"Lit pixel ({xx}, {yy}) lies outside {width}×{height}")
                rows[yy] |= 1 << (width - 1 - xx)
    return rows


def light_box(cp: int, width: int, height: int) -> list[int]:
    """Explicit lab fallback: one-pixel light lines, on integer cell centers."""
    arms = LIGHT_ARMS[cp]
    cx, cy = (width - 1) // 2, (height - 1) // 2
    rows = [0] * height
    bit = 1 << (width - 1 - cx)
    if "u" in arms:
        for y in range(cy + 1):
            rows[y] |= bit
    if "d" in arms:
        for y in range(cy, height):
            rows[y] |= bit
    if "l" in arms:
        for x in range(cx + 1):
            rows[cy] |= 1 << (width - 1 - x)
    if "r" in arms:
        for x in range(cx, width):
            rows[cy] |= 1 << (width - 1 - x)
    return rows


def ink_top(rows: list[int]) -> int:
    return next(i for i, row in enumerate(rows) if row)


def build_data() -> dict:
    licenses = {
        key: dict(name=name, text=(HERE / "LICENSES" / filename).read_text(), source=url)
        for key, (name, filename, url) in LICENSE_SPECS.items()
    }
    fonts = []
    for spec in FONT_SPECS:
        raw = gzip.decompress((ROOT / spec["file"]).read_bytes())
        source_hash = hashlib.sha256(raw).hexdigest()
        if source_hash != SOURCE_SHA256[spec["id"]]:
            raise ValueError(f"Source changed for {spec['id']}; inspect the new BDF before updating its pinned SHA256")
        props, records = parse_bdf(raw)
        width, height, ascent = spec["width"], spec["height"], props["FONT_ASCENT"]
        if props["FONT_ASCENT"] + props["FONT_DESCENT"] != height:
            raise ValueError(f"Unexpected line-cell metrics for {spec['id']}")
        glyphs, omitted = {}, []
        for cp, record in sorted(records.items()):
            try:
                glyphs[str(cp)] = cell_rows(record, width, height, ascent)
            except ValueError as exc:
                if cp in ASCII or cp in LIGHT_ARMS:
                    raise ValueError(f"{spec['id']} U+{cp:04X}: {exc}") from exc
                # An extended Courier glyph may overhang its nominal line cell.
                # Omit and report it rather than clip or alter the source bitmap.
                omitted.append(dict(codepoint=cp, reason=str(exc)))
        missing_ascii = ASCII - {int(cp) for cp in glyphs}
        if missing_ascii:
            raise ValueError(f"{spec['id']} lacks ASCII {sorted(missing_ascii)}")
        synthetic = []
        for cp in LIGHT_ARMS:
            if str(cp) not in glyphs:
                glyphs[str(cp)] = light_box(cp, width, height)
                synthetic.append(cp)
        font = {key: value for key, value in spec.items() if key != "file"}
        font.update(
            ascent=ascent, descent=props["FONT_DESCENT"],
            glyphs=dict(sorted(glyphs.items(), key=lambda item: int(item[0]))),
            sourceFile=spec["file"], sourceSHA256=source_hash,
            license=licenses[spec["licenseId"]]["name"],
            syntheticCodepoints=sorted(synthetic), omittedNativeGlyphs=omitted,
            metrics=dict(baseline=ascent, capTop=ink_top(glyphs["72"]),
                         xTop=ink_top(glyphs["120"]), pixelSize=props.get("PIXEL_SIZE")),
        )
        fonts.append(font)
    return dict(
        version=1, fonts=fonts, licenses=licenses,
        scope="Original ASCII, available box drawing and selected symbols. Missing basic light boxes are explicitly marked lab fallbacks. No resampling or silent clipping.",
        bitOrder="Most significant cell bit is leftmost; bit (width - 1 - x).",
    )


def bundle(data: dict, destination: Path) -> None:
    fragment = (HERE / "lab.fragment.html").read_text()
    engine = (HERE / "variant-engine.js").read_text()
    for token in ("<!--FONTLAB_DATA-->", "<!--FONTLAB_ENGINE-->"):
        if fragment.count(token) != 1:
            raise ValueError(f"Expected exactly one {token} placeholder")
    compact = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")
    notices = "\n\n".join(license["name"] + "\n" + license["text"] for license in data["licenses"].values())
    notices = notices.replace("--", "- -")
    output = "<!-- Hairline Lab font copyright and permission notices\n" + notices + "\n-->\n"
    output += fragment.replace("<!--FONTLAB_DATA-->", "<script>globalThis.fontLabData=" + compact + ";</script>")
    output = output.replace("<!--FONTLAB_ENGINE-->", "<script>\n" + engine.replace("</script", "<\\/script") + "\n</script>")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(output, encoding="utf-8")
    if destination.stat().st_size >= 1_000_000:
        raise ValueError("Font lab bundle exceeds the one-megabyte budget")
    print(f"{destination.relative_to(ROOT) if destination.is_relative_to(ROOT) else destination}: {destination.stat().st_size:,} bytes")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", nargs="?", const=str(HERE / "hairline-font-lab.html"), metavar="OUTPUT", help="also generate a self-contained HTML fragment")
    args = parser.parse_args()
    data = build_data()
    destination = HERE / "assets" / "font-data.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for font in data["fonts"]:
        print(f"{font['id']}: {len(font['glyphs'])} glyphs, {len(font['syntheticCodepoints'])} marked box fallbacks, {len(font['omittedNativeGlyphs'])} reported overhang omissions; ASCII exact")
    if args.bundle:
        bundle(data, Path(args.bundle).resolve())


if __name__ == "__main__":
    main()
