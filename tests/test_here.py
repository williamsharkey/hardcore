#!/usr/bin/env python3
"""ANSI state/paste framing and real owned PTYs for terminal-hosted Hardcore."""
import importlib.util
import json
import os
from pathlib import Path
import pty
import random
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
from types import SimpleNamespace
import unittest
import fcntl

ROOT=Path(__file__).resolve().parents[1]
SPEC=importlib.util.spec_from_file_location('hc_here',ROOT/'tools/here.py')
here=importlib.util.module_from_spec(SPEC);sys.modules[SPEC.name]=here;SPEC.loader.exec_module(here)


def rows(term):
    return [''.join(c.text for c in line if c.width) for line in term.screen.grid]


def wait(check, timeout=5):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        value=check()
        if value:return value
        time.sleep(.01)
    raise AssertionError('timed out waiting for owned fixture')


class ScreenTests(unittest.TestCase):
    def assert_valid(self,term):
        for buffer in (term.normal,term.alternate):
            self.assertEqual(len(buffer.grid),term.rows)
            self.assertTrue(0<=buffer.x<term.cols and 0<=buffer.y<term.rows)
            for line in buffer.grid:
                self.assertEqual(len(line),term.cols)
                for x,cell in enumerate(line):
                    self.assertLessEqual(len(cell.text),64)
                    if cell.width==2:self.assertLess(x+1,term.cols);self.assertEqual(line[x+1].width,0)
                    if cell.width==0:self.assertGreater(x,0);self.assertEqual(line[x-1].width,2)

    def test_parameterless_csi_and_recorded_xterm_sequences_are_bounded(self):
        for final in 'ABCDEFGHf`deJKLMPSTXZ@ghlmrsuc':
            term=here.Terminal(20,8)
            term.feed(b'filled\x1b[31m\x1b['+final.encode())
            self.assert_valid(term)
        term=here.Terminal(10,3)
        term.feed(b'\x1b[31mtext\x1b[m\x1b[H\x1b[J')
        self.assertEqual(term.style,here.Style())
        self.assertEqual(rows(term),[' '*10]*3)
        corpus=sorted((ROOT/'tests/xtermjs').glob('*.in'))
        self.assertGreater(len(corpus),50)
        for fixture in corpus:
            term=here.Terminal(80,25)
            payload=fixture.read_bytes()
            for start in range(0,len(payload),73):term.feed(payload[start:start+73])
            self.assert_valid(term)

    def test_random_split_control_streams_and_combining_limits(self):
        rng=random.Random(502)
        term=here.Terminal(47,12)
        atoms=[b'\x1b[',b'\x1b]',b'\x1bP',b'\x1b\\',b'\x07',b'\x18',b'\x1a',
               b'\x1b[?1049h',b'\x1b[?1049l',b'\x1b[r',b'\x1b[m',b'\x1b[3;9H',
               b'\x1b[2@',b'\x1b[3P',b'\x1b[K',b'\x1b[6n',b'\r\n',b'abc;:$?',
               '界 é 🚀'.encode(),b'\xff\xfe']
        for n in range(1500):
            atom=rng.choice(atoms);split=rng.randrange(len(atom)+1)
            term.feed(atom[:split]);term.feed(atom[split:])
            if n%31==0:term.resize(rng.randrange(2,80),rng.randrange(1,30));self.assert_valid(term)
        term.feed(('\x18\x1bcA'+'\u0301'*10000).encode())
        self.assertEqual(len(term.screen.grid[0][0].text),64)
        self.assert_valid(term)

    def test_split_utf8_combining_wide_styles_and_literal_links(self):
        term=here.Terminal(20,3)
        payload=b'\x1b[1;38;2;17;153;85;48;5;196m'+ 'café é 🚀 界'.encode()
        for byte in payload:term.feed(bytes([byte]))
        self.assertTrue(rows(term)[0].startswith('café é 🚀 界'))
        self.assertEqual(term.screen.grid[0][0].style.fg,(17,153,85))
        self.assertEqual(term.screen.grid[0][0].style.bg,(255,0,0))
        self.assertTrue(term.screen.grid[0][0].style.flags&here.BOLD)
        self.assertEqual(term.screen.grid[0][5].text,'é')
        for line in term.screen.grid:
            for x,cell in enumerate(line):
                if cell.width==2:self.assertEqual(line[x+1].width,0)
                if cell.width==0:self.assertEqual(line[x-1].width,2)
        term.feed(b'\x1b]8;;https://example.test/a\x1b\\L\x1b]8;;\x1b\\')
        self.assertIn('https://example.test/a',here.render_line(term.screen.grid[0]))
        term.feed(b'\x1b]8;;javascript:bad()\x07X')
        self.assertNotIn('javascript:',here.render_line(term.screen.grid[0]))
        term.feed('\x1b]8;;https://example.test/\x9bBAD\x07X'.encode())
        self.assertEqual(term.style.link,'')

    def test_alternate_screen_restores_cursor_style_and_primary(self):
        term=here.Terminal(10,4);term.feed(b'normal\x1b[2;3H\x1b[31m')
        before=rows(term)
        term.feed(b'\x1b[?1049h\x1b[2Jalternate\x1b[?25l\x1b[5 q')
        self.assertTrue(term.alt);self.assertEqual(term.cursor_shape,5);self.assertFalse(term.cursor_visible)
        term.feed(b'\x1b[?1049l')
        self.assertFalse(term.alt);self.assertEqual(rows(term),before)
        self.assertEqual((term.screen.y,term.screen.x),(1,2))
        self.assertEqual(term.style.fg,here.PALETTE[1])

    def test_scroll_region_erase_insert_delete_and_no_wide_orphans(self):
        term=here.Terminal(8,5)
        for y in range(5):term.feed(f'\x1b[{y+1};1Hrow{y}'.encode())
        term.feed(b'\x1b[2;4r\x1b[4;1H\n')
        self.assertEqual([r[:4] for r in rows(term)],['row0','row2','row3','    ','row4'])
        term.feed(b'\x1b[r\x1b[2;1HABCD\x1b[2;2H\x1b[2@')
        self.assertTrue(rows(term)[1].startswith('A  BCD'))
        term.feed(b'\x1b[2P');self.assertTrue(rows(term)[1].startswith('ABCD'))
        term.feed(('\x1b[3;1H界界\x1b[3;2H\x1b[X').encode())
        self.assertEqual(term.screen.grid[2][0].text,' ')
        term.resize(3,3)
        for line in term.screen.grid:
            for x,cell in enumerate(line):
                if cell.width==2:self.assertLess(x+1,len(line));self.assertEqual(line[x+1].width,0)
                if cell.width==0:self.assertGreater(x,0);self.assertEqual(line[x-1].width,2)

    def test_queries_modes_origin_and_colon_rgb(self):
        replies=[];term=here.Terminal(12,6,replies.append)
        term.feed(b'\x1b[2;5r\x1b[?6h\x1b[2;3H\x1b[6n\x1b[5n\x1b[c\x1b[>c\x1b[18t\x1b[?u')
        self.assertEqual(replies[:2],[b'\x1b[2;3R',b'\x1b[0n'])
        self.assertIn(b'\x1b[8;6;12t',replies);self.assertIn(b'\x1b[?0u',replies)
        term.feed(b'\x1b[?1;1002;1006;1004;2004h')
        self.assertEqual((term.app_cursor,term.mouse,term.mouse_sgr,term.focus,term.bracketed_paste),(True,1002,True,True,True))
        term.feed(b'\x1b[38:2::1:2:3;48:2:0:4:5:6mX')
        self.assertEqual(term.style.fg,(1,2,3));self.assertEqual(term.style.bg,(4,5,6))
        term.feed(b'\x1b]11;?\x07');self.assertTrue(replies[-1].startswith(b'\x1b]11;rgb:'))

    def test_unknown_oversized_and_split_control_strings_cannot_escape(self):
        term=here.Terminal(40,2)
        for payload in (b'\x1b]52;c;BAD\x1b',b'\\safe',b'\x1bPbad\x07stillbad\x1b\\ok',
                        b'\x1b]2;'+b'x'*17000+b'\x07after',b'\x1b['+b'1'*300+b'mtail',
                        '\x9bSECRET'.encode(),b'\x1b#8end'):
            term.feed(payload)
        text=''.join(rows(term))
        self.assertNotIn('BAD',text);self.assertNotIn('stillbad',text)
        self.assertIn('safeokaftertailSECRETend',text)
        output=here.render_line(term.screen.grid[0])
        self.assertNotIn('\x9b',output);self.assertNotIn(']52',output)
        term.feed(b'\x1b]4;'+b'9'*5000+b';?\x07')

    def test_history_keeps_screen_and_does_not_mutate_on_view(self):
        term=here.Terminal(8,3)
        term.feed(b'one\r\ntwo\r\nthree\r\nfour\r\nfive')
        before=rows(term);view=term.visible(2)
        self.assertEqual(''.join(c.text for c in view[0]),'one     ')
        self.assertEqual(rows(term),before);self.assertEqual(len(term.history),2)
        term=here.Terminal(8,2);term.feed('aaaa界\r\nb\r\nc'.encode());term.resize(5,2)
        before=[list(row) for row in term.history]
        self.assertEqual(term.visible(1)[0][-1].width,1)
        self.assertEqual(list(term.history),before)

    def test_repeated_resize_never_splits_wide_cells(self):
        term=here.Terminal(30,8)
        for width in (30,7,24,2,19,3,40,12):
            term.feed(('\r\n界 café '+'😀'*20).encode())
            term.resize(width,5)
            self.assertEqual(len(term.screen.grid),5)
            self.assertTrue(all(len(row)==width for row in term.screen.grid))
            for line in term.screen.grid:
                for x,cell in enumerate(line):
                    if cell.width==2:self.assertLess(x+1,width);self.assertEqual(line[x+1].width,0)

    def test_status_unicode_uses_columns_and_closed_app_ignores_rest_of_input(self):
        app=here.Application('/bin/sh',str(ROOT))
        try:
            app.cols=12;app.message='界́ café 🚀 end'
            cells=app.status()
            self.assertEqual(len(cells),12)
            self.assertEqual(cells[0].text,'界́');self.assertEqual(cells[1].width,0)
            self.assertNotIn('end',here.render_line(cells))
            app.stop=True
            app.decoder.feed(b'\x02c'+here.BEGIN_PASTE+b'ignored'+here.END_PASTE,lambda:bool(app.tabs))
            self.assertEqual(app.tabs,[])
        finally:app.selector.close()

    def test_closing_final_tab_ignores_batched_paste_mouse_focus_and_new_tab(self):
        app=here.Application('/bin/sh',str(ROOT));closed=[]
        try:
            app.tabs=[SimpleNamespace(fd=-1,terminal=here.Terminal(80,23),close=lambda:closed.append(True))]
            app.decoder.feed(b'\x02xy'+here.BEGIN_PASTE+b'payload\x02c'+here.END_PASTE+
                             b'\x1b[<0;2;3M\x1b[I\x02c',lambda:bool(app.tabs) and app.current.terminal.bracketed_paste)
            self.assertTrue(app.stop);self.assertEqual(app.tabs,[]);self.assertEqual(closed,[True])
        finally:app.selector.close()


class InputTests(unittest.TestCase):
    def decoder(self):
        sent=[];commands=[];mouse=[];focus=[]
        decoder=here.InputDecoder(sent.append,commands.append,lambda *args:mouse.append(args),focus.append)
        return decoder,sent,commands,mouse,focus

    def test_split_paste_preserves_every_byte_and_does_not_run_shortcuts(self):
        payload=b'abc\x02c\x02qy\x1b[?25l'+ 'café 🚀'.encode()
        for wrapped in (True,False):
            decoder,sent,commands,_,_=self.decoder()
            framed=here.BEGIN_PASTE+payload+here.END_PASTE
            for byte in framed:decoder.feed(bytes([byte]),wrapped)
            self.assertEqual(b''.join(sent),framed if wrapped else payload)
            self.assertEqual(commands,[]);self.assertFalse(decoder.paste)

    def test_prefix_escape_mouse_focus_and_partial_csi(self):
        decoder,sent,commands,mouse,focus=self.decoder()
        for value in (b'abc\x02',b'c\x02\x02\x1b[',b'<0;2;3M\x1b[O',b'\x1b[A'):
            decoder.feed(value)
        self.assertEqual(b''.join(sent),b'abc\x02\x1b[A')
        self.assertEqual(commands,[None,ord('c'),None,-1])
        self.assertEqual(mouse,[(0,2,3,True)]);self.assertEqual(focus,[False])

    def test_exhaustive_chunk_boundaries_for_framed_paste(self):
        wire=here.BEGIN_PASTE+b'words\x02q\x1b[201x'+here.END_PASTE+b'end'
        for split in range(len(wire)+1):
            decoder,sent,commands,_,_=self.decoder()
            decoder.feed(wire[:split]);decoder.feed(wire[split:])
            self.assertEqual(b''.join(sent),wire);self.assertFalse(commands)


WORKER=r'''
import fcntl,json,os,signal,struct,sys,termios,time,tty
log=sys.argv[1]
def record(kind,**fields):
    with open(log,'a') as stream:stream.write(json.dumps(dict(kind=kind,**fields))+'\n')
def size(*_):record('size',value=struct.unpack('HHHH',fcntl.ioctl(0,termios.TIOCGWINSZ,b'\0'*8))[:2],pid=os.getpid())
tty.setraw(0);signal.signal(signal.SIGWINCH,size)
size();record('ready',pid=os.getpid(),environment={key:os.environ.get(key) for key in ('HARDCORE_CTL','HARDCORE_WINDOW_PID','HARDCORE_TAB_ID','NO_COLOR','TERM_PROGRAM')})
os.write(1,b'\x1b]2;Owned here fixture\x07\x1b[38;2;17;153;85mCOLOR_READY\x1b[0m\r\n\x1b[?2004h')
while True:
    data=os.read(0,65536)
    if not data:break
    record('input',hex=data.hex())
    os.write(1,b'ACK')
'''


class OwnedPtyTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='hc-here-test-');self.path=Path(self.temp.name)
        self.worker=self.path/'worker with spaces.py';self.worker.write_text(WORKER)
        self.log=self.path/'events.jsonl'
        self.master,self.slave=pty.openpty()
        fcntl.ioctl(self.slave,termios.TIOCSWINSZ,struct.pack('HHHH',14,72,0,0))
        self.original=termios.tcgetattr(self.slave)
        os.set_blocking(self.master,False)
        self.output=bytearray();self.process=None

    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:self.process.wait(timeout=4)
            except subprocess.TimeoutExpired:self.process.kill();self.process.wait()
        os.close(self.master);os.close(self.slave);self.temp.cleanup()

    def drain(self):
        while select.select([self.master],[],[],0)[0]:
            try:self.output.extend(os.read(self.master,65536))
            except (BlockingIOError,OSError):break
        return self.output

    def records(self):
        self.drain()
        if not self.log.exists():return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def start(self):
        env=dict(os.environ,HARDCORE_CTL='not-a-gui',HARDCORE_WINDOW_PID='123',HARDCORE_TAB_ID='456',NO_COLOR='1')
        self.process=subprocess.Popen([sys.executable,str(ROOT/'tools/here.py'),'--shell','/bin/sh','--cwd',str(self.path),'--',sys.executable,str(self.worker),str(self.log)],stdin=self.slave,stdout=self.slave,stderr=self.slave,start_new_session=True,env=env)
        wait(lambda:any(r['kind']=='ready' for r in self.records()))
        wait(lambda:b'COLOR_READY' in self.drain())
        return next(r['pid'] for r in self.records() if r['kind']=='ready')

    def test_real_pty_colors_paste_tabs_resize_and_restoration(self):
        pid=self.start()
        environment=next(r['environment'] for r in self.records() if r['kind']=='ready')
        self.assertEqual(environment,dict(HARDCORE_CTL=None,HARDCORE_WINDOW_PID=None,HARDCORE_TAB_ID=None,NO_COLOR='1',TERM_PROGRAM='hardcore-here'))
        self.assertIn(b'38;2;17;153;85',self.output)
        os.write(self.master,here.BEGIN_PASTE+b'literal\x02c\x02qy'+here.END_PASTE)
        expected=here.BEGIN_PASTE+b'literal\x02c\x02qy'+here.END_PASTE
        wait(lambda:expected in b''.join(bytes.fromhex(r['hex']) for r in self.records() if r['kind']=='input'))
        self.assertIsNone(self.process.poll())
        fcntl.ioctl(self.slave,termios.TIOCSWINSZ,struct.pack('HHHH',20,91,0,0));os.kill(self.process.pid,signal.SIGWINCH)
        wait(lambda:any(r['kind']=='size' and r['value']==[19,91] and r['pid']==pid for r in self.records()))
        before=b''.join(bytes.fromhex(r['hex']) for r in self.records() if r['kind']=='input')
        os.write(self.master,b'\x02c')
        time.sleep(.1);self.drain()
        os.write(self.master,b"printf 'SECOND_TAB_READY\\n'\r")
        wait(lambda:b'SECOND_TAB_READY' in self.drain())
        os.write(self.master,b'\x021');time.sleep(.1);self.drain()
        self.assertEqual(before,b''.join(bytes.fromhex(r['hex']) for r in self.records() if r['kind']=='input'))
        os.write(self.master,b'\x02qy')
        wait(lambda:self.process.poll() is not None)
        self.assertEqual(self.process.returncode,0,self.drain()[-2000:])
        self.assertEqual(termios.tcgetattr(self.slave),self.original)
        self.assertIn(b'\x1b[?1049l',self.drain())
        with self.assertRaises(ProcessLookupError):os.kill(pid,0)

    def test_signal_exit_restores_tty_and_terminates_owned_child(self):
        pid=self.start();self.process.terminate()
        wait(lambda:self.process.poll() is not None)
        self.assertEqual(termios.tcgetattr(self.slave),self.original)
        with self.assertRaises(ProcessLookupError):os.kill(pid,0)

    @unittest.skipUnless(Path('/bin/zsh').exists(),'zsh not installed')
    def test_real_clean_zsh_prompt_and_fullscreen_escape_defaults(self):
        self.process=subprocess.Popen([sys.executable,str(ROOT/'tools/here.py'),'--cwd',str(self.path),'--','/bin/zsh','-f','-i'],
                                      stdin=self.slave,stdout=self.slave,stderr=self.slave,start_new_session=True)
        wait(lambda:b'hardcore' in self.drain())
        os.write(self.master,b"printf '\\033[?1049h\\033[H\\033[J\\033[31m'; printf ZSH_SCREEN_READY\r")
        wait(lambda:b'ZSH_SCREEN_READY' in self.drain())
        self.assertIsNone(self.process.poll())
        os.write(self.master,b"printf '\\033[m\\033[?1049l'; printf ZSH_RESTORED\r")
        wait(lambda:b'ZSH_RESTORED' in self.drain())
        os.write(self.master,b'\x02qy');wait(lambda:self.process.poll() is not None)
        self.assertEqual(self.process.returncode,0,self.drain()[-1000:])
        self.assertEqual(termios.tcgetattr(self.slave),self.original)

    def test_cleanup_includes_owned_background_process_groups(self):
        log=self.path/'background.pid'
        worker=self.path/'background.py'
        worker.write_text('import os,signal,sys,time\n'
                          'if not os.fork():\n'
                          ' os.setpgrp()\n'
                          ' signal.signal(signal.SIGHUP,signal.SIG_IGN)\n'
                          ' signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
                          ' open(sys.argv[1],"w").write(str(os.getpid()))\n'
                          ' while True:time.sleep(1)\n'
                          'while True:time.sleep(1)\n')
        tab=here.Session([sys.executable,str(worker),str(log)],str(self.path),40,10)
        try:
            wait(lambda:log.exists() and log.read_text())
            job=int(log.read_text())
            self.assertEqual(os.getsid(job),tab.pid)
            tab.close()
            self.assertIsNotNone(tab.exited)
            def gone():
                try:os.kill(job,0);return False
                except ProcessLookupError:return True
            wait(gone)
        finally:tab.close()

    def test_noninteractive_error_and_help(self):
        for args,code in (([],2),(['--help'],0)):
            result=subprocess.run([sys.executable,str(ROOT/'tools/here.py'),*args],capture_output=True,timeout=5)
            self.assertEqual(result.returncode,code)
            self.assertNotIn(b'\x1b[?1049h',result.stdout)
            if not args:self.assertIn(b'interactive terminal',result.stderr)


if __name__=='__main__':unittest.main()
