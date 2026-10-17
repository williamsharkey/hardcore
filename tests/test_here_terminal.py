#!/usr/bin/env python3
"""Semantic reflow, persistence and theme regressions for terminal-hosted Hardcore."""
import json
from pathlib import Path
import random
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from here_terminal import *


def logical(term):
    records=list(zip(term.history,term.history_wrapped,term.history_used))
    records+=list(zip(term.normal.grid,term.normal.wrapped,term.normal.used))+term._tail
    result=[]; current=[]
    for line,wrapped,used in records:
        current.extend(line[:term._extent(line,used)])
        if not wrapped:
            result.append(current);current=[]
    while result and not result[-1]:result.pop()
    return result


def text(term):
    return [''.join(c.text for c in line if c.width) for line in logical(term)]


class ReflowTests(unittest.TestCase):
    def valid(self,t):
        for buf in (t.normal,t.alternate):
            self.assertEqual(len(buf.grid),t.rows)
            self.assertEqual(len(buf.used),t.rows);self.assertEqual(len(buf.wrapped),t.rows)
            self.assertTrue(0<=buf.x<t.cols and 0<=buf.y<t.rows)
            for row in buf.grid:
                self.assertEqual(len(row),t.cols)
                for x,cell in enumerate(row):
                    if cell.width==0:self.assertGreater(x,0);self.assertEqual(row[x-1].width,2)
                    if cell.width==2:self.assertLess(x+1,t.cols);self.assertEqual(row[x+1].width,0)
        self.assertEqual(len(t.history),len(t.history_wrapped));self.assertEqual(len(t.history),len(t.history_used))

    def test_roundtrip_preserves_styles_hard_breaks_blanks_spaces_wide_and_cursor(self):
        t=Terminal(30,8)
        t.feed(('\x1b[31mfirst line has much more than thirty characters\x1b[m\r\n\r\n'
                '\x1b[1;38;2;2;3;4m界 é 🚀\x1b[m  spaces   \r\nlast').encode())
        original=logical(t); cursor=(t.normal.x,t.normal.y,t.normal.wrap)
        for width,height in ((5,3),(2,4),(19,7),(8,2),(30,8)):
            t.resize(width,height);self.valid(t);self.assertEqual(logical(t),original)
        self.assertEqual((t.normal.x,t.normal.y,t.normal.wrap),cursor)
        t.feed(b'!');self.assertEqual(text(t)[-1],'last!')

    def test_wrapped_wide_padding_never_becomes_a_space_inside_word(self):
        t=Terminal(5,5);t.feed('1234界5678界9'.encode())
        original=logical(t)
        for width in (7,2,9,5):t.resize(width,5);self.valid(t);self.assertEqual(logical(t),original)
        self.assertEqual(text(t),['1234界5678界9'])

    def test_new_width_boundary_retains_insertion_point(self):
        t=Terminal(10,3);t.feed(b'abcde');t.resize(5,3);t.feed(b'F')
        self.assertEqual(text(t),['abcdeF'])
        t.resize(10,3);t.feed(b'G');self.assertEqual(text(t),['abcdeFG'])

    def test_cursor_above_content_keeps_hidden_rows_until_growth(self):
        t=Terminal(15,6)
        for n in range(5):t.feed(f'\x1b[{n+1};1Hrow{n}:abcdefghij'.encode())
        t.feed(b'\x1b[1;3H')
        original=logical(t);t.resize(4,2);self.valid(t)
        self.assertTrue(t._tail);self.assertEqual(logical(t),original)
        t.resize(15,6);self.assertEqual(logical(t),original)
        self.assertEqual((t.normal.x,t.normal.y),(2,0))
        t.resize(4,2);t.feed(b'\x1b[2J');self.assertFalse(t._tail)

    def test_scrollback_reflows_with_primary_and_bounded_storage(self):
        t=Terminal(20,3)
        for n in range(45):t.feed(f'row{n:02}:abcdefghijklmnopqrstuvwxyz\r\n'.encode())
        expected=text(t);t.resize(5,3);self.assertEqual(text(t),expected)
        t.resize(20,3);self.assertEqual(text(t),expected);self.valid(t)
        self.assertLessEqual(len(t.history)*t.cols,200000)

    def test_alternate_is_coordinate_grid_primary_still_reflows(self):
        t=Terminal(15,5);t.feed(b'primary longer than fifteen\x1b[?1049hALT\x1b[4;10HX')
        primary=logical(t);t.resize(6,3);self.assertEqual(logical(t),primary)
        self.assertTrue(''.join(c.text for c in t.alternate.grid[0]).startswith('ALT'))
        t.resize(15,5);t.feed(b'\x1b[?1049l!');self.assertEqual(text(t),['primary longer than fifteen!'])

    def test_history_metadata_survives_insert_delete_scroll_region(self):
        rng=random.Random(45);t=Terminal(20,7)
        atoms=[b'ab cd\r\n',b'\x1b[2;4H',b'\x1b[L',b'\x1b[M',b'\x1b[2J',b'\x1b[2;6r',b'\x1b[r',b'\x1b[S',b'\x1b[T', '界'.encode()]
        for n in range(400):
            t.feed(rng.choice(atoms))
            if n%11==0:t.resize(rng.randrange(2,30),rng.randrange(2,12))
            self.valid(t)


class PersistenceTests(unittest.TestCase):
    def test_snapshot_roundtrip_continues_partial_utf8_osc_and_reflow(self):
        for payload,rest in [(b'head'+ '🚀'.encode()[:2],'🚀'.encode()[2:]+b' tail'),
                             (b'head\x1b]2;part',b'ial\x07 tail'),
                             (b'head\x1b[38;2;',b'12;34;56m tail')]:
            t=Terminal(9,4);t.feed(payload)
            snap=json.loads(json.dumps(t.snapshot()));clone=Terminal.from_snapshot(snap)
            t.feed(rest);clone.feed(rest)
            self.assertEqual(t.snapshot(),clone.snapshot())
            clone.resize(3,2);t.resize(3,2);self.assertEqual(t.snapshot(),clone.snapshot())

    def test_snapshot_is_detached_and_compact(self):
        t=Terminal(160,50);t.feed(b'unchanged')
        snap=t.snapshot();before=json.dumps(snap);t.feed(b'additional');self.assertEqual(json.dumps(snap),before)
        self.assertLess(len(before),20000)

    def test_apply_keeps_callback_and_optional_history(self):
        out=[];t=Terminal(8,2,out.append);t.feed(b'a\r\nb\r\nc')
        clone=Terminal.from_snapshot(t.snapshot());clone.feed(b'!')
        t.apply_snapshot(clone.snapshot(False));t.feed(b'\x1b[5n')
        self.assertEqual(out,[b'\x1b[0n']);self.assertEqual(text(t),['a','b','c!'])

    def test_partial_screen_metadata_keeps_cells_and_bounds_dimensions(self):
        t=Terminal(12,4);t.feed(b'unchanged')
        clone=Terminal.from_snapshot(t.snapshot());clone.feed(b'\x1b[2;3H')
        t.apply_snapshot(clone.snapshot(False,False))
        self.assertEqual(text(t),['unchanged']);self.assertEqual((t.screen.x,t.screen.y),(2,1))
        clone.resize(8,3)
        with self.assertRaises(ValueError):t.apply_snapshot(clone.snapshot(False,False))

    def test_large_hidden_tail_survives_snapshot(self):
        t=Terminal(250,50)
        for row in range(50):t.feed(f'\x1b[{row+1};1H'.encode()+b'X'*250)
        t.feed(b'\x1b[H');before=logical(t);t.resize(2,2)
        self.assertGreater(len(t._tail),4200)
        clone=Terminal.from_snapshot(json.loads(json.dumps(t.snapshot())))
        clone.resize(250,50);self.assertEqual(logical(clone),before)

    def test_hostile_snapshot_runs_cannot_allocate_unbounded_memory_or_inject_controls(self):
        t=Terminal(10,4)
        for run in ([100000000,'x',1,0],[1,'\x1b]52;payload',1,0],[1,'x',3,0],[1,'x',1,99]):
            snap=t.snapshot();snap['normal']['cells']['rows'][0]=[run]
            with self.assertRaises(ValueError):Terminal.from_snapshot(snap)
        snap=t.snapshot();snap['cols']=1000000
        with self.assertRaises(ValueError):Terminal.from_snapshot(snap)


class ThemeAndProtocolTests(unittest.TestCase):
    def test_terminal_reset_preserves_monotonic_change_and_attention_counters(self):
        t=Terminal(15,4)
        for _ in range(5):t.feed(b'content\x07')
        before=(t.revision,t.activity,t.bell_count)
        t.feed(b'\x1bc')
        self.assertEqual((t.revision,t.activity,t.bell_count),(before[0]+1,before[1]+1,before[2]))
        self.assertFalse(any(c.text!=' ' for row in t.screen.grid for c in row))

    def test_palette_recolors_indexed_not_truecolor_and_queries_match(self):
        replies=[];t=Terminal(15,4,replies.append)
        t.feed(b'\x1b[31mA\x1b[38;2;204;36;29mB\x1b[39mC')
        palette=list(PALETTE);palette[1]=(1,2,3)
        t.set_theme(palette,(9,8,7),(6,5,4))
        self.assertEqual(t.normal.grid[0][0].style.fg,(1,2,3))
        self.assertEqual(t.normal.grid[0][1].style.fg,(204,36,29))
        self.assertEqual(t.resolved_style(t.normal.grid[0][2].style).fg,(9,8,7))
        t.feed(b'\x1b]10;?\x07\x1b]4;1;?\x07')
        self.assertIn(b'0909/0808/0707',replies[0]);self.assertIn(b'0101/0202/0303',replies[1])
        t.feed(b'\x1bc\x1b[31mX');self.assertEqual(t.style.fg,(1,2,3))

    def test_cwd_prompt_bell_and_untrusted_remote_url(self):
        t=Terminal(20,4);t.feed(b'\x1b]7;file://localhost/tmp/a%20b\x07\x1b]133;B\x07\x07')
        self.assertEqual(t.cwd,'/tmp/a b');self.assertEqual(t.prompt_state,'input');self.assertEqual(t.bell_count,1)
        t.feed(b'\x1b]7;file://remote.invalid/evil\x07\x1b]7;file:///bad%1bpath\x07')
        self.assertEqual(t.cwd,'/tmp/a b')
        t.feed(b'\x1b]9;9;"/new folder"\x07\x1b]133;C\x07');self.assertEqual(t.cwd,'/new folder');self.assertEqual(t.prompt_state,'running')
        t.feed(('\x1b]2;A'+'\u0301'*1000+'\x07').encode())
        self.assertLessEqual(len(t.title),160)
        self.assertEqual(Terminal.from_snapshot(t.snapshot()).title,t.title)


if __name__=='__main__':unittest.main()
