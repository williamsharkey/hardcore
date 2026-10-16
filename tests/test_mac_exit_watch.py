#!/usr/bin/env python3
"""Verify exit status for non-child processes before/after orphaning."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from mac_exit_watch import ExitWatch


def run_case(code, orphan_first=False, signal_number=None):
    with tempfile.TemporaryDirectory(prefix='hc-exit-watch-') as temp:
        gate = Path(temp)/'go'
        # The watched process is a grandchild, never this observer's child.
        parent = subprocess.Popen([sys.executable, '-c', '''
import os,sys,time
from pathlib import Path
pid=os.fork()
if pid==0:
 while not Path(sys.argv[1]).exists():time.sleep(.01)
 os._exit(int(sys.argv[2]))
print(pid,flush=True)
sys.stdin.readline()
os._exit(0)
''', str(gate), str(code)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        pid = int(parent.stdout.readline())
        try:
            if orphan_first:
                parent.stdin.write('exit\n'); parent.stdin.flush(); parent.wait(timeout=5)
            with ExitWatch(pid) as watch:
                assert watch.wait(.02) is None
                if not orphan_first:
                    parent.stdin.write('exit\n'); parent.stdin.flush(); parent.wait(timeout=5)
                if signal_number:
                    os.kill(pid, signal_number)
                else:
                    gate.touch()
                result = watch.wait(5)
                assert result is not None
                if signal_number:
                    assert result['kind']=='signal' and result['signal']==signal_number, result
                else:
                    assert result['kind']=='exit' and result['exitCode']==code, result
                print('PASS', result, 'registered after orphaning:', orphan_first)
        finally:
            parent.stdin.close(); parent.stdout.close()
            if parent.poll() is None:parent.kill();parent.wait()
            try:os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:pass


for orphan_first in (False, True):
    for code in (0, 7, 42):run_case(code, orphan_first)
    run_case(0, orphan_first, signal.SIGTERM)

dead = subprocess.Popen(['/usr/bin/true']); dead.wait()
try:
    with ExitWatch(dead.pid):
        raise AssertionError('already-reaped PID accepted')
except ProcessLookupError:
    print('PASS already-exited process rejected')
