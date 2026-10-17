"""Bounded VT state for Hardcore's terminal-hosted frontend and session broker."""
from __future__ import annotations
import codecs
from collections import deque
from dataclasses import dataclass, replace
import re
import socket
import time
import unicodedata
from urllib.parse import urlsplit, unquote

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


def indexed_color(index, palette=PALETTE):
    index = max(0, min(255, index))
    if index < 16:
        return palette[index]
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
    fg_index: int | None = None
    bg_index: int | None = None


def resolve_style(style, palette=PALETTE, foreground=DEFAULT_FG, background=DEFAULT_BG):
    """Resolve a stored ANSI color identity without altering truecolor values."""
    return replace(style,
                   fg=indexed_color(style.fg_index, palette) if style.fg_index is not None else style.fg or foreground,
                   bg=indexed_color(style.bg_index, palette) if style.bg_index is not None else style.bg or background)


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
        # Forward continuation and occupied extent, including explicitly written
        # spaces. A wide glyph may wrap before the final column: that padding is
        # deliberately not part of the logical line.
        self.wrapped = [False] * rows
        self.used = [0] * rows

    def resize(self, cols, rows):
        self.grid = [line[:cols] + [Cell()] * max(0, cols-len(line)) for line in self.grid[:rows]]
        self.grid += [[Cell()] * cols for _ in range(rows-len(self.grid))]
        self.wrapped = (self.wrapped[:rows] + [False] * rows)[:rows]
        self.used = ([min(cols, n) for n in self.used[:rows]] + [0] * rows)[:rows]
        self.cols, self.rows = cols, rows
        for line in self.grid:
            repair_line(line)
        self.x, self.y = min(self.x, cols-1), min(self.y, rows-1)
        self.top, self.bottom, self.wrap = 0, rows-1, False
        if self.saved:
            x,y,style,wrap,origin = self.saved
            self.saved = min(x,cols-1),min(y,rows-1),style,False,origin


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
        self.history_wrapped = deque(maxlen=self.history.maxlen)
        self.history_used = deque(maxlen=self.history.maxlen)
        self._tail = []
        self.history_pushed = 0
        self.respond = respond
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')
        self.state, self.control = 'text', ''
        self.string_esc = False
        self.string_kind = ''
        self.title = ''
        self.cwd = ''
        self.bell_count = 0
        self.activity = 0
        self.command_count = 0
        self.prompt_state = ''
        self.palette = PALETTE
        self.foreground, self.background = DEFAULT_FG, DEFAULT_BG
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
        if (cols, rows) == (self.cols, self.rows):
            return
        self._reflow_normal(cols, rows)
        self.alternate.resize(cols, rows)
        self.tabstops.update(range(((self.cols+7)//8)*8, cols, 8))
        self.tabstops = {x for x in self.tabstops if 0 <= x < cols}
        self.cols, self.rows = cols, rows
        self.revision += 1

    @staticmethod
    def _extent(line, used=0):
        end = len(line)
        while end and line[end-1] == Cell():
            end -= 1
        return max(used, end)

    def _reflow_normal(self, cols, rows):
        """Repack soft lines, retaining hard breaks, spaces, styles and cursor.

        Reflow is confined to the normal buffer; full-screen applications keep
        their coordinate grid. Hidden rows below a cursor near the top survive
        a temporarily tiny viewport and return on growth (ED2 clears them).
        """
        s = self.normal
        records = list(zip(self.history, self.history_wrapped, self.history_used))
        cursor_row = len(records) + s.y
        saved_row = len(records) + s.saved[1] if s.saved else None
        records.extend(zip(s.grid, s.wrapped, s.used))
        records.extend(self._tail)
        # Untouched bottom padding is viewport space, not terminal history.
        last_needed = max(cursor_row, saved_row or 0)
        while len(records)-1 > last_needed and not records[-1][1] and not self._extent(records[-1][0], records[-1][2]):
            records.pop()
        groups, cells, marks = [], [], {}
        for i, (line, wrapped, used) in enumerate(records):
            extent = self._extent(line, used)
            positions = []
            if i == cursor_row:
                positions.append(('cursor', s.x + int(s.wrap), s.wrap))
            if i == saved_row:
                positions.append(('saved', s.saved[0] + int(s.saved[3]), s.saved[3]))
            extent = min(len(line), max([extent] + [p[1] for p in positions]))
            for name, offset, pending in positions:
                marks[name] = (len(cells)+offset, pending)
            cells.extend(line[:extent])
            if not wrapped or i == len(records)-1:
                groups.append((cells, marks)); cells, marks = [], {}
        packed, mapped = [], {}
        for cells, marks in groups:
            spans, at = [], 0
            if not cells:
                spans.append((0, 0, []))
            while at < len(cells):
                start, line = at, []
                while at < len(cells):
                    cell = cells[at]
                    if cell.width == 0:  # tolerate malformed imported rows
                        at += 1
                        continue
                    width = cell.width
                    if len(line)+width > cols:
                        break
                    line.append(cell)
                    if width == 2:
                        line.append(Cell('', 0, cell.style))
                    at += width
                spans.append((start, at, line))
            base = len(packed)
            for j, (start, end, line) in enumerate(spans):
                packed.append((line+[Cell()]*(cols-len(line)), j+1 < len(spans), len(line)))
            for name, (offset, pending) in marks.items():
                for j, (start, end, line) in enumerate(spans):
                    if offset < end or j == len(spans)-1 or (offset == end and pending):
                        x = max(0, offset-start)
                        wrap = x == cols
                        mapped[name] = (min(cols-1, x), base+j, wrap)
                        break
        if not packed:
            packed = [([Cell()]*cols, False, 0)]
        x, y, wrap = mapped.get('cursor', (0, 0, False))
        # Anchor at the bottom when possible, but never move the input cursor
        # out of view just to display rows below it.
        start = min(max(0, len(packed)-rows), y)
        history_limit = max(1, min(4000, 200000//cols))
        before = packed[max(0, start-history_limit):start]
        self.history = deque((r[0] for r in before), maxlen=history_limit)
        self.history_wrapped = deque((r[1] for r in before), maxlen=history_limit)
        self.history_used = deque((r[2] for r in before), maxlen=history_limit)
        visible = packed[start:start+rows]
        self._tail = packed[start+rows:start+rows+max(1,200000//cols)]
        visible += [([Cell()]*cols, False, 0) for _ in range(rows-len(visible))]
        s.grid, s.wrapped, s.used = [r[0] for r in visible], [r[1] for r in visible], [r[2] for r in visible]
        s.cols, s.rows, s.x, s.y, s.wrap = cols, rows, x, y-start, wrap
        s.top, s.bottom = 0, rows-1
        if s.saved and 'saved' in mapped:
            sx, sy, sw = mapped['saved']
            s.saved = (sx, max(0, min(rows-1, sy-start)), s.saved[2], sw, s.saved[4])

    def set_theme(self, palette, foreground, background):
        if len(palette) != 16:
            raise ValueError('a terminal palette must contain 16 colors')
        def color(value):
            if len(value) != 3 or any(not isinstance(c, int) or not 0 <= c <= 255 for c in value):
                raise ValueError('invalid RGB color')
            return tuple(value)
        palette = tuple(color(c) for c in palette)
        foreground, background = color(foreground), color(background)
        if (self.palette,self.foreground,self.background) == (palette,foreground,background):
            return
        self.palette = palette
        self.foreground, self.background = foreground, background
        def restyle(style):
            return replace(style,
                           fg=indexed_color(style.fg_index, self.palette) if style.fg_index is not None else style.fg,
                           bg=indexed_color(style.bg_index, self.palette) if style.bg_index is not None else style.bg)
        self.style = restyle(self.style)
        for buf in (self.normal, self.alternate):
            for line in buf.grid:
                line[:] = [replace(c, style=restyle(c.style)) for c in line]
            if buf.saved:
                x,y,style,wrap,origin = buf.saved
                buf.saved = x,y,restyle(style),wrap,origin
        for line in list(self.history)+[r[0] for r in self._tail]:
            line[:] = [replace(c, style=restyle(c.style)) for c in line]
        self.revision += 1

    def resolved_style(self, style):
        return resolve_style(style, self.palette, self.foreground, self.background)

    def blank_line(self):
        return [Cell(style=self.style)] * self.cols

    def scroll_up(self, count=1):
        s = self.screen
        for _ in range(min(max(1, count), s.bottom-s.top+1)):
            line = s.grid.pop(s.top)
            wrapped, used = s.wrapped.pop(s.top), s.used.pop(s.top)
            if not self.alt and s.top == 0 and s.bottom == self.rows-1:
                self.history.append(line)
                self.history_wrapped.append(wrapped)
                self.history_used.append(used)
                self.history_pushed += 1
            s.grid.insert(s.bottom, self.blank_line())
            s.wrapped.insert(s.bottom, False)
            s.used.insert(s.bottom, 0)

    def scroll_down(self, count=1):
        s = self.screen
        for _ in range(min(max(1, count), s.bottom-s.top+1)):
            s.grid.pop(s.bottom)
            s.grid.insert(s.top, self.blank_line())
            s.wrapped.pop(s.bottom); s.wrapped.insert(s.top, False)
            s.used.pop(s.bottom); s.used.insert(s.top, 0)

    def index(self, reverse=False, soft=False):
        s = self.screen
        if not reverse:
            s.wrapped[s.y] = soft
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
            self.index(soft=True)
        s.wrap = False
        if width == 2 and s.x == self.cols-1:
            if self.autowrap:
                s.x = 0
                self.index(soft=True)
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
        s.used[s.y] = max(s.used[s.y], s.x+width)
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
        if right == self.cols:
            s.used[s.y] = min(s.used[s.y], left)
            s.wrapped[s.y] = False
        s.wrap = False

    def erase_display(self, mode):
        s = self.screen
        if mode == 3:
            self.history.clear()
            self.history_wrapped.clear(); self.history_used.clear()
            return
        if mode == 2:
            s.grid = [self.blank_line() for _ in range(self.rows)]
            s.wrapped, s.used = [False]*self.rows, [0]*self.rows
            if not self.alt: self._tail = []
        elif mode in (0, 1):
            self.erase_line(mode)
            for y in (range(s.y+1, self.rows) if mode == 0 else range(s.y)):
                s.grid[y] = self.blank_line()
                s.wrapped[y], s.used[y] = False, 0
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
                key = 'fg' if value == 39 else 'bg'
                self.style = replace(self.style, **{key: None, key+'_index': None})
            elif 30 <= value <= 37 or 90 <= value <= 97:
                index = value-30 if value < 90 else value-90+8
                self.style = replace(self.style, fg=indexed_color(index, self.palette), fg_index=index)
            elif 40 <= value <= 47 or 100 <= value <= 107:
                index = value-40 if value < 100 else value-100+8
                self.style = replace(self.style, bg=indexed_color(index, self.palette), bg_index=index)
            elif value in (38,48) and i+1 < len(values):
                mode = values[i+1]
                color = None
                index = None
                if mode == 5 and i+2 < len(values):
                    index = max(0, min(255, values[i+2]))
                    color, i = indexed_color(index, self.palette), i+2
                elif mode == 2 and i+4 < len(values):
                    color, i = tuple(max(0,min(255,x)) for x in values[i+2:i+5]), i+4
                if color is not None:
                    key = 'fg' if value == 38 else 'bg'
                    self.style = replace(self.style, **{key: color, key+'_index': index})
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
                remove, insert = (s.bottom, s.y) if final == 'L' else (s.y, s.bottom)
                s.grid.pop(remove);s.grid.insert(insert,self.blank_line())
                s.wrapped.pop(remove);s.wrapped.insert(insert,False)
                s.used.pop(remove);s.used.insert(insert,0)
        elif final in '@PX':
            n = min(n,self.cols-s.x)
            line = s.grid[s.y]
            if final == '@': line[s.x:s.x] = [Cell(style=self.style)]*n;del line[self.cols:]
            elif final == 'P': del line[s.x:s.x+n];line.extend([Cell(style=self.style)]*n)
            else:
                for x in range(s.x,s.x+n): self.clear_cell(line,x)
            repair_line(line)
            s.used[s.y] = self._extent(line)
            s.wrapped[s.y] = False
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
            # Combining marks occupy no columns; cap bytes/characters as well
            # so a valid OSC cannot create unbounded metadata in snapshots.
            self.title = clean_text(value,80)[:160]
        elif code == '7':
            try:
                parsed = urlsplit(value)
                if parsed.scheme == 'file' and parsed.hostname in (None, '', 'localhost', socket.gethostname()):
                    path = unquote(parsed.path)
                    if path.startswith('/') and len(path) <= 4096 and not any(unicodedata.category(c) in ('Cc','Cs') for c in path):
                        self.cwd = path
            except ValueError:
                pass
        elif code == '9' and value.startswith('9;'):
            path = value[2:].strip('"')
            if len(path) <= 4096 and (path.startswith('/') or re.match(r'^[A-Za-z]:[\\/]', path)) and not any(unicodedata.category(c) in ('Cc','Cs') for c in path):
                self.cwd = path
        elif code in ('133', '633'):
            marker = value.split(';', 1)[0]
            if marker in ('A','B','C','D'):
                self.prompt_state = {'A':'prompt','B':'input','C':'running','D':'finished'}[marker]
                if marker == 'C':self.command_count += 1
        elif code == '8':
            _, _, url = value.partition(';')
            try:
                allowed = urlsplit(url).scheme in ('http','https','mailto','file')
            except ValueError:
                allowed = False
            if len(url)>2048 or any(unicodedata.category(c) in ('Cc','Cs') for c in url): allowed=False
            self.style = replace(self.style,link=url if allowed else '')
        elif code in ('10','11') and value == '?':
            rgb = self.foreground if code == '10' else self.background
            color = '/'.join(f'{x*257:04x}' for x in rgb)
            self.respond(f'\x1b]{code};rgb:{color}\x1b\\'.encode())
        elif code == '4':
            parts = value.split(';')
            if len(parts)==2 and len(parts[0])<=3 and parts[0].isdigit() and parts[1]=='?':
                rgb=indexed_color(min(255,int(parts[0])), self.palette)
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
                    respond=self.respond; theme=(self.palette,self.foreground,self.background)
                    revision,activity,bells,commands = self.revision,self.activity,self.bell_count,self.command_count
                    self.__init__(self.cols,self.rows,respond);self.set_theme(*theme)
                    # Broker generations describe changes, not VT state: RIS
                    # must not make a changed display appear unchanged.
                    self.revision,self.activity,self.bell_count = revision,activity,bells
                    self.command_count=commands
                continue
            if char == '\r': self.screen.x=0;self.screen.wrap=False
            elif char in ('\n','\v','\f'): self.index()
            elif char == '\b': self.screen.x=max(0,self.screen.x-1);self.screen.wrap=False
            elif char == '\t':
                self.screen.x=next((x for x in sorted(self.tabstops) if x>self.screen.x),self.cols-1)
                self.screen.wrap=False
            elif char == '\x0e': self.charset=1
            elif char == '\x0f': self.charset=0
            elif char == '\x07': self.bell_count += 1
            elif code>=32 and code!=127: self.put(char)
        if data:
            self.activity += 1
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



def _style_data(style):
    return [style.fg, style.bg, style.flags, style.link, style.fg_index, style.bg_index]


def _style_from_data(data):
    if not isinstance(data, (list, tuple)) or len(data) != 6:
        raise ValueError('invalid terminal style')
    colors = []
    for color in data[:2]:
        if color is not None and (not isinstance(color, (list, tuple)) or len(color) != 3 or
                                  any(type(c) is not int or not 0 <= c <= 255 for c in color)):
            raise ValueError('invalid terminal color')
        colors.append(tuple(color) if color is not None else None)
    flags, link, fi, bi = data[2:]
    if type(flags) is not int or not 0 <= flags < 256 or not isinstance(link, str) or len(link) > 2048:
        raise ValueError('invalid terminal style attributes')
    if any(unicodedata.category(c) in ('Cc','Cs') for c in link):
        raise ValueError('invalid terminal hyperlink')
    if link and urlsplit(link).scheme not in ('http','https','mailto','file'):
        raise ValueError('invalid terminal hyperlink scheme')
    if any(v is not None and (type(v) is not int or not 0 <= v <= 255) for v in (fi,bi)):
        raise ValueError('invalid terminal palette index')
    return Style(*colors, flags, link, fi, bi)


def encode_rows(rows):
    """Compact JSON-safe rows, deduplicated styles and runs of identical cells."""
    styles, lookup, output = [], {}, []
    for row in rows:
        runs = []
        for cell in row:
            if cell.style not in lookup:
                lookup[cell.style] = len(styles); styles.append(_style_data(cell.style))
            value = [cell.text, cell.width, lookup[cell.style]]
            if runs and runs[-1][1:] == value:
                runs[-1][0] += 1
            else:
                runs.append([1]+value)
        output.append(runs)
    return {'styles': styles, 'rows': output}


def decode_rows(data, *, max_rows=100000, max_cells=400000):
    """Validate persisted/broker rows without allocating attacker-sized runs."""
    if not isinstance(data, dict) or not isinstance(data.get('styles'), list) or not isinstance(data.get('rows'), list):
        raise ValueError('invalid terminal rows')
    if len(data['styles']) > min(max_cells, 100000) or len(data['rows']) > max_rows:
        raise ValueError('terminal snapshot exceeds bounds')
    styles = [_style_from_data(s) for s in data['styles']]
    output, total = [], 0
    for runs in data['rows']:
        if not isinstance(runs, list) or len(runs) > MAX_COLS:
            raise ValueError('invalid terminal row')
        row = []
        for run in runs:
            if not isinstance(run, list) or len(run) != 4:
                raise ValueError('invalid terminal cell run')
            n, text, width, si = run
            if type(n) is not int or n < 1 or len(row)+n > MAX_COLS or total+n > max_cells:
                raise ValueError('terminal cell run exceeds bounds')
            if not isinstance(text, str) or len(text) > 64 or type(width) is not int or width not in (0,1,2) or type(si) is not int or not 0 <= si < len(styles):
                raise ValueError('invalid terminal cell')
            if any(unicodedata.category(c) in ('Cc','Cs','Cf') and c != '\u200d' for c in text):
                raise ValueError('unsafe terminal cell')
            row.extend([Cell(text, width, styles[si])]*n); total += n
        repair_line(row)
        output.append(row)
    return output


def _terminal_snapshot(self, include_history=True, include_screen=True):
    def buffer(buf):
        saved = None
        if buf.saved:
            x,y,style,wrap,origin = buf.saved
            saved = [x,y,_style_data(style),wrap,origin]
        result = {'wrapped': list(buf.wrapped), 'used': list(buf.used),
                  'x': buf.x, 'y': buf.y, 'top': buf.top, 'bottom': buf.bottom,
                  'wrap': buf.wrap, 'saved': saved}
        if include_screen: result['cells'] = encode_rows(buf.grid)
        return result
    fields = ('alt','title','cwd','bell_count','activity','command_count','prompt_state','cursor_visible','cursor_shape',
              'autowrap','origin','insert','app_cursor','app_keypad','bracketed_paste','focus','mouse','mouse_sgr',
              'g0','g1','charset','revision','history_pushed','state','control','string_esc','string_kind')
    state = {name:getattr(self,name) for name in fields}
    state.update(version=1, cols=self.cols, rows=self.rows,
                 palette=self.palette, foreground=self.foreground, background=self.background,
                 style=_style_data(self.style), normal=buffer(self.normal), alternate=buffer(self.alternate),
                 tabstops=sorted(self.tabstops), decoder=self.decoder.getstate()[0].hex())
    if include_history:
        state.update(history=encode_rows(self.history), history_wrapped=list(self.history_wrapped),
                     history_used=list(self.history_used), tail=encode_rows(r[0] for r in self._tail),
                     tail_wrapped=[r[1] for r in self._tail], tail_used=[r[2] for r in self._tail])
    return state


def _terminal_from_snapshot(cls, data, respond=lambda data: None):
    if not isinstance(data, dict) or data.get('version') != 1:
        raise ValueError('unsupported terminal snapshot')
    cols, rows = data.get('cols'), data.get('rows')
    if type(cols) is not int or type(rows) is not int or not 2 <= cols <= MAX_COLS or not 1 <= rows <= MAX_ROWS:
        raise ValueError('invalid terminal dimensions')
    term = cls(cols, rows, respond)
    term.set_theme(data.get('palette',PALETTE), data.get('foreground',DEFAULT_FG), data.get('background',DEFAULT_BG))
    def records(cells, wrapped, used, limit=100000):
        lines = decode_rows(cells, max_rows=limit)
        if any(len(row) != cols for row in lines) or not isinstance(wrapped,list) or not isinstance(used,list) or len(wrapped) != len(lines) or len(used) != len(lines):
            raise ValueError('inconsistent terminal row metadata')
        if any(type(n) is not int or not 0 <= n <= cols for n in used) or any(type(w) is not bool for w in wrapped):
            raise ValueError('invalid terminal row metadata')
        return lines, wrapped, used
    for name in ('normal','alternate'):
        info, buf = data[name], getattr(term,name)
        if 'cells' in info:
            buf.grid,buf.wrapped,buf.used = records(info['cells'], info['wrapped'], info['used'], rows)
        else:
            wrapped,used = info['wrapped'],info['used']
            if not isinstance(wrapped,list) or not isinstance(used,list) or len(wrapped)!=rows or len(used)!=rows or \
               any(type(w) is not bool for w in wrapped) or any(type(n) is not int or not 0<=n<=cols for n in used):
                raise ValueError('invalid terminal row metadata')
            buf.wrapped,buf.used = list(wrapped),list(used)
        if len(buf.grid) != rows:
            raise ValueError('invalid terminal screen height')
        for field, bound in (('x',cols),('y',rows),('top',rows),('bottom',rows)):
            value = info[field]
            if type(value) is not int or not 0 <= value < bound:
                raise ValueError('invalid terminal cursor or scroll region')
            setattr(buf,field,value)
        if buf.top > buf.bottom:
            raise ValueError('invalid terminal scroll region')
        buf.wrap = bool(info['wrap'])
        if info.get('saved') is not None:
            x,y,style,wrap,origin = info['saved']
            if type(x) is not int or type(y) is not int or not 0 <= x < cols or not 0 <= y < rows:
                raise ValueError('invalid saved terminal cursor')
            buf.saved = x,y,_style_from_data(style),bool(wrap),bool(origin)
    if 'history' in data:
        lines,wrapped,used = records(data['history'],data['history_wrapped'],data['history_used'])
        limit = max(1,min(4000,200000//cols))
        term.history = deque(lines,maxlen=limit); term.history_wrapped = deque(wrapped,maxlen=limit); term.history_used = deque(used,maxlen=limit)
        lines,wrapped,used = records(data['tail'],data['tail_wrapped'],data['tail_used'])
        term._tail = list(zip(lines,wrapped,used))
    bools = ('alt','cursor_visible','autowrap','origin','insert','app_cursor','app_keypad','bracketed_paste','focus','mouse_sgr','string_esc')
    for name in bools:
        if type(data.get(name)) is not bool: raise ValueError('invalid terminal mode')
        setattr(term,name,data[name])
    for name in ('bell_count','activity','command_count','revision','history_pushed'):
        value = data.get(name,0)
        if type(value) is not int or not 0 <= value <= 2**63-1: raise ValueError('invalid terminal counter')
        setattr(term,name,value)
    for name, allowed in (('cursor_shape',range(7)),('mouse',(0,1000,1002,1003)),('charset',(0,1)),
                          ('state',('text','osc','discard','escape','escape_intermediate','csi','charset')),
                          ('string_kind',('','osc','dcs')),('prompt_state',('','prompt','input','running','finished'))):
        if data.get(name) not in allowed: raise ValueError('invalid terminal state')
        setattr(term,name,data[name])
    for name,limit in (('title',160),('cwd',4096),('control',16384),('g0',1),('g1',1)):
        value = data.get(name,'')
        if not isinstance(value,str) or len(value)>limit: raise ValueError('oversized terminal metadata')
        if name in ('title','cwd') and any(unicodedata.category(c) in ('Cc','Cs') for c in value):
            raise ValueError('unsafe terminal metadata')
        setattr(term,name,value)
    stops = data.get('tabstops',[])
    if not isinstance(stops,list) or len(stops)>cols or any(type(x) is not int or not 0<=x<cols for x in stops):
        raise ValueError('invalid terminal tab stops')
    term.tabstops = set(stops)
    term.style = _style_from_data(data['style'])
    pending = data.get('decoder','')
    if not isinstance(pending,str) or len(pending)>8: raise ValueError('invalid pending UTF-8')
    term.decoder.setstate((bytes.fromhex(pending),0))
    return term


def _terminal_apply_snapshot(self, data):
    new = type(self).from_snapshot(data,self.respond)
    for name in ('normal','alternate'):
        if 'cells' not in data[name]:
            if (new.cols,new.rows) != (self.cols,self.rows):
                raise ValueError('partial screen requires unchanged dimensions')
            getattr(new,name).grid = getattr(self,name).grid
    if 'history' not in data and new.cols == self.cols:
        new.history, new.history_wrapped, new.history_used, new._tail = self.history,self.history_wrapped,self.history_used,self._tail
    self.__dict__.update(new.__dict__)


Terminal.snapshot = _terminal_snapshot
Terminal.from_snapshot = classmethod(_terminal_from_snapshot)
Terminal.apply_snapshot = _terminal_apply_snapshot
