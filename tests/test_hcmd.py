"""hcmd, the Markdown reader Cmd+click opens files in: its rendering of a
fixture at fixed sizes against golden files (plain text and with styles and
OSC 8 links), plain mode with a line jump, stdin, and the live viewer in a pty
(search, resize, reload when the file changes, wheel, quit).

  python3 tests/test_hcmd.py build/hcmd          check
  python3 tests/test_hcmd.py build/hcmd --update rewrite the golden files
"""
import fcntl, os, pty, select, shutil, struct, subprocess, sys, tempfile, termios, time

HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.join(HERE, 'fixtures', 'hcmd')
fails = 0


def check(ok, what):
    global fails
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        fails += 1


def run(exe, *args, stdin=None):
    r = subprocess.run([exe, *args], input=stdin, capture_output=True, timeout=10)
    return r.returncode, r.stdout


def golden(exe, update, name, args, src):
    rc, out = run(exe, *args, src)
    out = out.replace(os.path.realpath(FIX).encode(), b'@DIR@')
    path = os.path.join(FIX, name)
    if update:
        open(path, 'wb').write(out)
    want = open(path, 'rb').read() if os.path.exists(path) else b''
    if rc != 0 or out != want:
        a, b = want.decode(errors='replace').splitlines(), out.decode(errors='replace').splitlines()
        for i in range(max(len(a), len(b))):
            if i >= len(a) or i >= len(b) or a[i] != b[i]:
                print(f'  first difference at line {i + 1}:\n  want {a[i] if i < len(a) else "<eof>"!r}\n  got  {b[i] if i < len(b) else "<eof>"!r}')
                break
    check(rc == 0 and out == want, f'{name}: hcmd {" ".join(args)} {os.path.basename(src)}')


def pty_viewer(exe):
    d = tempfile.mkdtemp(prefix='hcmd-test-')
    try:
        f = os.path.join(d, 'a.md')
        shutil.copy(os.path.join(FIX, 'sample.md'), f)
        pid, fd = pty.fork()
        if pid == 0:
            os.environ['TERM'] = 'xterm-256color'
            os.execv(exe, [exe, f])
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
        buf = bytearray()

        def pump(t):
            end = time.time() + t
            while time.time() < end:
                r, _, _ = select.select([fd], [], [], 0.05)
                if r:
                    try:
                        buf.extend(os.read(fd, 65536))
                    except OSError:
                        return

        def step(data, t, what, needle):
            buf.clear()
            if data:
                os.write(fd, data)
            pump(t)
            check(needle in buf, what)

        step(b'', 0.8, 'viewer: first screen shows the heading', b'hcmd fixture')
        step(b'/orchard\r', 0.5, 'viewer: / search finds the table cell (1/1)', b'1/1')
        buf.clear()
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 50, 0, 0))
        os.kill(pid, 28)  # SIGWINCH
        pump(0.6)
        check(b'\x1b[30;1H' in buf, 'viewer: redraws at the new size after a resize')
        with open(f, 'a') as h:
            h.write('\nAPPENDED-LINE\n')
        step(b'', 1.0, 'viewer: reloads when the file changes', b'reloaded')
        step(b'G', 0.5, 'viewer: G shows the appended line', b'APPENDED-LINE')
        step(b'g', 0.4, 'viewer: g goes back to the top', b'hcmd fixture')
        buf.clear()
        os.write(fd, b'\x1b[<65;5;5M')
        pump(0.4)
        check(b'\x1b[1;1H' in buf and b'hcmd fixture' not in buf, 'viewer: the wheel scrolls the heading off the top')
        buf.clear()
        os.write(fd, b'q')
        pump(0.4)
        _, st = os.waitpid(pid, 0)
        check(st == 0 and b'\x1b[?1049l' in buf, 'viewer: q quits and leaves the alternate screen')
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    exe = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build/hcmd')
    update = '--update' in sys.argv
    md = os.path.join(FIX, 'sample.md')
    golden(exe, update, 'sample-60x80.txt', ['--dump=60x80'], md)
    golden(exe, update, 'sample-40x80.txt', ['--dump=40x80'], md)
    golden(exe, update, 'sample-60x80.ansi', ['--dump-ansi=60x80'], md)
    golden(exe, update, 'plain-jump.txt', ['-p', '+3', '--dump=50x8'], os.path.join(FIX, 'plain.txt'))
    rc, out = run(exe, '-', stdin=b'# From stdin\n\n*hello*\n')
    check(rc == 0 and b'From stdin' in out and b'hello' in out and b'\x1b' not in out, 'stdin, piped: plain text without escapes')
    pty_viewer(exe)
    print(('FAIL' if fails else 'ok') + f' hcmd ({fails} failed)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
