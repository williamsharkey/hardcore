#!/usr/bin/env python3
"""Resizing a pane must not make a full-screen program's text bob. The
stand-in redraws on SIGWINCH as Claude Code 2.1 does on the main screen: it
erases from the top, redraws bottom-up with a blank row under its status
lines, hides the cursor while it draws and writes the frame in two pieces.
Every frame the window paints during divider drags and window resizes in the
grid must
show one of its frames whole, at the size it was drawn for: the prompt five
rows above the bottom, rules exactly as wide as the screen. Rewrapping the
old screen before the program answers painted it a line or two off."""
import sys, tempfile, time
from pathlib import Path
from test_chrome_mac import NativeApp, ROOT, wait_for

TUI = r'''
import fcntl,os,signal,struct,sys,termios,time,tty
def size():return struct.unpack("HHHH",fcntl.ioctl(0,termios.TIOCGWINSZ,bytes(8)))[:2]
history=[f"     {k}" for k in range(1,151)]
def frame():
    rows,cols=size()
    ui=["─"*cols,"❯ ","─"*cols,"  status one","  status two"]
    lines=(history+[""])[-(rows-1-len(ui)):]+ui
    data=("\x1b[?25l\x1b[H"+"\x1b[2K\x1b[1B"*rows+"\x1b[H"+"".join(l+"\r\r\n" for l in lines)+
          f"\x1b[3G\x1b[{len(ui)}A\x1b[?25h").encode()
    os.write(1,data[:len(data)//2]);time.sleep(.001);os.write(1,data[len(data)//2:])
winch=[0]
signal.signal(signal.SIGWINCH,lambda *a:winch.__setitem__(0,1))
tty.setraw(0)
os.write(1,("\r\n".join(history)+"\r\n").encode());frame()
while True:
    if winch[0]:winch[0]=0;frame()
    time.sleep(.001)
'''


def frames(path):
    lines = path.read_text(encoding='utf-8').split('\n')
    i = 0
    while i < len(lines):
        if lines[i].startswith('frame '):
            head = lines[i].split()
            cols, rows = map(int, head[4].split('x'))
            width, height = map(int, head[6].split('x'))
            yield lines[i], min(cols, width), rows, lines[i + 1:i + 1 + height]
            i += height
        i += 1


def check(path, what):
    seen = 0
    for head, cols, rows, shown in frames(path):   # (a screen waiting on its answer is cropped to its pane)
        seen += 1
        prompt = [y for y, line in enumerate(shown) if line.startswith('❯')]
        assert prompt == [rows - 5], (what, 'the prompt off its row: a rewrapped or half-drawn frame', head, prompt, shown)
        for y in (rows - 6, rows - 4):
            assert shown[y].rstrip() == '─' * cols, (what, 'a rule not as wide as its screen', head, shown[y])
    assert seen >= 5, (what, seen)
    return seen


def drag(app, x, y, dx, dy, steps):
    app.send(f'cmd pointer down {x},{y}')
    for k in steps:
        app.send(f'cmd pointer drag {x + k * dx},{y + k * dy}'); time.sleep(.02)
    app.send(f'cmd pointer up {x},{y}'); time.sleep(.3)


def run():
    artifacts = ROOT / 'build/grid-resize'
    with tempfile.TemporaryDirectory(prefix='hardcore-grid-resize-') as root:
        with NativeApp(Path(root), 2, artifacts) as app:
            (app.root / 'tui.py').write_text(TUI)
            app.send('cmd resize 1400,800'); app.send('cmd focus 1')
            app.send('cmd newtab'); app.send(f'type {sys.executable} {app.root / "tui.py"}\\n')
            for _ in range(2):
                app.send('cmd newtab'); app.send('type sleep 600\\n')
            app.send('cmd tab 1')
            wait_for(lambda: any('status two' in line for line in app.chrome()['source']), description='stand-in TUI drawn')
            app.send('cmd grid'); time.sleep(.5)
            c = app.chrome(); cw, ch = c['cell']['width'], c['cell']['height']
            ident = app.state()['tabs'][1]['id']
            pane = next(p for p in c['grid']['panes'] if p['id'] == ident)
            below = next(p for p in c['grid']['panes'] if p['y'] > pane['y'])
            app.send('cmd tab 1'); time.sleep(.3)
            steps = list(range(1, 7)) + list(range(5, -7, -1)) + list(range(-5, 1))
            counts = []
            for name, x, y, dx, dy in (('rows', (pane['x'] + 3) * cw, below['y'] * ch, 0, ch),
                                       ('columns', pane['x'] * cw, (pane['y'] + 3) * ch, cw, 0)):
                log = app.root / f'frames-{name}.txt'
                app.send(f'cmd framelog {log}')
                drag(app, x, y, dx, dy, steps)
                app.send('cmd framelog ')
                counts.append(check(log, f'dragging the divider across {name}'))
            # The window itself, around the grid
            log = app.root / 'frames-window.txt'
            app.send(f'cmd framelog {log}')
            for k in steps:
                app.send(f'cmd resize {1400 + k * 2 * cw},{800 + k * ch}'); time.sleep(.02)
            time.sleep(.3)
            app.send('cmd framelog ')
            counts.append(check(log, 'resizing the window'))
            # Leaving the grid gives every tab the window's size at once
            app.send('cmd grid')
            wait_for(lambda: len({(t['cols'], t['rows']) for t in app.state()['tabs']}) == 1, timeout=1,
                     description='every tab at the window size on leaving the grid')
    print(f'Grid resize: {"/".join(map(str, counts))} frames of divider drags and window resizes, '
          'none rewrapped or half-drawn')


if __name__ == '__main__':
    run()
