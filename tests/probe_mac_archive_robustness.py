#!/usr/bin/env python3
"""Seeded offline styled-archive robustness probe. Never reads the clipboard.

Compiles the native helper once, then decodes synthetic files with bounded
subprocess timeouts. --capture optionally replays a known saved manual fixture
and mutates private copies; the source capture is never changed.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import random
import stat
import subprocess
import sys
import tempfile
import time
import unicodedata

ROOT = Path(__file__).resolve().parents[1]
MAX_BYTES = 8 * 1024 * 1024
MAX_UNITS = 1024 * 1024
TOKEN = "a17c401b2c3d4e5f60718293a4b5c6d7"
TAIL = "HC_STYLE_TAIL"


def units(value: str) -> int:
    return len(value.encode("utf-16-le")) // 2


def rtf_text(value: str) -> str:
    if value.isascii() and all(c in "\n\t" or ord(c) >= 32 for c in value):
        return value.replace("\\", "\\\\").replace("{", "\\{").replace("}", "\\}").replace("\n", "\\par ").replace("\t", "\\tab ")
    output = []
    data = value.encode("utf-16-le")
    for offset in range(0, len(data), 2):
        unit = int.from_bytes(data[offset:offset + 2], "little")
        if unit == 10:
            output.append(r"\par ")
        elif unit in (92, 123, 125):
            output.append("\\" + chr(unit))
        elif 32 <= unit <= 126:
            output.append(chr(unit))
        else:
            output.append("\\u" + str(unit if unit < 32768 else unit - 65536) + "?")
    return "".join(output)


def rich_text(value: str, colored_tail: bool = True) -> bytes:
    body = rtf_text(value)
    if colored_tail:
        assert value.count(TAIL) == 1
        body = body.replace(TAIL, r"\cf1 " + TAIL + r"\cf0 ")
    return (r"{\rtf1\ansi\uc1{\colortbl;\red255\green0\blue0;}\pard " + body + "}").encode()


def framed(body: str, token: str = TOKEN, tail: bool = True) -> str:
    return f"HCTERM-FIXTURE:{token}:BEGIN\n{body}\n" + (TAIL + "\n" if tail else "") + f"HCTERM-FIXTURE:{token}:END\n"


def pair(case_id: str, plain_body: str, rtf_body: str, *, kind="canonical", accept=True,
         diagnostics=True) -> dict:
    plain, rich = framed(plain_body), framed(rtf_body)
    return {"id": case_id, "kind": kind, "plain": plain.encode(), "rtf": rich_text(rich),
            "expected": plain, "accept": accept, "diagnostics": diagnostics,
            "representationsExact": plain == rich, "coloredTail": True, "token": TOKEN}


def corpus(seed: int) -> list[dict]:
    rng = random.Random(seed)
    cases = [pair("literal-ascii", "Literal {braces} \\pict and text", "Literal {braces} \\pict and text", kind="literal")]
    examples = ["é", "Å", "ǻ", "ấ", "ΐ", "ṩ", "가", "각", "한", "ᾄ", "Ḋ", "Å", "\U0001d15e"]
    for index, source in enumerate(examples):
        decomposed = unicodedata.normalize("NFD", source)
        assert source != decomposed
        cases.append(pair(f"canonical-{index:02d}-decomposed-plain", decomposed, source))
        cases.append(pair(f"canonical-{index:02d}-composed-plain", source, decomposed))
    cases.extend([
        pair("reordered-combining", "a\u0315\u0300", "à\u0315"),
        pair("emoji-zwj-unchanged", "e\u0301 👩🏽‍💻 👨‍👩‍👧‍👦 🇺🇸", "é 👩🏽‍💻 👨‍👩‍👧‍👦 🇺🇸"),
        pair("supplementary-unchanged", "e\u0301 🚀 𝔘 𐐀", "é 🚀 𝔘 𐐀"),
    ])
    for index in range(8):
        chosen = rng.choices(examples + ["x\u0301", "🚀", "👩‍💻", "界"], k=15)
        rich = " | ".join(chosen)
        plain = " | ".join(unicodedata.normalize("NFD", x) for x in chosen)
        cases.append(pair(f"seeded-mixture-{index:02d}", plain, rich))
    for index, (plain, rich) in enumerate([("ff", "ﬀ"), ("1", "①"), ("Ａ", "A"),
                                         ("changed text", "original text"), ("red\r\nnext", "red\nnext"),
                                         ("red\u2028next", "red\nnext")]):
        cases.append(pair(f"refuse-semantic-{index:02d}", plain, rich, kind="refusal", accept=False))
    valid = pair("template", "literal", "literal", kind="literal")
    bad_payloads = {
        "missing-close-brace": valid["rtf"][:-1],
        "missing-open-brace": valid["rtf"][1:],
        "not-rtf": b"<html>not RTF</html>",
        "deep-groups": b"{\\rtf1 " + b"{" * 129 + b"text" + b"}" * 130,
        "embedded-object": b"{\\rtf1 {\\object\\objdata secret}}",
        "embedded-image": b"{\\rtf1 {\\pict image}}",
        "binary-payload": b"{\\rtf1 \\bin4 abcd}",
        "unpaired-close-brace": valid["rtf"] + b"}",
    }
    for name, data in bad_payloads.items():
        cases.append({**valid, "id": "malformed-" + name, "kind": "malformed",
                      "rtf": data, "accept": False, "diagnostics": False})
    cases.append({**valid, "id": "invalid-utf8", "kind": "malformed", "plain": valid["plain"].replace(b"literal", b"\xff"),
                  "accept": False, "diagnostics": False})
    # Exactly the accepted character boundary, plus refusal immediately above it.
    overhead = len(framed("", tail=False))
    boundary = framed("x" * (MAX_UNITS - overhead), tail=False)
    cases.append({**valid, "id": "stress-max-text", "kind": "stress", "plain": boundary.encode(),
                  "rtf": rich_text(boundary, False), "expected": boundary, "coloredTail": False})
    cases.append({**valid, "id": "refuse-max-text-plus-one", "kind": "bounds", "plain": (boundary + "x").encode(),
                  "accept": False, "diagnostics": False})
    cases.append({**valid, "id": "refuse-max-bytes-plus-one", "kind": "bounds", "rtf": b"{" + b" " * MAX_BYTES,
                  "accept": False, "diagnostics": False})
    cases.append({**valid, "id": "refuse-max-plain-bytes-plus-one", "kind": "bounds", "plain": b"x" * (MAX_BYTES + 1),
                  "accept": False, "diagnostics": False})
    mixed = ("café é 가 👩‍💻 repeated row\n" * 4000).rstrip("\n")
    cases.append(pair("stress-canonical-scrollback", unicodedata.normalize("NFD", mixed), mixed, kind="stress"))
    # A sparse canonical difference must not shift a style far later in an
    # otherwise ASCII capture. Record timing, without assuming machine speed.
    sparse = "é near start\n" + ("mostly ASCII scrollback row 0123456789\n" * 2700) + "near end é"
    cases.append(pair("stress-sparse-canonical-ascii", unicodedata.normalize("NFD", sparse), sparse, kind="stress"))
    many = framed("x" * 100010, tail=False)
    styled = rtf_text(many).replace("x" * 100010, (r"\b x\b0 x" * 50005))
    cases.append({**valid, "id": "refuse-too-many-style-runs", "kind": "bounds", "plain": many.encode(),
                  "rtf": (r"{\rtf1\ansi\uc1 " + styled + "}").encode(), "expected": many,
                  "accept": False, "diagnostics": True, "coloredTail": False})
    return cases


def private_bytes(path: Path, value: bytes) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(value)


def require(condition: bool, description: str) -> None:
    if not condition:
        raise AssertionError(description)


def fixture_text(text: str, token: str) -> tuple[str, int, int]:
    begin, end = f"HCTERM-FIXTURE:{token}:BEGIN", f"HCTERM-FIXTURE:{token}:END"
    require(text.count(begin) == text.count(end) == 1, "fixture markers were not unique")
    start, end_at = text.index(begin), text.index(end)
    finish = end_at + len(end)
    require(start + len(begin) < end_at, "fixture markers were out of order")
    boundaries = "\r\n\u2028\u2029"
    for at in (start, end_at):
        require(at == 0 or text[at - 1] in boundaries, "fixture marker was not at a line boundary")
    for at in (start + len(begin), finish):
        require(at == len(text) or text[at] in boundaries, "fixture marker did not end at a line boundary")
    if text[finish:finish + 2] == "\r\n":
        finish += 2
    elif finish < len(text) and text[finish] in boundaries:
        finish += 1
    return text[start:finish], start, finish


def verify_archive(archive: dict, case: dict, decoded_rtf: str) -> None:
    text = archive["text"]
    require(text == case["expected"], "output text did not preserve exact expected UTF-16")
    require(archive.get("fullTerminalState") is False, "archive claimed full terminal state")
    mapping = archive["styleMapping"]
    require(mapping.get("verified") is True and mapping.get("plainTextPreserved") is True,
            "mapping was not verified or plain text was altered")
    rich_fixture, rich_start, _ = fixture_text(decoded_rtf, case["token"])
    actual_exact = rich_fixture == case["expected"]
    expected_exact = case["representationsExact"]
    if expected_exact is None:
        expected_exact = actual_exact
    require(actual_exact == expected_exact, "decoded RTF did not have the expected literal comparison")
    require(archive["comparison"]["fixture"]["exact"] is expected_exact,
            "literal comparison metadata was incorrect")
    require(archive["rtfOriginalRange"] == {"start": units(decoded_rtf[:rich_start]), "length": units(rich_fixture)},
            "RTF fixture range did not match independently cropped text")
    require(mapping["method"] == ("literal" if expected_exact else "canonical-grapheme"), "mapping method was incorrect")
    require((mapping["remappedClusters"] == 0) == expected_exact, "remapped cluster count contradicted literal comparison")
    at = 0
    text_units = units(text)
    for run in archive["runs"]:
        require(type(run["start"]) is int and type(run["length"]) is int, "run offsets are not integers")
        require(run["start"] == at and run["length"] > 0, "style ranges overlap or have a gap")
        at += run["length"]
        require(at <= text_units, "style range exceeds text")
    require(at == text_units, "style ranges do not cover the complete text")
    if case.get("coloredTail"):
        tail_at = units(text[:text.index(TAIL)])
        run = next(run for run in archive["runs"] if run["start"] <= tail_at < run["start"] + run["length"])
        require(run["start"] == tail_at and run["length"] == len(TAIL), "later style offset shifted incorrectly")
        color = run.get("foreground", [])
        require(len(color) == 4 and color[0] > 0.9 and color[1] < 0.3 and color[2] < 0.1,
                "later style lost its foreground color")


def run_case(binary: Path, root: Path, case: dict, seed: int) -> dict:
    start = time.monotonic()
    directory = root / case["id"]
    directory.mkdir(mode=0o700)
    private_bytes(directory / "input.rtf", case["rtf"])
    private_bytes(directory / "input.txt", case["plain"])
    result = {"id": case["id"], "kind": case["kind"], "seed": seed,
              "expected": "accepted" if case["accept"] else "refused",
              "inputBytes": {"rtf": len(case["rtf"]), "plain": len(case["plain"])},
              "decodeTimeoutSeconds": 10}
    target = directory / "capture"
    try:
        decode_start = time.monotonic()
        process = subprocess.run([str(binary), "--decode", str(directory / "input.rtf"),
                                  str(directory / "input.txt"), case["token"], str(target)],
                                 capture_output=True, text=True, timeout=10)
        result["decodeSeconds"] = round(time.monotonic() - decode_start, 4)
        result["returncode"] = process.returncode
        require(process.returncode >= 0, "decoder terminated by a signal")
        if case["accept"]:
            require(process.returncode == 0, "decoder refused an eligible input: " + process.stderr.strip()[:500])
            verify_archive(json.loads((target / "archive.json").read_text()), case,
                           (target / "decoded-rtf.txt").read_bytes().decode("utf-8"))
        else:
            require(process.returncode != 0, "decoder accepted an input that must be refused")
            require(not (target / "archive.json").exists(), "refused input produced a normalized archive")
            if case.get("diagnostics"):
                require((target / "comparison.json").is_file(), "identified mismatch lost comparison diagnostics")
        if target.exists():
            require(stat.S_IMODE(target.stat().st_mode) == 0o700, "capture directory was not private")
            for path in target.iterdir():
                require(stat.S_IMODE(path.stat().st_mode) == 0o600, "capture artifact was not private")
            require((target / "original.rtf").read_bytes() == case["rtf"], "raw RTF changed")
            require((target / "original.txt").read_bytes() == case["plain"], "raw plain text changed")
        result["status"] = "passed"
    except (AssertionError, OSError, ValueError, KeyError, StopIteration, subprocess.TimeoutExpired) as error:
        result["status"] = "failed"
        result["error"] = str(error)[:1000] or type(error).__name__
    result["elapsedSeconds"] = round(time.monotonic() - start, 4)
    return result


def load_manual():
    spec = importlib.util.spec_from_file_location("hardcore_manual_archive", ROOT / "tools/manual_mac_terminal_archive.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def capture_cases(path: Path):
    manual = load_manual()
    _, token, expected, target, rtf, raw = manual.saved_capture(path)
    text = raw.decode("utf-8")
    selected, start, finish = fixture_text(text, token)
    require(selected == expected, "saved plaintext fixture did not match its manifest")
    base = {"id": "capture-replay", "kind": "saved-capture", "token": token, "plain": raw, "rtf": rtf,
            "expected": expected, "accept": True, "diagnostics": True,
            # Determine literal equality from the new decoded RTF evidence,
            # without requiring or trusting an older comparison report.
            "representationsExact": None, "coloredTail": False}
    body = text[start:finish]
    changed_word = body.replace("ANSI red", "ANSI ", 1)
    require(changed_word != body, "saved fixture lacked expected mutation target")
    changed_newline = body.replace("\nHistory row 05:", "\r\nHistory row 05:", 1)
    require(changed_newline != body, "saved fixture lacked newline mutation target")
    changed_token = body.replace(f"HCTERM-FIXTURE:{token}:BEGIN", f"HCTERM-FIXTURE:{'0' if token[0] != '0' else '1'}{token[1:]}:BEGIN", 1)
    cases = [base]
    for name, value, diagnostic in [("drop-word", changed_word, True), ("change-newline", changed_newline, True),
                                    ("change-token", changed_token, False)]:
        modified = (text[:start] + value + text[finish:]).encode()
        cases.append({**base, "id": "capture-refuse-" + name, "plain": modified,
                      "accept": False, "diagnostics": diagnostic})
    before = {name: hashlib.sha256(manual.read_private_bytes(target / name, MAX_BYTES)).hexdigest()
              for name in ("original.rtf", "original.txt")}
    return cases, target, before


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--output-dir", type=Path, help="new private result directory; refuses an existing path")
    parser.add_argument("--seed", type=int, default=20261015)
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("native AppKit decoding requires macOS")
    os.umask(0o077)
    if args.output_dir:
        output = args.output_dir.expanduser().absolute()
        output.mkdir(mode=0o700)
    else:
        output = Path(tempfile.mkdtemp(prefix="hc-archive-robustness-", dir="/private/tmp"))
    started = time.monotonic()
    results = []
    report = {"format": "hardcore-archive-robustness-v1", "seed": args.seed,
              "unicodeDataVersion": unicodedata.unidata_version, "clipboardAccess": False,
              "fullTerminalState": False, "outputDirectory": str(output)}
    try:
        binary = output / "archive-helper"
        report["helperSourceSHA256"] = hashlib.sha256((ROOT / "tools/mac_terminal_archive.m").read_bytes()).hexdigest()
        command = ["clang", "-O2", "-Wall", "-Wextra", "-Werror", "-fobjc-arc", "-framework", "Foundation",
                   "-framework", "AppKit", str(ROOT / "tools/mac_terminal_archive.m"), "-o", str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=30)
        require(compiled.returncode == 0, "native compile failed: " + compiled.stderr[:1000])
        native = subprocess.run([str(binary), "--selftest"], capture_output=True, text=True, timeout=5)
        require(native.returncode == 0, "native selftest failed: " + native.stderr[:1000])
        report["nativeSelftest"] = native.stdout.strip()
        cases = corpus(args.seed)
        saved = None
        if args.capture:
            extras, target, before = capture_cases(args.capture)
            cases.extend(extras)
            saved = target, before
            report["savedCapture"] = str(target)
        for case in cases:
            result = run_case(binary, output, case, args.seed)
            results.append(result)
            if result["status"] == "failed":
                print(f"FAIL {result['id']}: {result['error']}", flush=True)
        if saved:
            target, before = saved
            manual = load_manual()
            after = {name: hashlib.sha256(manual.read_private_bytes(target / name, MAX_BYTES)).hexdigest()
                     for name in before}
            results.append({"id": "capture-originals-unchanged", "kind": "saved-capture", "seed": args.seed,
                            "status": "passed" if before == after else "failed", "elapsedSeconds": 0})
            report["sourceOriginalHashes"] = after
    except (AssertionError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        results.append({"id": "probe-setup", "kind": "setup", "seed": args.seed, "status": "failed",
                        "error": str(error)[:1000], "elapsedSeconds": round(time.monotonic() - started, 4)})
    failed = [case["id"] for case in results if case["status"] != "passed"]
    report.update(status="failed" if failed else "passed", counts={"total": len(results), "passed": len(results) - len(failed), "failed": len(failed)},
                  failedCaseIDs=failed, elapsedSeconds=round(time.monotonic() - started, 4), cases=results)
    private_bytes(output / "report.json", (json.dumps(report, ensure_ascii=True, indent=2) + "\n").encode())
    print(f"Archive robustness: {report['status']}; {report['counts']['passed']}/{len(results)} passed. Report: {output / 'report.json'}")
    return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
