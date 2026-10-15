#!/usr/bin/env python3
"""User-operated styled Terminal history probe; never automates Terminal.

Run in a fresh Terminal window. Only pressing Return after the copy step calls
the helper's explicit --capture mode. --prepare-only, --decode-capture, and
automated tests never read the clipboard. Generated artifacts remain private.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]


def native_bool(value):
    """AppKit NSNumber BOOL values may arrive as JSON booleans or integer 0/1."""
    if type(value) is bool:
        return value
    if type(value) is int and value in (0, 1):
        return value == 1
    return None


def mapping_verified(value) -> bool:
    if not isinstance(value, dict):
        return False
    count = value.get("remappedClusters")
    if (native_bool(value.get("verified")) is not True or
            native_bool(value.get("plainTextPreserved")) is not True or
            type(count) is not int or not 0 <= count <= 1024 * 1024):
        return False
    return ((value.get("method") == "literal" and count == 0) or
            (value.get("method") == "canonical-grapheme" and count > 0))


def fixture(token: str) -> tuple[str, str]:
    """Return the output bytes as text and their expected plain-text content."""
    pairs = [(f"HCTERM-FIXTURE:{token}:BEGIN", f"HCTERM-FIXTURE:{token}:BEGIN")]
    samples = [
        ("\x1b[31mANSI red\x1b[0m", "ANSI red"),
        ("\x1b[44;97mBlue background, white foreground\x1b[0m", "Blue background, white foreground"),
        ("\x1b[1mBold\x1b[0m | \x1b[3mItalic\x1b[0m | \x1b[4mUnderline\x1b[0m", "Bold | Italic | Underline"),
        ("\x1b[38;2;17;153;85mRGB 17/153/85\x1b[0m", "RGB 17/153/85"),
        ("\x1b[7mReverse video\x1b[0m", "Reverse video"),
        ("Unicode: café e\u0301 🚀 界", "Unicode: café e\u0301 🚀 界"),
        ("\x1b]8;;https://example.com/hardcore-fixture\x1b\\fixture link\x1b]8;;\x1b\\", "fixture link"),
        ("Colored blanks:[\x1b[42m     \x1b[0m]", "Colored blanks:[     ]"),
        ("Wrap:" + "0123456789" * 18 + ":ENDWRAP", "Wrap:" + "0123456789" * 18 + ":ENDWRAP"),
    ]
    pairs.extend(samples)
    for line in range(1, 65):
        text = f"History row {line:02d}: styled capture fixture"
        pairs.append((f"\x1b[3{line % 7 + 1}m{text}\x1b[0m", text))
    pairs.append((f"HCTERM-FIXTURE:{token}:END", f"HCTERM-FIXTURE:{token}:END"))
    return "\n".join(x[0] for x in pairs) + "\n", "\n".join(x[1] for x in pairs) + "\n"


def evaluate(archive: dict, expected: str, token: str) -> dict:
    """Measure a captured fixture without claiming complete emulator state."""
    text = archive["text"]
    begin = f"HCTERM-FIXTURE:{token}:BEGIN"
    end = f"HCTERM-FIXTURE:{token}:END"
    first = text.index(begin)
    last = text.index(end, first) + len(end)
    actual = text[first:last]

    def attributes(label: str, offset: int = 0) -> dict:
        index = text.find(label)
        if index < 0:
            return {}
        unit = len(text[:index + offset].encode("utf-16-le")) // 2
        return next((run for run in archive.get("runs", [])
                     if run["start"] <= unit < run["start"] + run["length"]), {})

    color = attributes("ANSI red")
    background = attributes("Blue background")
    link = attributes("fixture link").get("link", {})
    styles = {
        "ansiForegroundPresent": "foreground" in color,
        "backgroundPresent": "background" in background,
        "boldPresent": bool(attributes("Bold").get("font", {}).get("bold")),
        "italicPresent": bool(attributes("Italic").get("font", {}).get("italic")),
        "underlinePresent": bool(attributes("Underline").get("underline")),
        "blankBackgroundPresent": "background" in attributes("Colored blanks:[", len("Colored blanks:[")),
        "hyperlinkExact": link.get("text") == "https://example.com/hardcore-fixture" and link.get("actionable") is False,
        "truecolorResolvedSRGB": attributes("RGB 17/153/85").get("foreground"),
    }
    return {
        "fixtureToken": token,
        # The decoder already cropped to the unique marker range, including
        # its trailing newline when present. Losing that newline is text loss.
        "fixtureTextExact": text == expected,
        "wholeSelectionTextExact": native_bool(archive.get("comparison", {}).get("wholeSelection", {}).get("exact")),
        "fixtureRepresentationsExact": native_bool(archive.get("comparison", {}).get("fixture", {}).get("exact")),
        "styleMapping": archive.get("styleMapping"),
        "styleMappingVerified": mapping_verified(archive.get("styleMapping")),
        "observedAttributes": archive.get("observedAttributes", []),
        "unrepresentedAttributes": archive.get("unrepresentedAttributes", []),
        "sanitizedCodeUnits": archive.get("sanitizedCodeUnits", 0),
        "historyRowsPresent": sum(f"History row {n:02d}:" in actual for n in range(1, 65)),
        "unicodePresent": "Unicode: café e\u0301 🚀 界" in actual,
        "fixtureStyles": styles,
        "fullTerminalState": False,
        "note": "Attribute presence is measured, not proof of exact color or emulator-state preservation.",
    }


def private_write(path: Path, text: str) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        stream.write(text)


def read_private_bytes(path: Path, limit: int) -> bytes:
    """Read an owned bounded regular file without following a final symlink."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError(f"Expected a private regular file: {path.name}")
        data = stream.read(limit + 1)
        if len(data) > limit:
            raise ValueError(f"Saved file exceeds its size limit: {path.name}")
        return data


def read_private(path: Path, limit: int) -> str:
    return read_private_bytes(path, limit).decode("utf-8")


def private_directory(value: Path) -> Path:
    supplied = value.expanduser()
    if supplied.is_symlink():
        raise ValueError("The saved fixture directory must not be a symlink.")
    directory = supplied.resolve(strict=True)
    info = directory.stat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise ValueError("The saved fixture must be an owned private directory (0700).")
    return directory


def saved_fixture(value: Path) -> tuple[Path, str, str]:
    run = private_directory(value)
    manifest = json.loads(read_private(run / "manifest.json", 64 * 1024))
    if not isinstance(manifest, dict):
        raise ValueError("The saved fixture manifest must be a JSON object.")
    token = manifest.get("fixtureToken")
    if (not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{32}", token)
            or manifest.get("fullTerminalState") is not False):
        raise ValueError("The saved fixture manifest has an invalid identity.")
    expected = read_private(run / "fixture.txt", 1024 * 1024)
    # Only this known disposable fixture is eligible. Reuse its actual saved
    # expected text, while refusing edited markers, truncated rows or commands.
    if expected != fixture(token)[1]:
        raise ValueError("The saved fixture text does not match this fixture version. Start a fresh run.")
    return run, token, expected


def saved_capture(value: Path):
    """Accept only original or recorded attempt captures of a known fixture."""
    target = private_directory(value)
    if target.name != "capture":
        raise ValueError("Use RUN/capture or RUN/attempt-*/capture from this fixture.")
    parent = private_directory(target.parent)
    if (parent / "manifest.json").exists():
        run, token, expected = saved_fixture(parent)
    else:
        metadata = json.loads(read_private(parent / "attempt.json", 64 * 1024))
        if not isinstance(metadata, dict) or metadata.get("fullTerminalState") is not False:
            raise ValueError("The saved capture attempt has an invalid manifest.")
        origin = metadata.get("sourceRun", metadata.get("recaptureOf"))
        if not isinstance(origin, str) or not Path(origin).is_absolute():
            raise ValueError("The saved capture attempt has no absolute source run.")
        run, token, expected = saved_fixture(Path(origin))
        if parent.parent != run or not parent.name.startswith("attempt-") or metadata.get("fixtureToken") != token:
            raise ValueError("The saved capture attempt does not belong to its source fixture.")
    rtf = read_private_bytes(target / "original.rtf", 8 * 1024 * 1024)
    plain = read_private_bytes(target / "original.txt", 8 * 1024 * 1024)
    return run, token, expected, target, rtf, plain


def compile_helper(directory: Path) -> Path:
    binary = directory / "archive-helper"
    subprocess.run(["clang", "-O2", "-Wall", "-Wextra", "-fobjc-arc", "-framework", "Foundation",
                    "-framework", "AppKit", str(ROOT / "tools/mac_terminal_archive.m"), "-o", str(binary)],
                   check=True, stdin=subprocess.DEVNULL, timeout=120)
    subprocess.run([str(binary), "--selftest"], check=True, stdin=subprocess.DEVNULL, timeout=15)
    return binary


def run_archive(arguments: list[str], attempt: Path, operation: str):
    """Bound native decoding/provider waits; run() kills/reaps timed-out children."""
    try:
        return subprocess.run(arguments, stdin=subprocess.DEVNULL, timeout=30)
    except subprocess.TimeoutExpired:
        private_write(attempt / "helper-timeout.json", json.dumps({"operation": operation,
            "status": "timed_out", "timeoutSeconds": 30, "fullTerminalState": False}, indent=2) + "\n")
        print(f"{operation} timed out after 30 seconds; this attempt is not verified.")
        return subprocess.CompletedProcess(arguments, 124)


def comparison_summary(comparison: dict) -> list[str]:
    """Summarize bounded metadata without displaying copied Terminal text."""
    if not isinstance(comparison, dict):
        return []
    lines = []
    for field, label in (("wholeSelection", "Whole copied selection"), ("fixture", "Fixture only")):
        value = comparison.get(field)
        if not isinstance(value, dict):
            continue
        exact = native_bool(value.get("exact"))
        if exact is not None:
            lines.append(f"{label}: RTF/plain text {'match' if exact else 'differ'}.")
        lengths = [value.get(key) for key in ("plainUTF16Length", "rtfUTF16Length")]
        if all(type(number) is int and 0 <= number <= 8 * 1024 * 1024 for number in lengths):
            lines.append(f"  Lengths (UTF-16 units): plain {lengths[0]}, RTF {lengths[1]}.")
        mismatch = value.get("firstMismatchUTF16")
        if type(mismatch) is int and 0 <= mismatch <= 8 * 1024 * 1024:
            lines.append(f"  First difference at UTF-16 unit {mismatch}.")
    fixture_info = comparison.get("fixture")
    equivalence = fixture_info.get("diagnosticEquivalence") if isinstance(fixture_info, dict) else None
    if isinstance(equivalence, dict) and native_bool(fixture_info.get("exact")) is False:
        reasons = []
        if native_bool(equivalence.get("lineEndingsOnly")) is True:
            reasons.append("line endings")
        if native_bool(equivalence.get("unicodeNormalizationOnly")) is True:
            reasons.append("Unicode normalization")
        if not reasons and native_bool(equivalence.get("lineEndingsAndUnicodeNormalization")) is True:
            reasons.append("line endings plus Unicode normalization")
        if reasons:
            lines.append("Diagnostic clue: " + ", ".join(reasons) + ". This does not count as an exact match.")
    return lines


def capture(binary: Path, token: str, expected: str, attempt: Path, source: Path) -> int:
    """Only call after the user explicitly confirms the manual copy step."""
    target = attempt / "capture"
    input("After copying the fixture, press Return to read that clipboard selection, or Control-C to cancel: ")
    result = run_archive([str(binary), "--capture", token, str(target)], attempt, "Capture")
    return finish(result, token, expected, attempt, source)


def finish(result, token: str, expected: str, attempt: Path, source: Path, operation="Capture") -> int:
    target = attempt / "capture"
    comparison_path = target / "comparison.json"
    comparison = None
    if comparison_path.exists():
        try:
            comparison = json.loads(read_private(comparison_path, 1024 * 1024))
        except (OSError, ValueError):
            print("The saved comparison could not be read; inspect the private diagnostic directory.")
        else:
            for line in comparison_summary(comparison):
                print(line)
    if result.returncode:
        print(f"{operation} was not verified (exit {result.returncode}); no normalized archive was accepted.")
        if comparison_path.exists():
            print(f"Comparison: {comparison_path}")
            print(f"Raw copied text, original RTF, and decoded RTF text are retained privately in: {target}")
        print("Leave Terminal open. You can select the same prior fixture and retry with:")
        print(shlex.join([sys.executable, str(Path(__file__).resolve()), "--recapture", str(source)]))
        print("Or start a new run in a fresh Terminal window:")
        print(shlex.join([sys.executable, str(Path(__file__).resolve())]))
        return result.returncode
    archive = json.loads((target / "archive.json").read_text())
    report = evaluate(archive, expected, token)
    private_write(attempt / "coverage.json", json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    mapping = report["styleMapping"] if isinstance(report["styleMapping"], dict) else {}
    consistent_mapping = ((mapping.get("method") == "literal" and report["fixtureRepresentationsExact"] is True) or
                          (mapping.get("method") == "canonical-grapheme" and report["fixtureRepresentationsExact"] is False))
    verified = report["fixtureTextExact"] and report["styleMappingVerified"] and consistent_mapping
    if not verified:
        print("Archive saved, but fixture verification was not an exact pass.")
    elif report["fixtureRepresentationsExact"] is False:
        print("Exact plain text preserved with verified canonical grapheme style mapping; raw RTF/plain strings still differ.")
    print(f"Leave Terminal open. Report: {attempt / 'coverage.json'}")
    return 0 if verified else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--prepare-only", action="store_true", help="compile and save fixture without printing it or reading clipboard")
    mode.add_argument("--recapture", type=Path, metavar="RUN_DIRECTORY",
                      help="manually copy an existing fixture again; compile the current helper and save a new unique attempt")
    mode.add_argument("--decode-capture", type=Path, metavar="CAPTURE_DIRECTORY",
                      help="offline: decode saved RUN/capture or RUN/attempt-*/capture with the current helper into a new attempt")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("this experiment requires macOS")
    if not (args.prepare_only or args.decode_capture) and (not sys.stdin.isatty() or os.environ.get("TERM_PROGRAM") != "Apple_Terminal"):
        parser.error("run this manually in a fresh Apple Terminal window, or use --prepare-only")
    os.umask(0o077)
    if args.decode_capture:
        try:
            run, token, plain, original, rtf_data, plain_data = saved_capture(args.decode_capture)
        except (OSError, ValueError) as error:
            parser.error(str(error))
        attempt = Path(tempfile.mkdtemp(prefix="attempt-", dir=run))
        private_write(attempt / "attempt.json", json.dumps({"fixtureToken": token, "sourceRun": str(run),
            "decodedCapture": str(original), "fullTerminalState": False}, indent=2) + "\n")
        # Snapshot validated bytes so later edits of the original capture cannot
        # change this decode. Existing artifacts remain untouched.
        for name, data in (("input.rtf", rtf_data), ("input.txt", plain_data)):
            fd = os.open(attempt / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "wb") as stream:
                stream.write(data)
        binary = compile_helper(attempt)
        print(f"Saved capture: {original}\nNew private attempt: {attempt}\nOffline decode: clipboard and Terminal are not accessed.", flush=True)
        result = run_archive([str(binary), "--decode", str(attempt / "input.rtf"), str(attempt / "input.txt"),
                              token, str(attempt / "capture")], attempt, "Saved capture decode")
        return finish(result, token, plain, attempt, run, operation="Saved capture decode")
    if args.recapture:
        try:
            run, token, plain = saved_fixture(args.recapture)
        except (OSError, ValueError) as error:
            parser.error(str(error))
        attempt = Path(tempfile.mkdtemp(prefix="attempt-", dir=run))
        private_write(attempt / "attempt.json", json.dumps({"fixtureToken": token, "recaptureOf": str(run),
            "fullTerminalState": False}, indent=2) + "\n")
        binary = compile_helper(attempt)
        print(f"Original fixture: {run}\nNew private attempt: {attempt}\nFixture token: {token}", flush=True)
        print("In Terminal, select the prior fixture from its BEGIN marker through its END marker, including the final newline.")
        print("Press ordinary Command-C. Copy only this fixture; raw diagnostic files retain the entire copied selection.")
        print("Command-A then Command-C also works if this fixture appears exactly once, but saves the surrounding history too.")
        print("Nothing is reprinted or typed into Terminal. Existing fixture files and previous captures are kept.")
        print("If the prior fixture is gone, cancel and run this script without --recapture in a fresh Terminal window.")
        return capture(binary, token, plain, attempt, run)
    run = Path(tempfile.mkdtemp(prefix="hc-terminal-archive-", dir="/private/tmp"))
    token = uuid.uuid4().hex
    output, plain = fixture(token)
    private_write(run / "fixture.txt", plain)
    private_write(run / "fixture.ansi", output)
    private_write(run / "manifest.json", json.dumps({"fixtureToken": token, "fullTerminalState": False}, indent=2) + "\n")
    binary = compile_helper(run)
    print(f"Private results: {run}", flush=True)
    if args.prepare_only:
        print("Prepared only. Clipboard was not read. Run this script normally in a fresh Terminal window to test.")
        return 0
    print("This tests a styled history archive; no shell/process transfer or window closure occurs.")
    print("After the fixture prints, press Command-A then ordinary Command-C in this Terminal window.")
    print("Then press Return here. The helper saves the copied text and RTF, including surrounding copied text.")
    print("Use a fresh disposable Terminal window so the copy contains no unrelated conversation or commands.")
    if shutil.get_terminal_size().columns < 60:
        print("Please widen this window to at least 60 columns before continuing.")
    input("Press Return to print the fixture, or Control-C to cancel: ")
    sys.stdout.write(output)
    sys.stdout.flush()
    return capture(binary, token, plain, run, run)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nCancelled; Terminal remains open.")
        raise SystemExit(130)
    except subprocess.TimeoutExpired:
        print("Helper setup timed out; no capture was verified.", file=sys.stderr)
        raise SystemExit(124)
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"Archive verification could not finish: {error}", file=sys.stderr)
        raise SystemExit(1)
