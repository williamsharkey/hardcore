"""Capture disposable CLI screens; no model prompts and no existing sessions touched.
Requires pyte. Run from repository root. Stored screens exclude ANSI and OSC data.
"""
import os, pty, select, signal, struct, fcntl, termios, time, json, subprocess
from pathlib import Path
import pyte
OUT=Path(__file__).resolve().parent
COLS,ROWS=128,36
screens=[]
for app,args in [('codex',['--no-daemon','--no-alt-screen']),('claude',[])]:
    screen=pyte.Screen(COLS,ROWS); stream=pyte.Stream(screen)
    pid,fd=pty.fork()
    if pid==0:
        os.environ['TERM']='xterm-256color'
        os.execvp(app,[app,*args])
    fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack('HHHH',ROWS,COLS,0,0))
    def collect(seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if not select.select([fd],[],[],.1)[0]: continue
            try: data=os.read(fd,65536)
            except OSError: break
            if not data: break
            stream.feed(data.decode('utf-8',errors='replace'))
            if b'\x1b[6n' in data: os.write(fd,b'\x1b[1;1R')
            if b'\x1b[c' in data: os.write(fd,b'\x1b[?1;2c')
    try:
        collect(10)
        screens.append(dict(id=app,title=app.title()+' · idle capture',source='Actual disposable '+app+' PTY, 128×36; no model request.',lines=screen.display))
        os.write(fd,b'!');collect(.5)
        os.write(fd,b'git log -3 --stat --oneline');collect(.5);os.write(fd,b'\r');collect(8)
        screens.append(dict(id=app+'-log',title=app.title()+' · repository output',source='Actual disposable '+app+' PTY after !git log -3 --stat --oneline.',lines=screen.display))
        print('captured',app,flush=True)
    finally:
        try: os.kill(pid,signal.SIGKILL)
        except ProcessLookupError: pass
        os.close(fd);os.waitpid(pid,0)
text=subprocess.check_output(['git','show','--format=short','HEAD:hardcore_mac.m'],text=True)
# A real source excerpt with long C/Objective-C lines, displayed without wrapping.
lines=text.splitlines()[2919:2955] if len(text.splitlines())>2955 else text.splitlines()[620:656]
screens.append(dict(id='source',title='Source · long-line capture',source='Actual hardcore_mac.m lines 621–656; clipped at 128 cells like a horizontal editor viewport.',lines=[x[:COLS].ljust(COLS) for x in lines]))
log=Path('build/merge-test.log').read_text()
screen=pyte.Screen(COLS,ROWS);stream=pyte.Stream(screen)
stream.feed(log.replace('\n','\r\n'))
screens.append(dict(id='tests',title='Build · test-output capture',source='Actual build/merge-test.log replayed into a 128×36 terminal.',lines=screen.display))
for s in screens:
    s['lines']=[x.replace(os.path.expanduser('~'),'~') for x in s['lines']]
    s['rightmost']=[len(x.rstrip()) for x in s['lines']]
(OUT/'captures.json').write_text(json.dumps(screens,ensure_ascii=False,indent=2)+'\n')
