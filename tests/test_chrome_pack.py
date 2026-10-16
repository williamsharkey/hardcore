#!/usr/bin/env python3
"""Headless tests for the production menu row packing and click mappings.

Compiles only the pure helper; no running terminal, clipboard or GUI is used.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hardcore_chrome_pack.h"

static void require(int yes, const char *message) {
    if (!yes) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static ChromePackRow pack(const char *text, int width, int left, int right) {
    unsigned char ink[CHROME_PACK_MAX] = {0};
    ChromePackRow result;
    for (int i = 0; i < width && text[i]; i++) ink[i] = text[i] != ' ';
    chrome_pack_row(&result, ink, width, left, right);
    return result;
}
static void validate(const char *text, int width, ChromePackRow *p) {
    int previous = -2, x;
    for (x = 0; x < width; x++) {
        if (x >= p->blocked_left && x < p->blocked_right)
            require(p->src[x] == -1 && !p->actionable[x], "terminal intrusion exposed a menu glyph or target");
        if (p->src[x] >= 0) {
            require(p->src[x] < width, "source out of range");
            require(p->dst[p->src[x]] == x, "forward/inverse maps differ");
        }
    }
    for (x = 0; x < width;) {
        int start, end, visible = 0, complete = 1, destination = -1;
        if (text[x] == ' ') { x++; continue; }
        start = x;
        while (x < width && text[x] != ' ') x++;
        end = x;
        for (int c = start; c < end; c++) {
            int d = p->dst[c];
            if (d < 0 || !p->actionable[d]) complete = 0;
            else { if (!visible) destination = d; visible++; }
        }
        if (!visible) continue;
        require(complete && visible == end-start, "partial word has a clickable fragment");
        require(destination >= previous + 1, "word separators collapsed to zero");
        for (int c = start; c < end; c++) require(p->dst[c] == destination+c-start, "word split or reordered");
        previous = destination + end-start;
    }
}
static uint32_t rng = 0x243a91;
static uint32_t random32(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; }
int main(int argc, char **argv) {
    require(argc == 2, "case required");
    const char *which = argv[1];
    if (!strcmp(which, "monokai")) {
        const char *text = " Monokai                D    ";
        ChromePackRow p = pack(text, 28, 0, 2);
        for (int i = 1; i < 8; i++) require(p.dst[i] == i+1, "Monokai did not shift just one column");
        require(p.dst[24] == 24, "unobstructed theme control moved unnecessarily");
        validate(text, 28, &p);
    } else if (!strcmp(which, "borrow-spaces")) {
        const char *text = " cool hat    boy ";
        ChromePackRow p = pack(text, 16, 0, 4);
        for (int i = 0; i < 16; i++) if (text[i] != ' ') require(p.dst[i] >= 0, "internal whitespace was not borrowed");
        require(p.dst[1] == 4 && p.dst[13] == 13, "packing moved more than necessary");
        validate(text, 16, &p);
    } else if (!strcmp(which, "both-sides")) {
        const char *text = " one two   three four   ";
        ChromePackRow p = pack(text, 24, 9, 12);
        validate(text, 24, &p);
        for (int i = 0; i < 24; i++) if (text[i] != ' ') require(p.dst[i] >= 0, "words failed to pack around interior notch");
    } else if (!strcmp(which, "partial-inert")) {
        const char *text = " verylonglabel          D   ";
        ChromePackRow p = pack(text, 28, 0, 8);
        /* Here the complete word fits by moving. Force a genuinely narrow
           remainder, then ensure its visible suffix can never select it. */
        p = pack(text, 28, 0, 20);
        require(p.src[24] == 24 && p.actionable[24], "surviving theme key lost its click mapping");
        validate(text, 28, &p);
        p = pack("abcdefghijklmnop", 16, 0, 7);
        for (int i = 7; i < 16; i++) require(p.src[i] == i && !p.actionable[i], "partial label is not visible and inert");
    } else if (!strcmp(which, "full-and-hover")) {
        const char *text = " Menu text D L  ";
        ChromePackRow p = pack(text, 16, 0, 16);
        for (int i = 0; i < 16; i++) require(p.src[i] < 0 && !p.actionable[i], "fully occluded row retains targets");
        p = pack(text, 16, 0, 0);
        for (int i = 0; i < 16; i++) require(p.src[i] == i && p.dst[i] == i && p.actionable[i], "hover does not restore full geometry");
    } else if (!strcmp(which, "property")) {
        for (int trial = 0; trial < 10000; trial++) {
            int width = 3 + random32()%26, left = random32()%width, right = left+1+random32()%(width-left);
            char text[CHROME_PACK_MAX+1];
            for (int x = 0; x < width; x++) text[x] = random32()%4 == 0 ? ' ' : 'a'+random32()%26;
            text[width] = 0;
            ChromePackRow p = pack(text, width, left, right);
            validate(text, width, &p);
        }
    } else require(0, "unknown case");
    return 0;
}
'''


class ChromePackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="hardcore-chrome-pack-")
        folder = Path(cls.tmp.name)
        source = folder / "pack.c"
        source.write_text(HARNESS)
        cls.binary = folder / "pack"
        subprocess.run(["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT), str(source), "-o", str(cls.binary)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_monokai_shifts_one_cell(self): self.case("monokai")
    def test_borrows_internal_whitespace(self): self.case("borrow-spaces")
    def test_packs_both_sides_of_a_notch(self): self.case("both-sides")
    def test_partial_controls_are_inert(self): self.case("partial-inert")
    def test_fully_hidden_and_hover_rows(self): self.case("full-and-hover")
    def test_randomized_mappings_and_separators(self): self.case("property")


if __name__ == "__main__":
    unittest.main()
