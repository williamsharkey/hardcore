#!/usr/bin/env python3
"""Real GUI/PTY integration test; isolated app copies and config, no release upload.
Run on a logged-in Mac after `make`: python3 tests/test_update_mac.py.
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def wait_for(test, timeout=20):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = test()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError("timed out waiting for update test")


def main():
    if sys.platform != "darwin":
        print("SKIP: macOS GUI required")
        return
    with tempfile.TemporaryDirectory(prefix="hardcore-update-test-") as tmp:
        root = Path(tmp)
        install = root / "installed" / "hardcore.app"
        candidate = root / "candidate" / "hardcore.app"
        shutil.copytree(ROOT / "hardcore.app", install)
        shutil.copytree(ROOT / "hardcore.app", candidate)
        exe = install / "Contents/MacOS/hardcore"
        marker = candidate / "Contents/Resources/update-test-marker"
        marker.write_text("replacement bundle\n")
        # Re-sign the modified fixture so we exercise the actual signature check.
        subprocess.run(["codesign", "--force", "--sign", "-", str(candidate)], check=True, capture_output=True)
        cfg = root / "config"
        cfg.mkdir()
        (cfg / "config.ini").write_text("font=t\ntab_underlay=1\n")
        fifo = root / "control"
        env = dict(os.environ, HARDCORE_CTL=str(fifo), HARDCORE_NO_ACTIVATE="1",
                   HARDCORE_TEST_CONFIG=str(cfg), HARDCORE_SHELL="/bin/zsh")
        worker = root / "worker.py"
        worker.write_text('''import json, os, shutil, subprocess, sys, threading, time
from pathlib import Path
root, exe, candidate = map(Path, sys.argv[1:])
(root/'identity').write_text(json.dumps({'worker':os.getpid(),'shell':os.getppid(),'command':shutil.which('hardcore')}))
def beat():
    while True:
        (root/'heartbeat').write_text(str(time.monotonic()))
        time.sleep(.05)
threading.Thread(target=beat, daemon=True).start()
print('history line\\r\\n'*80, end='')
print('\\x1b[?1049h\\x1b[?2004h\\x1b[?1000h\\x1b[?1006hLIVE FOREGROUND ─ 雪', flush=True)
for line in sys.stdin:
    if line.strip() == 'update':
        result=subprocess.run([str(exe),'update','--from',str(candidate)],capture_output=True,text=True,timeout=40)
        (root/'result').write_text(json.dumps({'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr,'worker':os.getpid()}))
    elif line.strip() == 'alive':
        (root/'alive').write_text(str(os.getpid()))
    elif line.strip() == 'quit':
        break
''')
        log = open(root / "app.log", "w+")
        proc = subprocess.Popen([str(exe), "--cd", str(root)], env=env, stdout=log, stderr=log)
        children = []
        second = None
        try:
            wait_for(fifo.exists)

            def send(command):
                fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                try:
                    os.write(fd, (command + "\n").encode())
                finally:
                    os.close(fd)

            counter = 0

            def state():
                nonlocal counter
                counter += 1
                path = root / f"state-{counter}.json"
                send(f"cmd state {path}")
                wait_for(path.exists)
                return json.loads(path.read_text())

            wait_for(lambda: Path(f"/tmp/hardcore-{os.getuid()}/{proc.pid}.sock").exists())
            send("cmd resize 901,503")
            command = shlex.join([sys.executable, str(worker), str(root), str(exe), str(candidate)])
            send("type " + command + "\\n")
            wait_for((root / "identity").exists)
            identity = json.loads((root / "identity").read_text())
            children += [identity['worker'], identity['shell']]
            assert Path(identity['command']).resolve() == exe.resolve(), identity
            wait_for(lambda: state()['tabs'][0]['alt'] == 1)
            send("cmd newtab")
            wait_for(lambda: len(state()['tabs']) == 2)
            send("type printf 'second tab history\\\\n%.0s' {1..100}\\n")
            time.sleep(.5)
            send("cmd view 5")
            before = state()
            children += [t['pid'] for t in before['tabs']]
            result = subprocess.run([str(exe), "--reload"], capture_output=True, text=True, timeout=40)
            assert result.returncode == 0, result.stdout + result.stderr
            after = state()
            assert before == after, (before, after)
            assert proc.poll() is None
            assert time.monotonic() - float((root/'heartbeat').read_text()) < 2
            print("PASS: same app PID, shell PIDs, active tab, window frame, alt screen, scrollback and scroll position after reload")

            invalid = root / "invalid.app"
            (invalid / "Contents/MacOS").mkdir(parents=True)
            (invalid / "Contents/MacOS/hardcore").write_text("not an executable")
            original = exe.read_bytes()
            result = subprocess.run([str(exe), "update", "--from", str(invalid)], capture_output=True, text=True, timeout=30)
            assert result.returncode != 0
            assert exe.read_bytes() == original and proc.poll() is None
            assert state()['tabs'] == after['tabs']
            print("PASS: invalid replacement rejected without changing installation or sessions")

            fifo2 = root / "control2"
            second = subprocess.Popen([str(exe), "--cd", str(root)],
                                      env=dict(env, HARDCORE_CTL=str(fifo2)), stdout=log, stderr=log)
            wait_for(fifo2.exists)
            wait_for(lambda: Path(f"/tmp/hardcore-{os.getuid()}/{second.pid}.sock").exists())
            second_state = root / "second-before.json"
            with open(fifo2, "w") as f:
                f.write(f"cmd state {second_state}\n")
            wait_for(second_state.exists)
            second_before = json.loads(second_state.read_text())
            children += [t['pid'] for t in second_before['tabs']]
            send("cmd tab 0")
            send("type update\\n")
            wait_for((root / "result").exists, timeout=45)
            result = json.loads((root / "result").read_text())
            assert result['returncode'] == 0 and '2 windows reloaded in place' in result['stdout'], result
            assert result['worker'] == identity['worker']
            assert (install / "Contents/Resources/update-test-marker").exists()
            assert Path(str(install) + ".previous").exists()
            after_update = state()
            assert after_update['pid'] == before['pid']
            assert [t['pid'] for t in after_update['tabs']] == [t['pid'] for t in before['tabs']]
            assert after_update['tabs'][0]['alt'] == 1
            assert after_update['tabs'][1]['view'] == 5
            send("type alive\\n")
            wait_for((root / "alive").exists)
            assert int((root/'alive').read_text()) == identity['worker']
            second_state = root / "second-after.json"
            with open(fifo2, "w") as f:
                f.write(f"cmd state {second_state}\n")
            wait_for(second_state.exists)
            second_after = json.loads(second_state.read_text())
            assert second_before['pid'] == second_after['pid']
            assert [t['pid'] for t in second_before['tabs']] == [t['pid'] for t in second_after['tabs']]
            print("PASS: update invoked inside foreground process replaces app bundle, reloads both windows, and returns to the same live process")

            # Repeat to ensure descriptors/watchers are correctly recreated.
            result = subprocess.run([str(exe), "--reload"], capture_output=True, text=True, timeout=40)
            assert result.returncode == 0, result.stdout + result.stderr
            assert state()['pid'] == proc.pid
            print("PASS: repeated live reload")
        except BaseException:
            log.flush(); log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            if second is not None:
                if second.poll() is None:
                    second.terminate()
                    second.wait(timeout=10)
                Path(f"/tmp/hardcore-{os.getuid()}/{second.pid}.sock").unlink(missing_ok=True)
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=10)
            for pid in set(children):
                try:
                    os.kill(pid, signal.SIGHUP)
                except ProcessLookupError:
                    pass
            Path(f"/tmp/hardcore-{os.getuid()}/{proc.pid}.sock").unlink(missing_ok=True)
            log.close()


if __name__ == "__main__":
    main()
