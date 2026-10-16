#!/usr/bin/env python3
"""Offline archive tests. None of these paths read the system pasteboard."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("manual_archive", ROOT / "tools/manual_mac_terminal_archive.py")
manual = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manual)
TOKEN = "0123456789abcdef0123456789abcdef"


def plain_rtf(text):
    pieces = [r"{\rtf1\ansi\uc1 "]
    data = text.encode("utf-16-le")
    for offset in range(0, len(data), 2):
        unit = int.from_bytes(data[offset:offset + 2], "little")
        if unit == 10:
            pieces.append(r"\par ")
        elif unit in (92, 123, 125):
            pieces.append("\\" + chr(unit))
        elif 32 <= unit <= 126:
            pieces.append(chr(unit))
        else:
            pieces.append("\\u" + str(unit if unit < 32768 else unit - 65536) + "?")
    return ("".join(pieces) + "}").encode()


@unittest.skipUnless(sys.platform == "darwin", "AppKit decoder requires macOS")
class ArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="hc-archive-test-", dir="/private/tmp")
        cls.root = Path(cls.temp.name)
        cls.binary = cls.root / "helper"
        subprocess.run(["clang", "-O2", "-Wall", "-Wextra", "-Werror", "-fobjc-arc",
                        "-framework", "Foundation", "-framework", "AppKit",
                        str(ROOT / "tools/mac_terminal_archive.m"), "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.run = Path(tempfile.mkdtemp(dir=self.root))
        self.plain = f"HCTERM-FIXTURE:{TOKEN}:BEGIN\nred\nHCTERM-FIXTURE:{TOKEN}:END\n"
        self.rtf = (r"{\rtf1\ansi{\colortbl;\red255\green0\blue0;}\pard " +
                    self.plain.replace("\n", r"\par " ).replace("red", r"\cf1 red\cf0 ") + "}").encode()
        (self.run / "input.rtf").write_bytes(self.rtf)
        (self.run / "input.txt").write_text(self.plain)

    def decode(self, token=TOKEN, out="output"):
        return subprocess.run([str(self.binary), "--decode", str(self.run / "input.rtf"),
                               str(self.run / "input.txt"), token, str(self.run / out)],
                              capture_output=True, text=True)

    def test_selftest(self):
        result = subprocess.run([str(self.binary), "--selftest"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("22 checks; 0 failures", result.stdout)

    def test_archive_originals_permissions_and_color(self):
        result = self.decode()
        self.assertEqual(result.returncode, 0, result.stderr)
        folder = self.run / "output"
        self.assertEqual(stat.S_IMODE(folder.stat().st_mode), 0o700)
        for path in folder.iterdir():
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual((folder / "original.rtf").read_bytes(), self.rtf)
        self.assertEqual((folder / "original.txt").read_text(), self.plain)
        archive = json.loads((folder / "archive.json").read_text())
        self.assertEqual(archive["text"], self.plain)
        self.assertFalse(archive["fullTerminalState"])
        self.assertEqual(archive["colorSpace"], "sRGB")
        # RTF's calibrated RGB is color-managed when AppKit resolves sRGB.
        red = next(run["foreground"] for run in archive["runs"] if "foreground" in run)
        self.assertGreater(red[0], 0.9)
        self.assertLess(red[1], 0.3)
        self.assertLess(red[2], 0.1)
        self.assertEqual(red[3], 1)

    def test_wrong_token_saves_nothing(self):
        self.assertNotEqual(self.decode("a" * 32).returncode, 0)
        self.assertFalse((self.run / "output").exists())

    def test_fixture_mismatch_preserves_private_diagnostics_without_archive(self):
        (self.run / "input.txt").write_text(self.plain.replace("red", "changed"))
        result = self.decode()
        self.assertNotEqual(result.returncode, 0)
        output = self.run / "output"
        self.assertFalse((output / "archive.json").exists())
        self.assertEqual((output / "original.rtf").read_bytes(), self.rtf)
        self.assertEqual((output / "original.txt").read_text(), self.plain.replace("red", "changed"))
        self.assertEqual((output / "decoded-rtf.txt").read_text(), self.plain)
        comparison = json.loads((output / "comparison.json").read_text())
        self.assertFalse(comparison["fixture"]["exact"])
        self.assertEqual(comparison["fixture"]["firstMismatchUTF16"], self.plain.index("red"))
        self.assertEqual(comparison["fixture"]["plainCodeUnits"][0], "0063")
        self.assertEqual(comparison["fixture"]["rtfCodeUnits"][0], "0072")
        self.assertFalse(comparison["fullTerminalState"])
        self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o700)
        for path in output.iterdir():
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertNotEqual(self.decode().returncode, 0)
        self.assertEqual((output / "original.rtf").read_bytes(), self.rtf)

    def test_surrounding_text_is_not_normalized(self):
        prefix, suffix = "unrelated prefix\n", "unrelated suffix\n"
        (self.run / "input.txt").write_text(prefix + self.plain + suffix)
        data = self.rtf.replace(b"\\pard ", b"\\pard unrelated prefix\\par ")
        (self.run / "input.rtf").write_bytes(data[:-1] + b"unrelated suffix\\par }")
        result = self.decode()
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = json.loads((self.run / "output/archive.json").read_text())
        self.assertEqual(archive["text"], self.plain)
        self.assertEqual(archive["originalRange"]["start"], len(prefix))
        self.assertEqual((self.run / "output/original.txt").read_text(), prefix + self.plain + suffix)

    def test_duplicate_markers_refused(self):
        (self.run / "input.txt").write_text(self.plain + self.plain)
        self.assertNotEqual(self.decode().returncode, 0)
        self.assertFalse((self.run / "output").exists())

    def test_different_surroundings_use_independent_attribute_ranges(self):
        prefix = "plain prefix 🚀\n"
        (self.run / "input.txt").write_text(prefix + self.plain + "plain suffix\n")
        rtf = self.rtf.replace(b"\\pard ", b"\\pard shorter\\par ")
        (self.run / "input.rtf").write_bytes(rtf[:-1] + b"different suffix\\par }")
        result = self.decode()
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = json.loads((self.run / "output/archive.json").read_text())
        self.assertEqual(archive["text"], self.plain)
        self.assertEqual(archive["originalRange"]["start"], len(prefix.encode("utf-16-le")) // 2)
        self.assertEqual(archive["rtfOriginalRange"]["start"], len("shorter\n"))
        self.assertTrue(archive["comparison"]["fixture"]["exact"])
        self.assertFalse(archive["comparison"]["wholeSelection"]["exact"])
        red = next(run for run in archive["runs"] if "foreground" in run)
        self.assertEqual(red["start"], self.plain.index("red"))
        self.assertEqual(red["length"], len("red"))

    def test_duplicate_marker_only_in_rtf_refused_without_saving(self):
        (self.run / "input.rtf").write_bytes(plain_rtf(self.plain + self.plain))
        result = self.decode()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("markers", result.stderr)
        self.assertFalse((self.run / "output").exists())

    def test_canonical_composition_maps_styles_while_preserving_exact_plain_text(self):
        nfc = self.plain.replace("red", "café")
        nfd = self.plain.replace("red", "cafe\u0301")
        (self.run / "input.rtf").write_bytes(plain_rtf(nfc))
        (self.run / "input.txt").write_text(nfd)
        result = self.decode()
        self.assertEqual(result.returncode, 0, result.stderr)
        comparison = json.loads((self.run / "output/comparison.json").read_text())["fixture"]
        self.assertFalse(comparison["exact"])
        self.assertTrue(comparison["diagnosticEquivalence"]["unicodeNormalizationOnly"])
        self.assertFalse(comparison["diagnosticEquivalence"]["lineEndingsOnly"])
        archive = json.loads((self.run / "output/archive.json").read_text())
        self.assertEqual(archive["text"], nfd)
        self.assertEqual(archive["styleMapping"]["method"], "canonical-grapheme")
        self.assertEqual(archive["styleMapping"]["remappedClusters"], 1)
        self.assertTrue(archive["styleMapping"]["verified"])
        self.assertTrue(archive["styleMapping"]["plainTextPreserved"])
        self.assertFalse(archive["fullTerminalState"])

    def test_canonical_mapping_both_directions_and_later_style_offsets(self):
        cases = [("e\u0301", "é"), ("é", "e\u0301"),
                 ("\u1100\u1161", "가"), ("a\u0315\u0300", "à\u0315")]
        for index, (plain_cluster, rtf_cluster) in enumerate(cases):
            with self.subTest(plain_cluster=plain_cluster):
                plain = self.plain.replace("red", plain_cluster + " tail 🚀")
                rich = self.plain.replace("red", rtf_cluster + " tail 🚀")
                rtf = plain_rtf(rich).replace(b"\\ansi", b"\\ansi{\\colortbl;\\red255\\green0\\blue0;}")
                rtf = rtf.replace(b"tail", b"\\cf1 tail\\cf0 ")
                (self.run / "input.txt").write_text(plain)
                (self.run / "input.rtf").write_bytes(rtf)
                result = self.decode(out=f"mapped-{index}")
                self.assertEqual(result.returncode, 0, result.stderr)
                archive = json.loads((self.run / f"mapped-{index}/archive.json").read_text())
                self.assertEqual(archive["text"], plain)
                self.assertFalse(archive["comparison"]["fixture"]["exact"])
                self.assertEqual(archive["styleMapping"]["remappedClusters"], 1)
                at = len(plain[:plain.index("tail")].encode("utf-16-le")) // 2
                run = next(run for run in archive["runs"] if run["start"] <= at < run["start"] + run["length"])
                self.assertGreater(run["foreground"][0], 0.9)
                self.assertLess(run["foreground"][1], 0.3)
                self.assertEqual(run["start"], at)
                self.assertEqual(run["length"], 4)
                self.assertEqual(sum(run["length"] for run in archive["runs"]), len(plain.encode("utf-16-le")) // 2)

    def test_differently_styled_changed_grapheme_refused(self):
        plain = self.plain.replace("red", "é")
        rich = self.plain.replace("red", "e\u0301")
        rtf = plain_rtf(rich).replace(b"e\\u769?", b"e\\b \\u769?\\b0 ")
        (self.run / "input.txt").write_text(plain)
        (self.run / "input.rtf").write_bytes(rtf)
        result = self.decode()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("differing styles", result.stderr)
        self.assertTrue((self.run / "output/comparison.json").exists())
        self.assertFalse((self.run / "output/archive.json").exists())

    def test_unchanged_grapheme_retains_internal_styles_when_other_cluster_changes(self):
        plain = self.plain.replace("red", "e\u0301 x\u0301")
        rich = self.plain.replace("red", "é x\u0301")
        rtf = plain_rtf(rich).replace(b"x\\u769?", b"x\\b \\u769?\\b0 ")
        (self.run / "input.txt").write_text(plain)
        (self.run / "input.rtf").write_bytes(rtf)
        result = self.decode()
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = json.loads((self.run / "output/archive.json").read_text())
        self.assertEqual(archive["text"], plain)
        at = len(plain[:plain.index("x\u0301")].encode("utf-16-le")) // 2
        attrs = lambda unit: next(run for run in archive["runs"] if run["start"] <= unit < run["start"] + run["length"])
        self.assertFalse(attrs(at).get("font", {}).get("bold", False))
        self.assertTrue(attrs(at + 1)["font"]["bold"])

    def test_compatibility_only_unicode_difference_is_not_mapped(self):
        (self.run / "input.txt").write_text(self.plain.replace("red", "ff"))
        (self.run / "input.rtf").write_bytes(plain_rtf(self.plain.replace("red", "ﬀ")))
        result = self.decode()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.run / "output/archive.json").exists())

    def test_newline_differences_are_diagnosed_but_never_accepted(self):
        for index, replacement in enumerate(("red\r\n", "red\u2028")):
            with self.subTest(replacement=replacement):
                (self.run / "input.txt").write_text(self.plain.replace("red\n", replacement + "extra\n"))
                (self.run / "input.rtf").write_bytes(plain_rtf(self.plain.replace("red\n", "red\nextra\n")))
                result = self.decode(out=f"output-{index}")
                self.assertNotEqual(result.returncode, 0)
                folder = self.run / f"output-{index}"
                comparison = json.loads((folder / "comparison.json").read_text())["fixture"]
                self.assertFalse(comparison["exact"])
                self.assertEqual(comparison["diagnosticEquivalence"]["lineEndingsOnly"], index == 0)
                self.assertFalse((folder / "archive.json").exists())

    def test_one_sided_missing_final_newline_is_refused_in_either_representation(self):
        for plain_missing in (True, False):
            with self.subTest(plain_missing=plain_missing):
                (self.run / "input.txt").write_text(self.plain[:-1] if plain_missing else self.plain)
                (self.run / "input.rtf").write_bytes(plain_rtf(self.plain if plain_missing else self.plain[:-1]))
                result = self.decode(out=f"output-{plain_missing}")
                self.assertNotEqual(result.returncode, 0)
                folder = self.run / f"output-{plain_missing}"
                comparison = json.loads((folder / "comparison.json").read_text())["fixture"]
                self.assertFalse(comparison["exact"])
                self.assertEqual(comparison["firstMismatchUTF16"], len(self.plain) - 1)
                self.assertFalse((folder / "archive.json").exists())

    def test_marker_line_ending_differences_retain_diagnostics_without_acceptance(self):
        # Different line endings still identify the fixture. They must not
        # prevent diagnosis, or silently count as exact character retention.
        for index, newline in enumerate(("\r\n", "\r", "\u2028", "\u2029")):
            with self.subTest(newline=newline):
                (self.run / "input.txt").write_bytes(self.plain.replace("\n", newline).encode())
                result = self.decode(out=f"newlines-{index}")
                self.assertNotEqual(result.returncode, 0)
                folder = self.run / f"newlines-{index}"
                comparison = json.loads((folder / "comparison.json").read_text())["fixture"]
                self.assertFalse(comparison["exact"])
                self.assertEqual(comparison["diagnosticEquivalence"]["lineEndingsOnly"], index < 2)
                self.assertFalse((folder / "archive.json").exists())

    def test_output_reuse_refused_without_overwrite(self):
        self.assertEqual(self.decode().returncode, 0)
        original = (self.run / "output/archive.json").read_bytes()
        self.assertNotEqual(self.decode().returncode, 0)
        self.assertEqual((self.run / "output/archive.json").read_bytes(), original)

    def test_input_symlink_refused(self):
        (self.run / "input.rtf").rename(self.run / "other.rtf")
        (self.run / "input.rtf").symlink_to(self.run / "other.rtf")
        self.assertNotEqual(self.decode().returncode, 0)
        self.assertFalse((self.run / "output").exists())

    def test_output_symlink_refused(self):
        (self.run / "elsewhere").mkdir()
        (self.run / "output").symlink_to(self.run / "elsewhere", target_is_directory=True)
        self.assertNotEqual(self.decode().returncode, 0)
        self.assertEqual(list((self.run / "elsewhere").iterdir()), [])

    def test_oversized_regular_file_refused(self):
        with (self.run / "input.rtf").open("wb") as stream:
            stream.truncate(8 * 1024 * 1024 + 1)
        self.assertNotEqual(self.decode().returncode, 0)
        self.assertFalse((self.run / "output").exists())

    def test_rtf_object_refused(self):
        (self.run / "input.rtf").write_bytes(b"{\\rtf1 {\\object\\objdata hidden}}")
        self.assertNotEqual(self.decode().returncode, 0)
        self.assertFalse((self.run / "output").exists())

    def test_escaped_literal_control_names_and_braces_are_preserved(self):
        text = (f"HCTERM-FIXTURE:{TOKEN}:BEGIN\n"
                r"literal \object \objdata \pict \bin \PICT123 {braces} and \\pict" "\n"
                f"HCTERM-FIXTURE:{TOKEN}:END\n")
        escaped = text.replace("\\", "\\\\").replace("{", "\\{").replace("}", "\\}").replace("\n", "\\par ")
        (self.run / "input.rtf").write_bytes((r"{\rtf1\ansi " + escaped + "}").encode())
        (self.run / "input.txt").write_text(text)
        result = self.decode()
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = json.loads((self.run / "output/archive.json").read_text())
        self.assertEqual(archive["text"], text)

    def test_real_unsupported_control_words_refused_with_case_and_parameter_delimiters(self):
        for control in (r"\object", r"\ObJeCt0", r"\objdata", r"\OBJDATA12", r"\pict", r"\PiCt-1",
                        r"\bin", r"\BIN123", r"\pict{nested}", r"\objdata\plain", r"\\\pict"):
            with self.subTest(control=control):
                (self.run / "input.rtf").write_bytes((r"{\rtf1 " + control + " hidden}").encode())
                result = self.decode()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("objects, images, and binary payloads", result.stderr)
                self.assertFalse((self.run / "output").exists())

    def test_control_word_prefixes_are_not_mistaken_for_unsupported_words(self):
        # RTF readers ignore unknown words. Similar prefixes are not object,
        # pict or bin controls and must not be rejected by this lexical gate.
        text = self.plain.replace("\n", r"\par ")
        (self.run / "input.rtf").write_bytes((r"{\rtf1\ansi\objectlike0\objdataextra0\picture0\binary0 "
                                               + text + "}").encode())
        result = self.decode()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((self.run / "output/archive.json").read_text())["text"], self.plain)

    def test_deeply_nested_rtf_refused(self):
        (self.run / "input.rtf").write_bytes(b"{\\rtf1 " + b"{" * 129 + b"x" + b"}" * 130)
        result = self.decode()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nesting", result.stderr)
        self.assertFalse((self.run / "output").exists())

    def test_fixture_report_is_explicitly_partial(self):
        ansi, plain = manual.fixture(TOKEN)
        self.assertIn("\x1b[", ansi)
        self.assertNotIn("\x1b", plain)
        report = manual.evaluate({"text": plain, "observedAttributes": ["foreground"]}, plain, TOKEN)
        self.assertTrue(report["fixtureTextExact"])
        self.assertEqual(report["historyRowsPresent"], 64)
        self.assertTrue(report["unicodePresent"])
        self.assertFalse(report["fullTerminalState"])

    def test_wrap_difference_reported(self):
        _, plain = manual.fixture(TOKEN)
        modified = plain.replace("Wrap:0123456789", "Wrap:\n0123456789")
        self.assertFalse(manual.evaluate({"text": modified}, plain, TOKEN)["fixtureTextExact"])

    def test_missing_final_newline_is_reported_as_text_loss(self):
        _, plain = manual.fixture(TOKEN)
        self.assertTrue(plain.endswith("\n"))
        self.assertFalse(manual.evaluate({"text": plain[:-1]}, plain, TOKEN)["fixtureTextExact"])
        self.assertFalse(manual.evaluate({"text": plain + "\n"}, plain, TOKEN)["fixtureTextExact"])
        self.assertTrue(manual.evaluate({"text": plain}, plain, TOKEN)["fixtureTextExact"])


if __name__ == "__main__":
    unittest.main()
