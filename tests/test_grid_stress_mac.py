#!/usr/bin/env python3
"""Puts the tiled grid through the cases other terminals' pane trackers report:
resize storms, tiny windows, extreme divider drags, closing and hiding the
active pane, focus and mouse reports to panes away from the origin, and output
floods while dividers move. Every pane's child must see its pane's size and
never a zero one."""
import json, shlex, sys, tempfile, time
from pathlib import Path
from test_chrome_mac import NativeApp, ROOT, wait_for

WORKER = r'''
import fcntl,json,os,select,signal,struct,sys,termios,tty
from pathlib import Path
root=Path(sys.argv[1]);init=bytes.fromhex(sys.argv[2])
winsizes=[];received=bytearray()
def size():return list(struct.unpack("HHHH",fcntl.ioctl(0,termios.TIOCGWINSZ,bytes(8)))[:2])
def winch(signum,frame):winsizes.append(size())
signal.signal(signal.SIGWINCH,winch)
tty.setraw(0);os.write(1,init)
def record():
    temp=root.with_suffix(".tmp")
    temp.write_text(json.dumps(dict(pid=os.getpid(),winch=winsizes,size=size(),input=received.hex())))
    temp.replace(root.with_suffix(".json"))
record()
while True:
    say=root.with_suffix(".say")
    if say.exists():
        os.write(1,say.read_bytes());say.unlink()
    flood=root.with_suffix(".flood")
    if flood.exists():
        flood.unlink();line=b"\x1b[32mflood\x1b[0m \xe6\x97\xa5\xe6\x9c\xac " + b"x"*90 + b"\r\n"
        for _ in range(int(sys.argv[3]) if len(sys.argv)>3 else 20000):os.write(1,line)
    if select.select([0],[],[],.02)[0]:
        data=os.read(0,4096)
        if not data:break
        received.extend(data)
    record()
'''


def data(path):
    try:
        return json.loads(path.with_suffix('.json').read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def start_worker(app, path, init=b''):
    app.send('cmd newtab')
    command = ' '.join(shlex.quote(v) for v in (sys.executable, '-u', str(app.root / 'worker.py'), str(path), init.hex()))
    app.send('type ' + command + r'\n')
    ready = wait_for(lambda: data(path), description=f'grid worker {path.name}')
    app.pids.add(ready['pid'])
    return app.state()['tabs'][app.state()['active']]['id']


def grid(app):
    return app.chrome()['grid']


def check_layout(app):
    """Panes tile without overlap; each visible pane's PTY is its size."""
    c = app.chrome(); g = c['grid']; panes = g['panes']
    for i, a in enumerate(panes):
        assert a['width'] >= 0 and a['height'] >= 0, a
        assert a['x'] >= g['left'] and a['y'] >= g['top'], (a, g)
        assert a['x'] + a['width'] <= g['left'] + g['width'] and a['y'] + a['height'] <= g['top'] + g['height'], (a, g)
        assert g['left'] + g['width'] <= g['window'][0] and g['top'] + g['height'] <= g['window'][1], g
        for b in panes[:i]:
            assert (not a['width'] or not a['height'] or not b['width'] or not b['height'] or
                    a['x'] >= b['x'] + b['width'] or b['x'] >= a['x'] + a['width'] or
                    a['y'] >= b['y'] + b['height'] or b['y'] >= a['y'] + a['height']), (a, b)
    if panes:
        assert sum(p['width'] * p['height'] for p in panes) == g['width'] * g['height'], g
    def sized():
        tabs = {t['id']: t for t in app.state()['tabs']}
        return all((tabs[p['id']]['cols'], tabs[p['id']]['rows']) == (p['width'], p['height'] - 1)
                   for p in grid(app)['panes'] if p['height'] > 1)
    wait_for(sized, description='every pane PTY at its pane size')
    return c


def worker_sized(app, workers):
    def ok():
        panes = {p['id']: p for p in grid(app)['panes']}
        for identity, path in workers:
            p = panes.get(identity)
            if p and p['height'] > 1 and data(path)['size'] != [p['height'] - 1, p['width']]:
                return False
        return True
    wait_for(ok, description='every child sees its pane size')
    for _, path in workers:
        sizes = data(path)['winch'] + [data(path)['size']]
        assert all(r > 0 and c > 0 for r, c in sizes), (path.name, sizes)


def press(app, c, col, row, action='down'):
    cw, ch = c['cell']['width'], c['cell']['height']
    app.send(f'cmd pointer {action} {int((col + .5) * cw)},{int((row + .5) * ch)}')


def click(app, c, col, row):
    press(app, c, col, row); press(app, c, col, row, 'up')


def pane(app, identity):
    return next(p for p in grid(app)['panes'] if p['id'] == identity)


def run():
    artifacts = ROOT / 'build/grid-stress'
    with tempfile.TemporaryDirectory(prefix='hardcore-grid-stress-') as root:
        with NativeApp(Path(root), 2, artifacts) as app:
            (app.root / 'worker.py').write_text(WORKER)
            app.send('cmd resize 1400,800')
            app.send('cmd focus 1')
            workers = []
            for i in range(11):
                path = app.root / f'w{i}'
                init = (b'\x1b[?1004h' if i < 2 else b'\x1b[?1003h\x1b[?1006h' if i == 2 else
                        b'\x1b[?1000h\x1b[?1006h' if i == 4 else b'')
                workers.append((start_worker(app, path, init), path))
            app.send('cmd grid')
            c = check_layout(app); g = c['grid']
            assert g['enabled'] and len(g['panes']) == 12, g
            worker_sized(app, workers)

            # Resize storm: many sizes in quick succession, then settle.
            for k in range(30):
                w = 500 + (k * 377) % 1300; h = 260 + (k * 211) % 700
                app.send(f'cmd resize {w},{h}')
            app.send('cmd resize 1300,760')
            check_layout(app); worker_sized(app, workers)

            # A window too small for twelve readable panes: some panes have no
            # room, and no child may ever be told it has zero rows or columns.
            for size in ('200,120', '120,90', '320,140', '420,150'):
                app.send(f'cmd resize {size}')
                c = check_layout(app); worker_sized(app, workers); g = c['grid']
                if len(g['panes']) <= (g['width'] // 6) * (g['height'] // 2):
                    assert all(p['height'] >= 2 for p in g['panes']), ('a pane with no line of text', size, g)
            app.snap('tiny')
            app.send('cmd resize 1300,760')
            c = check_layout(app); worker_sized(app, workers)

            # Dividers dragged far past the window, both ways, on both axes.
            cw, ch = c['cell']['width'], c['cell']['height']
            first = c['grid']['panes'][0]; second = c['grid']['panes'][1]
            x = second['x'] * cw; y = (second['y'] + 3) * ch
            for to in (-5000, 50000, x + 7 * cw):
                app.send(f'cmd pointer down {x},{y}'); app.send(f'cmd pointer drag {to},{y}'); app.send(f'cmd pointer up {to},{y}')
                c = check_layout(app); x = c['grid']['panes'][1]['x'] * cw
            row = next(p for p in c['grid']['panes'] if p['y'] > first['y'])
            y = row['y'] * ch; x = (row['x'] + 3) * cw
            for to in (-5000, 50000, c['grid']['height'] * ch // 2):
                app.send(f'cmd pointer down {x},{y}'); app.send(f'cmd pointer drag {x},{to}'); app.send(f'cmd pointer up {x},{to}')
                c = check_layout(app); y = next(p for p in c['grid']['panes'] if p['y'] > first['y'])['y'] * ch
            worker_sized(app, workers)

            # A click on a divider that doesn't move leaves it where it is.
            c = app.chrome(); before = c['grid']['panes']; p = before[1]
            for dx in (-3, -1, 0, 2, 4):
                x = p['x'] * cw + dx; y = (p['y'] + 3) * ch
                app.send(f'cmd pointer down {x},{y}'); app.send(f'cmd pointer up {x},{y}')
                assert grid(app)['panes'] == before, ('a still click moved a divider', dx)

            # Focus moves between panes: the pane left gets CSI O, the one chosen CSI I.
            (a, pa), (b, pb) = workers[0], workers[1]
            app.send('cmd focus 1')       # (behind other windows, AppKit may have resigned it)
            c = app.chrome(); p = pane(app, a); click(app, c, p['x'] + 2, p['y'] + 2)
            before_a, before_b = data(pa)['input'], data(pb)['input']
            p = pane(app, b); click(app, c, p['x'] + 2, p['y'] + 2)
            wait_for(lambda: data(pa)['input'][len(before_a):].endswith('1b5b4f'), description='focus out to the pane left')
            wait_for(lambda: data(pb)['input'][len(before_b):].endswith('1b5b49'), description='focus in to the pane chosen')

            # Mouse reports carry coordinates inside the pane, not the window.
            identity, path = workers[4]
            c = app.chrome(); p = pane(app, identity)
            assert p['x'] > 0 and p['y'] > 0, p
            assert p['height'] >= 5, p
            before = data(path)['input']
            # The click that picks a pane reaches its program too (as tmux does): cell (2, 0), then (5, 3)
            click(app, c, p['x'] + 2, p['y'] + 1)
            click(app, c, p['x'] + 5, p['y'] + 4)
            want = b'\x1b[<0;3;1M\x1b[<0;3;1m\x1b[<0;6;4M\x1b[<0;6;4m'.hex()
            try:
                wait_for(lambda: data(path)['input'][len(before):] == want, timeout=5)
            except AssertionError:
                raise AssertionError(('pane-relative SGR mouse', bytes.fromhex(data(path)['input'][len(before):]), p,
                                      [(t['id'], t['mouse'], t['cols'], t['rows']) for t in app.state()['tabs']], app.state()['active']))

            # A pane that isn't focused still repaints when its program prints.
            (quiet, _), (busy, pbusy) = workers[5], workers[6]
            c = app.chrome(); p = pane(app, quiet); click(app, c, p['x'] + 2, p['y'] + 2)
            # (each control command paints once itself: compare one quiet second with one that has output)
            time.sleep(1); start = app.state()['paints']; time.sleep(1)
            quiet_frames = app.state()['paints'] - start
            start = app.state()['paints']
            pbusy.with_suffix('.say').write_bytes(b'background pane line\r\n'); time.sleep(1)
            busy_frames = app.state()['paints'] - start
            assert busy_frames > quiet_frames, ('no frame for output in a pane not focused', quiet_frames, busy_frames)
            assert app.state()['tabs'][[t['id'] for t in app.state()['tabs']].index(busy)]['activity'] == 0, \
                'a pane on screen was marked unread'

            # A bell in a pane not focused marks its title, as the tab menu does.
            pbusy.with_suffix('.say').write_bytes(b'\a')
            wait_for(lambda: pane(app, busy)['title'].split(' ', 1)[0].endswith('!'), description='attention mark in the pane title')

            # Scrolling over a pane scrolls it and leaves focus where it was.
            pbusy.with_suffix('.flood').write_text('')
            wait_for(lambda: not pbusy.with_suffix('.flood').exists(), description='flood in the scrolled pane')
            wait_for(lambda: app.state()['tabs'][[t['id'] for t in app.state()['tabs']].index(busy)]['hist'] > 100, description='scrollback')
            c = app.chrome(); p = pane(app, busy); active = app.state()['active']
            app.send(f"cmd wheel {int((p['x'] + 4) * cw)},{int((p['y'] + 4) * ch)},5")
            s = app.state(); t = next(t for t in s['tabs'] if t['id'] == busy)
            assert s['active'] == active and t['view'] > 0, (s['active'], active, t['view'])

            # Right and middle clicks go to the pane under the pointer.
            identity, path = workers[4]
            c = app.chrome(); p = pane(app, identity); before = data(path)['input']
            press(app, c, p['x'] + 3, p['y'] + 2, 'right-down'); press(app, c, p['x'] + 3, p['y'] + 2, 'right-up')
            wait_for(lambda: bytes.fromhex(data(path)['input'][len(before):]) == b'\x1b[<2;4;2M\x1b[<2;4;2m',
                     description='right click reported to the pane under it')
            assert app.state()['tabs'][app.state()['active']]['id'] == identity

            # Any-motion reports only while the pointer is over the focused pane.
            identity, path = workers[2]
            c = app.chrome(); p = pane(app, identity); click(app, c, p['x'] + 2, p['y'] + 2)
            time.sleep(.3); before = data(path)['input']
            other = pane(app, workers[3][0])
            for k in range(3):
                press(app, c, other['x'] + 2 + k, other['y'] + 2, 'move')
            time.sleep(.5)
            assert data(path)['input'] == before, ('motion over another pane reported', bytes.fromhex(data(path)['input'][len(before):]))
            press(app, c, p['x'] + 3, p['y'] + 2, 'move')
            wait_for(lambda: bytes.fromhex(data(path)['input'][len(before):]) == b'\x1b[<35;4;2M', description='motion over its own pane')

            # Output floods in every pane while a divider moves.
            for _, path in workers:
                path.with_suffix('.flood').touch()
            c = app.chrome(); p = c['grid']['panes'][1]; x = p['x'] * cw; y = (p['y'] + 3) * ch
            t0 = time.time()
            app.send(f'cmd pointer down {x},{y}')
            for k in range(40):
                app.send(f'cmd pointer drag {x + (k % 9 - 4) * cw},{y}')
            app.send(f'cmd pointer up {x},{y}')
            app.state(); took = time.time() - t0
            wait_for(lambda: not any(p.with_suffix('.flood').exists() for _, p in workers), description='floods started')
            check_layout(app); worker_sized(app, workers)
            print(f'  flood + 40 divider steps answered in {took:.2f}s')

            # Closing the focused pane gives the next one focus (CSI I) ...
            (a, pa), (b, pb) = workers[0], workers[1]
            c = app.chrome(); p = pane(app, a); click(app, c, p['x'] + 2, p['y'] + 2)
            before_b = data(pb)['input']
            app.send('cmd focus 1'); app.send('cmd closetab')
            wait_for(lambda: data(pb)['input'][len(before_b):].endswith('1b5b49'), description='focus in after the focused pane closed')
            # ... and never reveals a pane that was hidden.
            ids = [t['id'] for t in app.state()['tabs']]
            hidden = ids[ids.index(b) + 1]
            app.send(f'cmd grid-hide {ids.index(hidden)}')
            app.send(f'cmd tab {ids.index(b)}'); app.send('cmd closetab')
            wait_for(lambda: b not in [t['id'] for t in app.state()['tabs']], description='pane closed')
            assert hidden not in [p['id'] for p in grid(app)['panes']], 'closing a pane revealed a hidden one'

            # Closing the active pane leaves a visible pane active, and input goes there.
            for _ in range(3):
                s = app.state(); closing = s['tabs'][s['active']]['id']
                app.send('cmd closetab')
                wait_for(lambda: closing not in [t['id'] for t in app.state()['tabs']], description='pane closed')
                s = app.state(); g = grid(app)
                assert s['tabs'][s['active']]['id'] in [p['id'] for p in g['panes']], (s['active'], g)
                check_layout(app)
            alive = [(i, p) for i, p in workers if i in [t['id'] for t in app.state()['tabs']]]
            identity, path = alive[-1]
            c = app.chrome(); p = pane(app, identity); click(app, c, p['x'] + 2, p['y'] + 2)
            before = data(path)['input']
            app.send('key 0 0 q q')
            wait_for(lambda: data(path)['input'][len(before):].endswith('71'), description='keys to the chosen pane')

            # Hiding every pane: keys go nowhere; a new tab shows up in the grid.
            shown = [p['id'] for p in grid(app)['panes']]
            for i, t in enumerate(app.state()['tabs']):
                if t['id'] in shown:
                    app.send(f'cmd grid-hide {i}')     # (Hide toggles)
            g = grid(app); assert g['panes'] == [], g
            snapshot = {p.name: data(p)['input'] for _, p in alive}
            app.send('key 0 0 z z'); time.sleep(.5)
            assert snapshot == {p.name: data(p)['input'] for _, p in alive}, 'a hidden pane took a key'
            app.send('cmd newtab')
            wait_for(lambda: len(grid(app)['panes']) == 1, description='a new tab joins the grid')
            check_layout(app)
            app.send('cmd grid')
            wait_for(lambda: len({(t['cols'], t['rows']) for t in app.state()['tabs']}) == 1,
                     description='every tab at the window size again')
    print('Grid stress: resize storm, tiny window, extreme drags, focus and mouse reports, floods, close and hide passed')


if __name__ == '__main__':
    run()
