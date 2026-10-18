#!/usr/bin/env python3
"""Input and pty lifecycle cases from other terminals' trackers, in an isolated
native app: keys mid-composition belong to the input method (ghostty#4634),
Ctrl+letter on a non-Latin layout (claude-code#19704), F13-F20, motion
reported once per cell, no app descriptors in a shell (the control FIFO and
Metal's cache files leaked before), and a shell whose background job writes
as it exits (yes & exit) still closes promptly."""
import os, signal, tempfile, time
from pathlib import Path
from test_chrome_mac import NativeApp, ROOT, wait_for
from test_grid_stress_mac import WORKER, data, start_worker, press

CONTROL = 0x40000


def fresh(app, path, before):
    return bytes.fromhex(data(path)['input'][len(before):])


def run():
    with tempfile.TemporaryDirectory(prefix='hardcore-input-') as root:
        with NativeApp(Path(root), 2, ROOT / 'build/input-native') as app:
            (app.root / 'worker.py').write_text(WORKER)
            app.send('cmd focus 1')
            path = app.root / 'keys'
            start_worker(app, path, b'\x1b[?1003h\x1b[?1006h')

            # Mid-composition, Return is the input method's: nothing reaches the pty.
            before = data(path)['input']
            app.send('cmd mark に')
            wait_for(lambda: app.state()['preedit'] == 1, description='composed text shown at the cursor')
            app.send('key 36 0 \\x0d \\x0d'); app.send('key 125 0 \\uf701 \\uf701'); app.send('key 51 0 \\x7f \\x7f')
            time.sleep(.5)
            assert fresh(app, path, before) == b'', fresh(app, path, before)
            app.send('cmd commit に')
            app.send('key 36 0 \\r \\r')
            wait_for(lambda: fresh(app, path, before) == 'に\r'.encode(), description='committed text, then Return')
            assert app.state()['preedit'] == 0

            # Ctrl+C on a Russian layout: the key's character is Cyrillic, the byte is still ^C.
            before = data(path)['input']
            app.send(f'key 8 {CONTROL:x} с с')
            app.send(f'key 0 {CONTROL:x} ф ф')
            wait_for(lambda: fresh(app, path, before) == b'\x03\x01', description=f'Ctrl on a Cyrillic layout ({fresh(app, path, before)!r})')

            # F13-F20 send their xterm sequences; nothing private-use leaks out.
            before = data(path)['input']
            for code in (105, 107, 113, 106, 64, 79, 80, 90):
                app.send(f'key {code} 0 - -')
            app.send('key 71 0 \\uf739 \\uf739')      # keypad Clear: no sequence, so nothing
            want = b''.join(b'\x1b[%d~' % n for n in (25, 26, 28, 29, 31, 32, 33, 34))
            wait_for(lambda: fresh(app, path, before) == want, description=f'F13-F20 ({fresh(app, path, before)!r})')

            # Any-motion: one report per cell, however many pixels the pointer moves in it.
            c = app.chrome(); cw, ch = c['cell']['width'], c['cell']['height']
            before = data(path)['input']
            col, row = c['dock']['width'] + 5, 6
            for dx in range(0, cw, 2):
                app.send(f'cmd pointer move {col * cw + dx},{row * ch + ch // 2}')
            app.send(f'cmd pointer move {(col + 1) * cw + 1},{row * ch + ch // 2}')
            time.sleep(.5)
            reports = fresh(app, path, before).split(b'\x1b[<')[1:]
            assert len(reports) == 2, ('motion within one cell reported more than once', reports)

            # A shell gets no descriptors of the app's beyond its pty.
            listing = app.root / 'fds.txt'
            tabs = len(app.state()['tabs'])
            app.send('cmd newtab')
            wait_for(lambda: len(app.state()['tabs']) > tabs, description='second shell')
            time.sleep(1)
            app.send(f'type lsof -p $$ -F fn > {listing}.tmp; mv {listing}.tmp {listing}\\n')
            wait_for(lambda: listing.exists(), timeout=30, description='fd listing')
            fds, fd = {}, None
            for line in listing.read_text().split():
                if line[0] == 'f': fd = line[1:]
                elif line[0] == 'n' and fd and fd.isdigit(): fds[int(fd)] = line[1:]
            tty = fds.get(0)
            assert tty and tty.startswith('/dev/tty'), fds
            assert all(name == tty for name in fds.values()), ('the shell inherited the app\'s descriptors', fds)

            # A background job that outlives its shell keeps writing; the tab still ends.
            pidfile = app.root / 'yes.pid'
            count = len(app.state()['tabs'])
            app.send(f'type yes > /dev/tty &! echo $! > {pidfile}; sleep .3; exit\\n')
            wait_for(lambda: pidfile.exists() and pidfile.read_text().strip(), description='writer started')
            app.pids.add(int(pidfile.read_text()))
            t0 = time.time()
            wait_for(lambda: len(app.state()['tabs']) < count, timeout=10, description='tab ends while its job writes')
            took = time.time() - t0
            t0 = time.time(); app.state(); answer = time.time() - t0
            assert answer < 1, ('the app is slow to answer after the tab ended', answer)
            try:
                os.kill(int(pidfile.read_text()), signal.SIGKILL)
            except ProcessLookupError:
                pass
            print(f'  tab with a writing job closed in {took:.2f}s')
    print('Native input: composition keys, Ctrl on Cyrillic, F13-F20, motion per cell, no inherited fds, exit with a writer passed')


if __name__ == '__main__':
    run()
