#!/usr/bin/env python3
"""Run Hardcore's tabbed shells inside the current POSIX terminal.

Ctrl-B then: c new tab, n/p next/previous, s config, / search, D detach,
R reload UI, x close, q end session, ? help. Ctrl-B Ctrl-B sends a literal Ctrl-B.
The host terminal supplies fonts and rendering; no GUI or GPU window opens.
"""
from __future__ import annotations

import argparse
import codecs
from collections import deque
from dataclasses import dataclass, replace
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import selectors
import signal
import struct
import subprocess
import sys
import termios
import time
import tty
import unicodedata
from urllib.parse import urlsplit

# Standalone bundles and tests loaded via importlib need sibling helpers.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from here_terminal import (MAX_TABS, MAX_COLS, MAX_ROWS, DEFAULT_FG, DEFAULT_BG,
    PALETTE, BOLD, DIM, ITALIC, UNDER, BLINK, INVERSE, HIDDEN, STRIKE,
    DEC_GRAPHICS, BEGIN_PASTE, END_PASTE, char_width, clean_text, indexed_color,
    Style, Cell, Buffer, repair_line, text_cells, Terminal, resolve_style)


class InputDecoder:
    """Incremental keyboard/paste framing; pasted Ctrl-B is always literal."""
    def __init__(self, send, command, mouse, focus):
        self.send,self.command,self.mouse,self.focus=send,command,mouse,focus
        self.pending=bytearray()
        self.prefix=False
        self.paste=False
        self.paste_wrapped=False
        self.started=0.0

    def feed(self, data, bracketed=True):
        self.pending.extend(data)
        self.started=time.monotonic()
        while self.pending:
            if self.paste:
                at=self.pending.find(END_PASTE)
                if at>=0:
                    self.send(bytes(self.pending[:at])+(END_PASTE if self.paste_wrapped else b''))
                    del self.pending[:at+len(END_PASTE)];self.paste=False
                    continue
                keep=next((n for n in range(min(len(self.pending),len(END_PASTE)-1),0,-1) if END_PASTE.startswith(self.pending[-n:])),0)
                end=len(self.pending)-keep
                if end:self.send(bytes(self.pending[:end]));del self.pending[:end]
                break
            if self.prefix:
                self.prefix=False;value=self.pending.pop(0)
                if value==2:self.command(-1);self.send(b'\x02')
                else:self.command(value)
                continue
            if self.pending[0]==2:
                del self.pending[0];self.prefix=True;self.command(None);continue
            if self.pending[0]==27:
                if BEGIN_PASTE.startswith(self.pending) and len(self.pending)<len(BEGIN_PASTE):break
                if self.pending.startswith(BEGIN_PASTE):
                    del self.pending[:len(BEGIN_PASTE)];self.paste=True
                    self.paste_wrapped=bracketed() if callable(bracketed) else bracketed
                    if self.paste_wrapped:self.send(BEGIN_PASTE)
                    continue
                if len(self.pending)==1:break
                if self.pending[1]==ord('['):
                    end=next((i for i in range(2,len(self.pending)) if 0x40<=self.pending[i]<=0x7e),None)
                    if end is None:
                        if len(self.pending)>128:self.send(bytes(self.pending));self.pending.clear()
                        break
                    sequence=bytes(self.pending[:end+1]);del self.pending[:end+1]
                    match=re.fullmatch(rb'\x1b\[<(\d+);(\d+);(\d+)([Mm])',sequence)
                    if match:self.mouse(*(int(match[i]) for i in (1,2,3)),match[4]==b'M')
                    elif sequence in (b'\x1b[I',b'\x1b[O'):self.focus(sequence==b'\x1b[I')
                    else:self.send(sequence)
                    continue
            at=next((i for i,c in enumerate(self.pending) if i and c in (2,27)),len(self.pending))
            self.send(bytes(self.pending[:at]));del self.pending[:at]

    def flush_escape(self):
        if self.pending and not self.paste and time.monotonic()-self.started>.04:
            self.send(bytes(self.pending));self.pending.clear()


class Session:
    def __init__(self, command, cwd, cols, rows, close_fds=()):
        self.pending=bytearray()
        self.terminal=Terminal(cols,rows,self.queue)
        self.scroll=0
        self.exited=None
        self.fd=-1
        self.pid,self.fd=pty.fork()
        if not self.pid:
            try:
                for fd in close_fds:
                    try:os.close(fd)
                    except OSError:pass
                for sig in (signal.SIGINT,signal.SIGTERM,signal.SIGHUP,signal.SIGPIPE,signal.SIGTSTP):
                    signal.signal(sig,signal.SIG_DFL)
                os.chdir(cwd)
                fcntl.ioctl(0,termios.TIOCSWINSZ,struct.pack('HHHH',rows,cols,0,0))
                env=dict(os.environ,TERM='xterm-256color',COLORTERM='truecolor',TERM_PROGRAM='hardcore-here')
                for name in ('HARDCORE_CTL','HARDCORE_WINDOW_PID','HARDCORE_TAB_ID'):
                    env.pop(name,None)
                os.execvpe(command[0],command,env)
            except Exception as error:
                os.write(2,f'hardcore here: {error}\r\n'.encode(errors='replace'))
                os._exit(127)
        os.set_blocking(self.fd,False)
        self.resize(cols,rows)
        self.label=Path(command[0]).name

    def queue(self, data):
        if self.fd>=0 and len(self.pending)<2*1024*1024:
            self.pending.extend(data)

    def resize(self, cols, rows):
        self.terminal.resize(cols,rows)
        if self.fd>=0:
            try:fcntl.ioctl(self.fd,termios.TIOCSWINSZ,struct.pack('HHHH',rows,cols,0,0))
            except OSError:pass
        self.scroll=0

    def read(self):
        try:data=os.read(self.fd,65536)
        except BlockingIOError:return True
        except OSError as error:
            if error.errno==errno.EIO:return False
            raise
        if not data:return False
        before=self.terminal.history_pushed
        self.terminal.feed(data)
        if self.scroll:self.scroll=min(len(self.terminal.history),self.scroll+self.terminal.history_pushed-before)
        return True

    def write(self):
        if not self.pending:return
        try:n=os.write(self.fd,self.pending)
        except BlockingIOError:return
        except OSError as error:
            if error.errno in (errno.EIO,errno.EPIPE):return
            raise
        del self.pending[:n]

    def poll(self):
        if self.exited is not None:return
        try:pid,status=os.waitpid(self.pid,os.WNOHANG)
        except ChildProcessError:self.exited=0;return
        if pid:self.exited=os.waitstatus_to_exitcode(status)

    def close(self):
        # Closing the PTY sends a hangup to its controlling session. Explicit
        # group signals also cover the owned shell and its foreground program.
        groups={self.pid} if self.exited is None or self.fd>=0 else set()
        if groups:
            # Include owned background jobs in their own process groups. The
            # session created by forkpty is private to this tab; no parent or
            # neighboring terminal can match it. Do this before reaping/close.
            try:
                result=subprocess.run(['ps','-A','-o','pid='],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=.3)
                for value in result.stdout.split():
                    try:
                        pid=int(value)
                        if os.getsid(pid)==self.pid:groups.add(os.getpgid(pid))
                    except (OSError,ValueError):pass
            except (OSError,subprocess.TimeoutExpired):pass
        if self.fd>=0:
            try:
                foreground=os.tcgetpgrp(self.fd)
                if foreground>0 and os.getsid(foreground)==self.pid:groups.add(foreground)
            except OSError:pass
            try:os.close(self.fd)
            except OSError:pass
            self.fd=-1
        groups={group for group in groups if group>1 and group!=os.getpgrp()}
        def signal_groups(sig):
            for group in tuple(groups):
                try:os.killpg(group,sig)
                except (ProcessLookupError,PermissionError):
                    # Darwin reports EPERM, not ESRCH, when the PTY hangup
                    # already killed a group and only its zombie remains.
                    # Never retry a group we can no longer signal.
                    groups.discard(group)
        signal_groups(signal.SIGHUP)
        for next_signal in (signal.SIGTERM,signal.SIGKILL):
            deadline=time.monotonic()+.4
            while groups and time.monotonic()<deadline:
                self.poll()
                signal_groups(0)
                if groups:time.sleep(.01)
            signal_groups(next_signal)
        # Reap without an unbounded wait if the child changed credentials or
        # exited between the group query and hangup. The host is already safe.
        deadline=time.monotonic()+.2
        while self.exited is None and time.monotonic()<deadline:
            self.poll()
            if self.exited is None:time.sleep(.01)


def style_escape(style, theme=None):
    if theme is not None:style=resolve_style(style,theme.palette,theme.fg,theme.bg)
    codes=['0']
    for bit,code in ((BOLD,1),(DIM,2),(ITALIC,3),(UNDER,4),(BLINK,5),(INVERSE,7),(HIDDEN,8),(STRIKE,9)):
        if style.flags&bit:codes.append(str(code))
    codes += ['38','2',*(str(x) for x in (style.fg or DEFAULT_FG)), '48','2',*(str(x) for x in (style.bg or DEFAULT_BG))]
    return '\x1b['+';'.join(codes)+'m'


def render_line(cells, theme=None):
    result=[];style=None;link=''
    for cell in cells:
        if cell.width==0:continue
        if cell.style!=style:
            style=cell.style
            if style.link!=link:
                if link:result.append('\x1b]8;;\x1b\\')
                link=style.link
                if link:result.append('\x1b]8;;'+link+'\x1b\\')
            result.append(style_escape(style,theme))
        result.append(cell.text)
    if link:result.append('\x1b]8;;\x1b\\')
    return ''.join(result)


class HostTerminal:
    ENTER=b'\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?2004h\x1b[?1004h\x1b[?1006h\x1b[?1000h'
    LEAVE=b'\x1b]8;;\x1b\\\x1b[0m\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1004l\x1b[?2004l\x1b[?1l\x1b>\x1b[0 q\x1b[?7h\x1b[?25h\x1b[?1049l'
    def __init__(self, input_fd=0, output_fd=1):
        self.input_fd,self.output_fd=input_fd,output_fd
        self.saved=None
        self.active=False

    def write(self, data):
        if isinstance(data,str):data=data.encode('utf-8',errors='replace')
        while data:
            try:n=os.write(self.output_fd,data)
            except InterruptedError:continue
            data=data[n:]

    def enter(self):
        if self.saved is None:self.saved=termios.tcgetattr(self.input_fd)
        self.active=True
        tty.setraw(self.input_fd,termios.TCSANOW)
        self.write(self.ENTER)

    def restore(self):
        if not self.active:return
        self.active=False
        try:self.write(self.LEAVE)
        except OSError:pass
        finally:
            if self.saved is not None:
                try:termios.tcsetattr(self.input_fd,termios.TCSANOW,self.saved)
                except OSError:pass


class Application:
    HELP=("Hardcore here — your terminal, independent shells", "", "Ctrl-B c       New shell tab", "Ctrl-B n / p   Next / previous tab", "Ctrl-B 1..9    Choose a tab", "Ctrl-B x       Close tab (confirmation)", "Ctrl-B q       Quit (confirmation)", "Ctrl-B u / d   Scroll history a page", "Ctrl-B ]       Return to live output", "Ctrl-B Ctrl-B  Send a literal Ctrl-B", "", "Click a tab below. Shift-drag uses host text selection.", "Fonts and pixel size are controlled by your host terminal.", "Press any key to return.")
    def __init__(self, shell, cwd, command=None, host=None):
        self.host=host or HostTerminal()
        self.shell,self.cwd=shell,cwd
        self.command=command or [shell,'-i']
        self.tabs=[];self.active=0
        self.stop=False;self.resize_pending=True;self.suspend=False
        self.confirm=None;self.help=False;self.prefix=False
        self.focused=True
        self.cols,self.rows=80,23
        self.previous=[];self.status_hits=[];self.message=''
        self.last_paint=0.0;self.dirty=True;self.last_modes=None
        self.selector=selectors.DefaultSelector()
        self.decoder=InputDecoder(self.send,self.control,self.mouse,self.focus)
        self.old_signals={}

    @property
    def current(self):return self.tabs[self.active]

    def spawn(self, command=None):
        if len(self.tabs)>=MAX_TABS:self.message=f'At most {MAX_TABS} tabs';return
        try:session=Session(command or [self.shell,'-i'],self.cwd,self.cols,self.rows,[t.fd for t in self.tabs if t.fd>=0])
        except OSError as error:
            if not self.tabs:raise
            self.message=f'Cannot open tab: {error}';self.dirty=True;return
        self.tabs.append(session)
        self.choose(len(self.tabs)-1)
        self.selector.register(session.fd,selectors.EVENT_READ,session)

    def choose(self, at):
        if not self.tabs:return
        old=self.tabs[self.active] if self.active<len(self.tabs) else None
        if old and old.terminal.focus and self.focused:old.queue(b'\x1b[O')
        self.active=at%len(self.tabs)
        if self.current.terminal.focus and self.focused:self.current.queue(b'\x1b[I')
        self.previous=[];self.dirty=True;self.message='';self.last_modes=None

    def control(self, key):
        if self.stop:return
        self.prefix=key is None;self.dirty=True
        if key is None:return
        if self.help:self.help=False;self.previous=[];return
        if self.confirm:self.confirm=None;return
        if key==-1:return
        if key==ord('c'):self.spawn()
        elif key in (ord('n'),ord('p')):self.choose(self.active+(1 if key==ord('n') else -1))
        elif ord('1')<=key<=ord('9') and key-ord('1')<len(self.tabs):self.choose(key-ord('1'))
        elif key in (ord('x'),ord('q')):self.confirm=chr(key)
        elif key==ord('?'):self.help=True
        elif key in (ord('u'),ord('[')):self.current.scroll=min(len(self.current.terminal.history),self.current.scroll+self.rows)
        elif key==ord('d'):self.current.scroll=max(0,self.current.scroll-self.rows)
        elif key==ord(']'):self.current.scroll=0
        else:self.message='Unknown shortcut. Ctrl-B ? for help'

    def send(self, data):
        if self.stop:return
        if self.help:self.help=False;self.previous=[];self.dirty=True;return
        if self.confirm:
            action=self.confirm;self.confirm=None;self.dirty=True
            if data[:1].lower()==b'y':
                if action=='q':self.stop=True
                else:self.close_current()
            return
        if not self.tabs:return
        if self.current.scroll:
            if data in (b'\x1b[5~',b'\x1b[A'):self.current.scroll=min(len(self.current.terminal.history),self.current.scroll+(self.rows if data==b'\x1b[5~' else 1));self.dirty=True;return
            if data in (b'\x1b[6~',b'\x1b[B'):self.current.scroll=max(0,self.current.scroll-(self.rows if data==b'\x1b[6~' else 1));self.dirty=True;return
            self.current.scroll=0
        self.current.queue(data);self.dirty=True

    def close_current(self):
        tab=self.current
        if tab.fd>=0:
            try:self.selector.unregister(tab.fd)
            except KeyError:pass
        tab.close();self.tabs.pop(self.active)
        if not self.tabs:self.stop=True
        else:self.choose(min(self.active,len(self.tabs)-1))

    def focus(self, inside):
        if self.stop:return
        self.focused=inside
        if self.tabs and self.current.terminal.focus:self.current.queue(b'\x1b[I' if inside else b'\x1b[O')

    def mouse(self, button, x, y, down):
        if self.stop or not self.tabs:return
        if self.help or self.confirm:return
        if not 0<=button<=255:return
        if y==self.rows+1:
            if down and button==0:
                for left,right,index in self.status_hits:
                    if left<=x-1<right:self.choose(index);break
            return
        if not (1<=x<=self.cols and 1<=y<=self.rows):return
        tab=self.current;term=tab.terminal
        if not term.mouse:
            if down and button in (64,65):
                tab.scroll=max(0,min(len(term.history),tab.scroll+(3 if button==64 else -3)));self.dirty=True
            return
        if button&32 and (term.mouse==1000 or (term.mouse==1002 and button&3==3)):return
        if term.mouse_sgr:tab.queue(f'\x1b[<{button};{x};{y}{"M" if down else "m"}'.encode())
        elif x<224 and y<224:tab.queue(b'\x1b[M'+bytes((32+(button if down else 3),32+x,32+y)))

    def resize(self):
        size=os.get_terminal_size(self.host.output_fd)
        self.cols=max(2,min(MAX_COLS,size.columns))
        self.rows=max(1,min(MAX_ROWS,size.lines-1))
        for tab in self.tabs:tab.resize(self.cols,self.rows)
        self.previous=[];self.dirty=True;self.resize_pending=False

    def status(self):
        base=Style(fg=(168,153,132),bg=(60,56,54))
        text=' hardcore '
        cells=[Cell(c,1,base) for c in text[:self.cols]]
        self.status_hits=[]
        if self.confirm:text='Quit all tabs? y/N' if self.confirm=='q' else 'Close this shell and its programs? y/N'
        elif self.prefix:text='Ctrl-B: c new · n/p switch · x close · q quit · ? help'
        elif self.message:text=self.message
        else:text=''
        if text:
            cells=text_cells(text,self.cols,base)
            return cells+[Cell(style=base)]*(self.cols-len(cells))
        room=max(1,self.cols-len(cells)-7)
        count=min(len(self.tabs),max(1,room//8))
        start=max(0,min(self.active-count+1,len(self.tabs)-count))
        label_width=max(4,room//max(1,count))
        for index in range(start,start+count):
            tab=self.tabs[index]
            marker='×' if tab.exited is not None else '↑' if tab.scroll else ''
            label=f' {index+1} {tab.terminal.title or tab.label}{marker} '
            label=clean_text(label,label_width)
            style=Style(fg=DEFAULT_BG,bg=(250,189,47),flags=BOLD) if index==self.active else base
            left=len(cells)
            cells.extend(text_cells(label,label_width,style))
            self.status_hits.append((left,len(cells),index))
        while len(cells)<self.cols:cells.append(Cell(style=base))
        for offset,char in enumerate(' ^B ? '):
            x=self.cols-6+offset
            if x>=len(text) and x>=0:cells[x]=Cell(char,1,base)
        return cells[:self.cols]

    def paint(self):
        tab=self.current;term=tab.terminal
        grid=[list(row) for row in term.visible(tab.scroll)]
        if self.help:
            width=min(self.cols,max(len(line) for line in self.HELP)+4)
            left=max(0,(self.cols-width)//2);top=max(0,(self.rows-len(self.HELP))//2)
            for n,line in enumerate(self.HELP[:self.rows]):
                row=top+n
                if row>=self.rows:break
                style=Style(fg=DEFAULT_FG,bg=(60,56,54),flags=BOLD if n==0 else 0)
                label=clean_text(' '+line,width)
                overlay=[Cell(c,1,style) for c in label]
                overlay += [Cell(style=style)]*max(0,width-len(overlay))
                grid[row][left:left+width]=overlay[:width]
        grid.append(self.status())
        output=['\x1b[?25l']
        for row,cells in enumerate(grid):
            if row>=len(self.previous) or cells!=self.previous[row]:
                output.append(f'\x1b[{row+1};1H'+render_line(cells))
        self.previous=grid
        modes=(term.app_cursor,term.app_keypad,term.mouse,term.cursor_shape)
        if modes!=self.last_modes:
            output.append('\x1b[?1000l\x1b[?1002l\x1b[?1003l')
            output.append(f'\x1b[?{term.mouse or 1000}h\x1b[?1{"h" if term.app_cursor else "l"}')
            output.append('\x1b=' if term.app_keypad else '\x1b>')
            output.append(f'\x1b[{term.cursor_shape} q');self.last_modes=modes
        if not self.help and not self.confirm and not tab.scroll and term.cursor_visible:
            output.append(f'\x1b[{term.screen.y+1};{term.screen.x+1}H\x1b[?25h')
        self.host.write(''.join(output))
        self.last_paint=time.monotonic();self.dirty=False

    def request_stop(self, signum, frame):
        self.stop=True

    def run(self):
        try:
            for sig in (signal.SIGINT,signal.SIGTERM,signal.SIGHUP):
                self.old_signals[sig]=signal.signal(sig,self.request_stop)
            self.old_signals[signal.SIGWINCH]=signal.signal(signal.SIGWINCH,lambda *_:setattr(self,'resize_pending',True))
            self.old_signals[signal.SIGTSTP]=signal.signal(signal.SIGTSTP,lambda *_:setattr(self,'suspend',True))
            self.host.enter();self.resize();self.spawn(self.command)
            self.selector.register(self.host.input_fd,selectors.EVENT_READ,None)
            while not self.stop:
                if self.suspend:
                    self.host.restore();os.kill(os.getpid(),signal.SIGSTOP)
                    self.suspend=False;self.host.enter();self.resize_pending=True;self.previous=[];self.last_modes=None
                if self.resize_pending:self.resize()
                self.decoder.flush_escape()
                for tab in self.tabs:
                    status=tab.exited
                    tab.poll()
                    if tab.exited!=status:self.dirty=True
                    if tab.fd>=0:self.selector.modify(tab.fd,selectors.EVENT_READ|(selectors.EVENT_WRITE if tab.pending else 0),tab)
                # Bound pasted input while a child is not reading its PTY.
                listening=self.current.fd<0 or len(self.current.pending)<1024*1024
                registered=self.host.input_fd in self.selector.get_map()
                if listening and not registered:self.selector.register(self.host.input_fd,selectors.EVENT_READ,None)
                elif not listening and registered:self.selector.unregister(self.host.input_fd)
                now=time.monotonic()
                if self.dirty and now-self.last_paint>=1/60:self.paint()
                for key,events in self.selector.select(.016 if self.dirty or self.decoder.pending else .1):
                    if self.stop:break
                    if key.data is None:
                        data=os.read(self.host.input_fd,65536)
                        if not data:self.stop=True;break
                        self.decoder.feed(data,lambda:bool(self.tabs) and self.current.terminal.bracketed_paste);self.dirty=True
                    else:
                        tab=key.data
                        # Input earlier in this same readiness batch may have
                        # closed this tab (and a new PTY may reuse its fd).
                        if tab.fd<0 or tab.fd!=key.fd or tab not in self.tabs:continue
                        if events&selectors.EVENT_READ:
                            if not tab.read():
                                self.selector.unregister(tab.fd);os.close(tab.fd);tab.fd=-1
                                tab.poll()
                                if len(self.tabs)==1:self.stop=True
                            self.dirty=True
                        if events&selectors.EVENT_WRITE and tab.fd>=0:tab.write()
        finally:
            # Restore the host before waiting for owned shells to exit.
            self.host.restore()
            for tab in self.tabs:tab.close()
            self.selector.close()
            for sig,handler in self.old_signals.items():signal.signal(sig,handler)
        return 0


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cwd',default=os.getcwd(),help='directory for the first shell')
    parser.add_argument('--shell',default=os.environ.get('SHELL') or '/bin/sh',help='interactive shell for new tabs')
    mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--session',metavar='NAME',help='create or attach a named persistent session')
    mode.add_argument('--attach',metavar='NAME',help='attach an existing session')
    mode.add_argument('--list',action='store_true',help='list live terminal-hosted sessions')
    mode.add_argument('--stop',metavar='NAME',help='end a session and its owned programs')
    parser.add_argument('--takeover',action='store_true',help='explicitly replace an attached frontend')
    parser.add_argument('command',nargs=argparse.REMAINDER,help='optional command, following --')
    args=parser.parse_args(argv)
    from here_broker import BrokerClient,BrokerError,start_session,status_session,list_sessions,stop_session
    from here_frontend import StudioApplication
    try:
        if args.list:
            sessions=list_sessions()
            for state in sessions:
                print(f"{state['name']}  {len(state['tabs'])} tabs  "
                      f"{'attached' if state.get('attached') else 'detached'}")
            if not sessions:print('No terminal-hosted Hardcore sessions.')
            return 0
        if args.stop:stop_session(args.stop);return 0
        if not os.isatty(0) or not os.isatty(1):
            parser.exit(2,'hardcore here needs an interactive terminal on stdin and stdout. Run it directly in Terminal, a Linux terminal, or WSL.\n')
        cwd=str(Path(args.cwd).expanduser().resolve())
        if not Path(cwd).is_dir():parser.exit(2,f'hardcore here: not a directory: {cwd}\n')
        command=args.command[1:] if args.command[:1]==['--'] else args.command
        size=os.get_terminal_size(1)
        cols=max(2,min(500,size.columns)-(29 if size.columns>=76 else 0))
        rows=max(1,min(200,size.lines-1))
        name=args.attach or args.session
        if name:
            try:status_session(name)
            except (FileNotFoundError,ConnectionRefusedError):
                if args.attach:raise BrokerError(f'Session {name} does not exist')
                name=start_session(name,shell=args.shell,cwd=cwd,command=command or None,cols=cols,rows=rows)
        else:name=start_session(shell=args.shell,cwd=cwd,command=command or None,cols=cols,rows=rows)
        client=BrokerClient(name,takeover=args.takeover)
        return StudioApplication(client,name).run()
    except (OSError,ValueError,BrokerError) as error:
        print(f'hardcore here: {error}',file=sys.stderr)
        return 1


if __name__=='__main__':
    raise SystemExit(main())
