#!/usr/bin/env python3
"""Run Hardcore's tabbed shells inside the current POSIX terminal.

Ctrl-B then: c new tab, n/p next/previous, 1..9 select, x close, q quit,
u/d scroll history, ] return live, ? help. Ctrl-B Ctrl-B sends a literal Ctrl-B.
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

MAX_TABS = 16
MAX_COLS = 500
MAX_ROWS = 200
DEFAULT_FG = (235, 219, 178)
DEFAULT_BG = (40, 40, 40)
PALETTE = ((40,40,40),(204,36,29),(152,151,26),(215,153,33),(69,133,136),(177,98,134),(104,157,106),(168,153,132),
           (146,131,116),(251,73,52),(184,187,38),(250,189,47),(131,165,152),(211,134,155),(142,192,124),(235,219,178))
BOLD, DIM, ITALIC, UNDER, BLINK, INVERSE, HIDDEN, STRIKE = (1 << i for i in range(8))
DEC_GRAPHICS = dict(zip('`abcdefghijklmnopqrstuvwxyz{|}~', '◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·'))
BEGIN_PASTE = b'\x1b[200~'
END_PASTE = b'\x1b[201~'


def char_width(char):
    if unicodedata.combining(char) or char in ('\u200d', '\ufe0e', '\ufe0f'):
        return 0
    return 2 if unicodedata.east_asian_width(char) in 'WF' else 1


def clean_text(text, columns):
    result, used = [], 0
    for char in text:
        if unicodedata.category(char) in ('Cc', 'Cs', 'Cf'):
            continue
        width = char_width(char)
        if used + width > columns:
            break
        if width or result:
            result.append(char)
            used += width
    return ''.join(result)


def indexed_color(index):
    index = max(0, min(255, index))
    if index < 16:
        return PALETTE[index]
    if index >= 232:
        return (8 + (index-232)*10,) * 3
    index -= 16
    levels = (0, 95, 135, 175, 215, 255)
    return levels[index//36], levels[(index//6)%6], levels[index%6]


@dataclass(frozen=True, slots=True)
class Style:
    fg: tuple | None = None
    bg: tuple | None = None
    flags: int = 0
    link: str = ''


@dataclass(frozen=True, slots=True)
class Cell:
    text: str = ' '
    width: int = 1  # zero denotes the continuation of a double-width glyph
    style: Style = Style()


class Buffer:
    def __init__(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.grid = [[Cell()] * cols for _ in range(rows)]
        self.x = self.y = 0
        self.top, self.bottom = 0, rows-1
        self.wrap = False
        self.saved = None

    def resize(self, cols, rows):
        self.grid = [line[:cols] + [Cell()] * max(0, cols-len(line)) for line in self.grid[:rows]]
        self.grid += [[Cell()] * cols for _ in range(rows-len(self.grid))]
        self.cols, self.rows = cols, rows
        for line in self.grid:
            repair_line(line)
        self.x, self.y = min(self.x, cols-1), min(self.y, rows-1)
        self.top, self.bottom, self.wrap = 0, rows-1, False


def repair_line(line):
    for x, cell in enumerate(line):
        if (cell.width == 2 and (x+1 == len(line) or line[x+1].width != 0)) or \
           (cell.width == 0 and (x == 0 or line[x-1].width != 2)):
            line[x] = Cell(style=cell.style)


def text_cells(text, columns, style):
    result=[]
    for char in clean_text(text,columns):
        width=char_width(char)
        if not width:
            if result:
                at=len(result)-1 if result[-1].width else len(result)-2
                if len(result[at].text)<64:result[at]=replace(result[at],text=result[at].text+char)
        else:
            result.append(Cell(char,width,style))
            if width==2:result.append(Cell('',0,style))
    return result


class Terminal:
    """Bounded VT screen state. Application escape strings never reach the host.

    Standard ANSI/xterm cursor, erase, scroll, style, alternate-screen and mode
    operations are interpreted. Unknown control strings are discarded through
    their terminator, including split and oversized OSC/DCS sequences.
    """
    def __init__(self, cols, rows, respond=lambda data: None):
        self.cols, self.rows = cols, rows
        self.normal, self.alternate = Buffer(cols, rows), Buffer(cols, rows)
        self.alt = False
        self.style = Style()
        self.history = deque(maxlen=max(1, min(1000, 100000//cols)))
        self.history_pushed = 0
        self.respond = respond
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')
        self.state, self.control = 'text', ''
        self.string_esc = False
        self.string_kind = ''
        self.title = ''
        self.cursor_visible, self.cursor_shape = True, 0
        self.autowrap, self.origin, self.insert = True, False, False
        self.app_cursor = self.app_keypad = self.bracketed_paste = self.focus = False
        self.mouse = 0
        self.mouse_sgr = False
        self.g0 = self.g1 = 'B'
        self.charset = 0
        self.tabstops = set(range(8, cols, 8))
        self.revision = 0

    @property
    def screen(self):
        return self.alternate if self.alt else self.normal

    def resize(self, cols, rows):
        cols, rows = max(2, min(MAX_COLS, cols)), max(1, min(MAX_ROWS, rows))
        for buffer in (self.normal, self.alternate):
            buffer.resize(cols, rows)
        self.tabstops.update(range(((self.cols+7)//8)*8, cols, 8))
        self.tabstops = {x for x in self.tabstops if 0 <= x < cols}
        self.cols, self.rows = cols, rows
        self.revision += 1

    def blank_line(self):
        return [Cell(style=self.style)] * self.cols

    def scroll_up(self, count=1):
        s = self.screen
        for _ in range(min(max(1, count), s.bottom-s.top+1)):
            line = s.grid.pop(s.top)
            if not self.alt and s.top == 0 and s.bottom == self.rows-1:
                self.history.append(line)
                self.history_pushed += 1
            s.grid.insert(s.bottom, self.blank_line())

    def scroll_down(self, count=1):
        s = self.screen
        for _ in range(min(max(1, count), s.bottom-s.top+1)):
            s.grid.pop(s.bottom)
            s.grid.insert(s.top, self.blank_line())

    def index(self, reverse=False):
        s = self.screen
        s.wrap = False
        if reverse:
            if s.y == s.top:
                self.scroll_down()
            else:
                s.y = max(0, s.y-1)
        elif s.y == s.bottom:
            self.scroll_up()
        else:
            s.y = min(self.rows-1, s.y+1)

    def clear_cell(self, line, x):
        if line[x].width == 0 and x:
            line[x-1] = Cell(style=self.style)
        if line[x].width == 2 and x+1 < self.cols:
            line[x+1] = Cell(style=self.style)
        line[x] = Cell(style=self.style)

    def put(self, char):
        s = self.screen
        if unicodedata.category(char) in ('Cc','Cs','Cf') and char != '\u200d':
            return
        if (self.g1 if self.charset else self.g0) == '0':
            char = DEC_GRAPHICS.get(char, char)
        width = char_width(char)
        previous = s.x if s.wrap else s.x-1
        if previous >= 0:
            if s.grid[s.y][previous].width == 0:
                previous -= 1
            prior = s.grid[s.y][previous]
            if not width or prior.text.endswith('\u200d'):
                if len(prior.text)<64:
                    s.grid[s.y][previous] = replace(prior, text=prior.text+char)
                return
        if not width:
            return
        if s.wrap and self.autowrap:
            s.x = 0
            self.index()
        s.wrap = False
        if width == 2 and s.x == self.cols-1:
            if self.autowrap:
                s.x = 0
                self.index()
            else:
                char, width = '\ufffd', 1
        line = s.grid[s.y]
        if self.insert:
            line[s.x:s.x] = [Cell(style=self.style)] * width
            del line[self.cols:]
            repair_line(line)
        self.clear_cell(line, s.x)
        if width == 2:
            self.clear_cell(line, s.x+1)
        line[s.x] = Cell(char, width, self.style)
        if width == 2:
            line[s.x+1] = Cell('', 0, self.style)
        if s.x+width >= self.cols:
            s.x, s.wrap = self.cols-1, True
        else:
            s.x += width

    def save(self):
        s = self.screen
        s.saved = (s.x, s.y, self.style, s.wrap, self.origin)

    def restore(self):
        s = self.screen
        if s.saved:
            x, y, self.style, s.wrap, self.origin = s.saved
            s.x, s.y = min(x, self.cols-1), min(y, self.rows-1)

    def position(self, row=1, col=1):
        s = self.screen
        low, high = (s.top, s.bottom) if self.origin else (0, self.rows-1)
        s.y = max(low, min(high, low+row-1))
        s.x = max(0, min(self.cols-1, col-1))
        s.wrap = False

    def erase_line(self, mode):
        s = self.screen
        left, right = (s.x, self.cols) if mode == 0 else (0, s.x+1) if mode == 1 else (0, self.cols)
        for x in range(left, right):
            self.clear_cell(s.grid[s.y], x)
        s.wrap = False

    def erase_display(self, mode):
        s = self.screen
        if mode == 3:
            self.history.clear()
            return
        if mode == 2:
            s.grid = [self.blank_line() for _ in range(self.rows)]
        elif mode in (0, 1):
            self.erase_line(mode)
            for y in (range(s.y+1, self.rows) if mode == 0 else range(s.y)):
                s.grid[y] = self.blank_line()
        s.wrap = False

    def set_mode(self, private, values, enabled):
        for mode in values:
            if not private:
                if mode == 4:
                    self.insert = enabled
                continue
            if mode == 1:
                self.app_cursor = enabled
            elif mode == 6:
                self.origin = enabled
                self.position()
            elif mode == 7:
                self.autowrap = enabled
            elif mode == 25:
                self.cursor_visible = enabled
            elif mode in (47, 1047, 1049):
                if enabled and not self.alt:
                    if mode == 1049:
                        self.save()
                        self.alternate = Buffer(self.cols, self.rows)
                    self.alt = True
                elif not enabled and self.alt:
                    self.alt = False
                    if mode == 1049:
                        self.restore()
            elif mode == 1048:
                self.save() if enabled else self.restore()
            elif mode in (1000, 1002, 1003):
                self.mouse = mode if enabled else (0 if self.mouse == mode else self.mouse)
            elif mode == 1006:
                self.mouse_sgr = enabled
            elif mode == 1004:
                self.focus = enabled
            elif mode == 2004:
                self.bracketed_paste = enabled

    def sgr(self, raw):
        # Both semicolon RGB and ISO colon RGB forms (with optional colorspace).
        raw = re.sub(r'(38|48):2:(?:0)?:', r'\1;2;', raw)
        raw = raw.replace(':', ';')
        try:
            values = [int(x or 0) for x in raw.split(';')]
        except ValueError:
            return
        i = 0
        while i < len(values):
            value = values[i]
            if value == 0:
                self.style = Style(link=self.style.link)
            elif value in (1,2,3,4,5,7,8,9):
                bit = {1:BOLD,2:DIM,3:ITALIC,4:UNDER,5:BLINK,7:INVERSE,8:HIDDEN,9:STRIKE}[value]
                self.style = replace(self.style, flags=self.style.flags|bit)
            elif value in (22,23,24,25,27,28,29):
                bit = {22:BOLD|DIM,23:ITALIC,24:UNDER,25:BLINK,27:INVERSE,28:HIDDEN,29:STRIKE}[value]
                self.style = replace(self.style, flags=self.style.flags & ~bit)
            elif value in (39,49):
                self.style = replace(self.style, **{'fg' if value == 39 else 'bg': None})
            elif 30 <= value <= 37 or 90 <= value <= 97:
                self.style = replace(self.style, fg=indexed_color(value-30 if value < 90 else value-90+8))
            elif 40 <= value <= 47 or 100 <= value <= 107:
                self.style = replace(self.style, bg=indexed_color(value-40 if value < 100 else value-100+8))
            elif value in (38,48) and i+1 < len(values):
                mode = values[i+1]
                color = None
                if mode == 5 and i+2 < len(values):
                    color, i = indexed_color(values[i+2]), i+2
                elif mode == 2 and i+4 < len(values):
                    color, i = tuple(max(0,min(255,x)) for x in values[i+2:i+5]), i+4
                if color is not None:
                    self.style = replace(self.style, **{'fg' if value == 38 else 'bg': color})
            i += 1

    def csi(self, sequence):
        final, raw = sequence[-1], sequence[:-1]
        private = bool(raw) and raw[0] in '?><='
        prefix = raw[0] if private else ''
        if private:
            raw = raw[1:]
        intermediate = ''.join(c for c in raw if ' ' <= c <= '/')
        raw = ''.join(c for c in raw if c not in intermediate) if intermediate else raw
        if final == 'm' and not private:
            self.sgr(raw)
            return
        try:
            values = [min(1000000,int(x or 0)) for x in raw.split(';')]
        except ValueError:
            return
        n = max(1, values[0])
        s = self.screen
        low, high = (s.top,s.bottom) if self.origin else (0,self.rows-1)
        if final in 'ABeCDaEFG`Hfd':
            if final == 'A': s.y = max(low,s.y-n)
            elif final in 'Be': s.y = min(high,s.y+n)
            elif final in 'Ca': s.x = min(self.cols-1,s.x+n)
            elif final == 'D': s.x = max(0,s.x-n)
            elif final == 'E': s.y,s.x = min(high,s.y+n),0
            elif final == 'F': s.y,s.x = max(low,s.y-n),0
            elif final in 'G`': s.x = min(self.cols-1,n-1)
            elif final == 'd': self.position(n,s.x+1)
            else: self.position(n,max(1,values[1]) if len(values)>1 else 1)
            s.wrap = False
        elif final == 'J': self.erase_display(values[0])
        elif final == 'K': self.erase_line(values[0])
        elif final in 'hl': self.set_mode(prefix == '?',values,final == 'h')
        elif final == 's' and not private: self.save()
        elif final == 'u' and not private: self.restore()
        elif final == 'u' and prefix == '?': self.respond(b'\x1b[?0u')
        elif final == 'r' and not private:
            top = n-1
            bottom = (values[1] or self.rows)-1 if len(values)>1 else self.rows-1
            if 0 <= top < bottom < self.rows:
                s.top,s.bottom = top,bottom
                self.position()
        elif final in 'LM' and s.top <= s.y <= s.bottom:
            for _ in range(min(n,s.bottom-s.y+1)):
                if final == 'L': s.grid.pop(s.bottom);s.grid.insert(s.y,self.blank_line())
                else: s.grid.pop(s.y);s.grid.insert(s.bottom,self.blank_line())
        elif final in '@PX':
            n = min(n,self.cols-s.x)
            line = s.grid[s.y]
            if final == '@': line[s.x:s.x] = [Cell(style=self.style)]*n;del line[self.cols:]
            elif final == 'P': del line[s.x:s.x+n];line.extend([Cell(style=self.style)]*n)
            else:
                for x in range(s.x,s.x+n): self.clear_cell(line,x)
            repair_line(line)
        elif final == 'S': self.scroll_up(n)
        elif final == 'T': self.scroll_down(n)
        elif final in 'IZ':
            stops = sorted(self.tabstops,reverse=final=='Z')
            for _ in range(min(n,self.cols)):
                choices = [x for x in stops if x>s.x] if final == 'I' else [x for x in stops if x<s.x]
                s.x = choices[0] if choices else self.cols-1 if final=='I' else 0
            s.wrap = False
        elif final == 'g':
            if values[0] == 0: self.tabstops.discard(s.x)
            elif values[0] == 3: self.tabstops.clear()
        elif final == 'n':
            if values[0] == 5: self.respond(b'\x1b[0n')
            elif values[0] == 6:
                row = s.y-s.top+1 if self.origin else s.y+1
                self.respond(f'\x1b[{"?" if prefix=="?" else ""}{row};{s.x+1}R'.encode())
        elif final == 'c':
            self.respond(b'\x1b[>0;0;0c' if prefix == '>' else b'\x1b[?1;2c')
        elif final == 'q' and intermediate == ' ': self.cursor_shape = values[0] if 0 <= values[0] <= 6 else 0
        elif final == 't' and values[0] == 18: self.respond(f'\x1b[8;{self.rows};{self.cols}t'.encode())
        elif final == 'p' and intermediate == '$' and prefix == '?':
            self.respond(f'\x1b[?{values[0]};0$y'.encode())

    def osc(self, text):
        code, _, value = text.partition(';')
        if code in ('0','2'):
            self.title = clean_text(value,80)
        elif code == '8':
            _, _, url = value.partition(';')
            try:
                allowed = urlsplit(url).scheme in ('http','https','mailto','file')
            except ValueError:
                allowed = False
            if len(url)>2048 or any(unicodedata.category(c) in ('Cc','Cs') for c in url): allowed=False
            self.style = replace(self.style,link=url if allowed else '')
        elif code in ('10','11') and value == '?':
            rgb = DEFAULT_FG if code == '10' else DEFAULT_BG
            color = '/'.join(f'{x*257:04x}' for x in rgb)
            self.respond(f'\x1b]{code};rgb:{color}\x1b\\'.encode())
        elif code == '4':
            parts = value.split(';')
            if len(parts)==2 and len(parts[0])<=3 and parts[0].isdigit() and parts[1]=='?':
                rgb=indexed_color(min(255,int(parts[0])))
                color='/'.join(f'{x*257:04x}' for x in rgb)
                self.respond(f'\x1b]4;{parts[0]};rgb:{color}\x1b\\'.encode())

    def feed(self, data):
        text = self.decoder.decode(data)
        for char in text:
            code = ord(char)
            if self.state in ('osc','discard'):
                if self.string_esc:
                    self.string_esc = False
                    if char == '\\':
                        if self.state == 'osc': self.osc(self.control)
                        self.state,self.control='text',''
                        continue
                if char == '\x1b': self.string_esc=True
                elif char == '\x07' and self.string_kind == 'osc':
                    if self.state == 'osc':self.osc(self.control)
                    self.state,self.control='text',''
                elif char in ('\x18','\x1a'):
                    self.state,self.control='text',''
                elif self.state == 'osc':
                    if len(self.control)<16384: self.control += char
                    else: self.state,self.control='discard',''
                continue
            if char == '\x1b': self.state,self.control='escape','';continue
            if char in ('\x18','\x1a'): self.state,self.control='text','';continue
            if self.state == 'escape_intermediate':
                if '0' <= char <= '~':self.state='text'
                continue
            if self.state == 'csi':
                if '@' <= char <= '~':
                    if len(self.control)<256: self.csi(self.control+char)
                    self.state,self.control='text',''
                elif len(self.control)<256: self.control+=char
                continue
            if self.state == 'charset':
                if self.control == '(': self.g0=char
                else: self.g1=char
                self.state,self.control='text',''
                continue
            if self.state == 'escape':
                self.state='text'
                if char == '[': self.state='csi'
                elif char == ']': self.state='osc';self.string_kind='osc';self.string_esc=False
                elif char in 'PX^_': self.state='discard';self.string_kind='dcs';self.string_esc=False
                elif char in '()*+': self.state,self.control='charset',char
                elif ' ' <= char <= '/': self.state='escape_intermediate'
                elif char == '7': self.save()
                elif char == '8': self.restore()
                elif char == 'D': self.index()
                elif char == 'M': self.index(True)
                elif char == 'E': self.screen.x=0;self.index()
                elif char == 'H': self.tabstops.add(self.screen.x)
                elif char == '=': self.app_keypad=True
                elif char == '>': self.app_keypad=False
                elif char == 'c':
                    respond=self.respond;self.__init__(self.cols,self.rows,respond)
                continue
            if char == '\r': self.screen.x=0;self.screen.wrap=False
            elif char in ('\n','\v','\f'): self.index()
            elif char == '\b': self.screen.x=max(0,self.screen.x-1);self.screen.wrap=False
            elif char == '\t':
                self.screen.x=next((x for x in sorted(self.tabstops) if x>self.screen.x),self.cols-1)
                self.screen.wrap=False
            elif char == '\x0e': self.charset=1
            elif char == '\x0f': self.charset=0
            elif code>=32 and code!=127: self.put(char)
        self.revision += 1

    def visible(self, scroll=0):
        if self.alt or not scroll:
            return self.screen.grid
        lines = list(self.history)+self.normal.grid
        end = len(lines)-min(scroll,len(self.history))
        result = lines[max(0,end-self.rows):end]
        view=[line[:self.cols]+[Cell()] * max(0,self.cols-len(line)) for line in result]
        for line in view:repair_line(line)
        return view


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


def style_escape(style):
    codes=['0']
    for bit,code in ((BOLD,1),(DIM,2),(ITALIC,3),(UNDER,4),(BLINK,5),(INVERSE,7),(HIDDEN,8),(STRIKE,9)):
        if style.flags&bit:codes.append(str(code))
    codes += ['38','2',*(str(x) for x in (style.fg or DEFAULT_FG)), '48','2',*(str(x) for x in (style.bg or DEFAULT_BG))]
    return '\x1b['+';'.join(codes)+'m'


def render_line(cells):
    result=[];style=None;link=''
    for cell in cells:
        if cell.width==0:continue
        if cell.style!=style:
            style=cell.style
            if style.link!=link:
                if link:result.append('\x1b]8;;\x1b\\')
                link=style.link
                if link:result.append('\x1b]8;;'+link+'\x1b\\')
            result.append(style_escape(style))
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
    parser.add_argument('--cwd',default=os.getcwd(),help='directory for the first shell and new tabs')
    parser.add_argument('--shell',default=os.environ.get('SHELL') or '/bin/sh',help='interactive shell for new tabs')
    parser.add_argument('command',nargs=argparse.REMAINDER,help='optional command, following --')
    args=parser.parse_args(argv)
    if not os.isatty(0) or not os.isatty(1):
        parser.exit(2,'hardcore here needs an interactive terminal on stdin and stdout. Run it directly in Terminal, a Linux terminal, or WSL.\n')
    cwd=str(Path(args.cwd).expanduser().resolve())
    if not Path(cwd).is_dir():parser.exit(2,f'hardcore here: not a directory: {cwd}\n')
    command=args.command[1:] if args.command[:1]==['--'] else args.command
    try:return Application(args.shell,cwd,command or None).run()
    except (OSError,ValueError) as error:
        print(f'hardcore here: {error}',file=sys.stderr)
        return 1


if __name__=='__main__':
    raise SystemExit(main())
