#!/usr/bin/env python3
"""Exercise native receiver failure paths and rendered-PTY attachment receipts.

Requires macOS with WindowServer and a built Hardcore executable. All homes,
configuration, sockets, records, and processes are disposable; no Terminal.app,
real Claude installation, credentials, or inference is used.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("claude_handoff", REPO / "tools/claude_handoff.py")
handoff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(handoff)


def wait_for(check, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError("timed out waiting for native receiver state")


def exact(sock, length):
    output = b""
    while len(output) < length:
        part = sock.recv(length - len(output))
        if not part:
            raise EOFError("window closed")
        output += part
    return output


def windows(runtime):
    found = {}
    for path in runtime.glob("*.sock"):
        try:
            with socket.socket(socket.AF_UNIX) as sock:
                sock.settimeout(1)
                sock.connect(str(path))
                message = plistlib.dumps({"op": "list"}, fmt=plistlib.FMT_BINARY)
                sock.sendall(struct.pack("!I", len(message)) + message)
                length = struct.unpack("!I", exact(sock, 4))[0]
                assert length < 8 * 1024 * 1024
                data = plistlib.loads(exact(sock, length))
                found[data["pid"]] = data
        except (OSError, EOFError):
            pass
    return found


def probe(executable, root):
    config_root = root / "home"
    (config_root / "Library/Application Support").mkdir(parents=True)
    project = root / "project ; café\ncontinuation"
    project.mkdir()
    runtime = root / "run"
    runtime.mkdir(mode=0o700)
    command_dir = root / "bin"
    command_dir.mkdir()
    fake = command_dir / "claude"
    listing = root / "jobs.json"
    unexpected = root / "unexpected-attach"
    attach_mode = root / "attach-mode.json"
    attach_client = root / "attach-client.json"
    show_marker = root / "show-marker"
    scroll_marker = root / "scroll-marker"
    scrolled_marker = root / "scrolled-marker"
    exit_client = root / "exit-client"
    received_input = root / "received-input"
    fake.write_text("#!" + sys.executable + "\n" +
        "import json,os,pathlib,select,sys,time,tty\n" +
        "if sys.argv[1:2] == ['agents']:\n" +
        "    print(pathlib.Path(" + repr(str(listing)) + ").read_text())\n" +
        "else:\n" +
        "    pathlib.Path(" + repr(str(unexpected)) + ").write_text(json.dumps(sys.argv))\n" +
        "    mode=pathlib.Path(" + repr(str(attach_mode)) + ")\n" +
        "    if not mode.exists(): raise SystemExit(97)\n" +
        "    config=json.loads(mode.read_text())\n" +
        "    tty.setraw(0)\n" +
        "    pathlib.Path(" + repr(str(attach_client)) + ").write_text(json.dumps({'pid':os.getpid(),'tty':os.ttyname(0)}))\n" +
        "    print('Disposable attachment is running; verification marker has not been sent.\\r\\n\\x1b[6n',end='',flush=True)\n" +
        "    deadline=time.monotonic()+90\n" +
        "    while not pathlib.Path(" + repr(str(show_marker)) + ").exists():\n" +
        "        if time.monotonic()>deadline: raise SystemExit(98)\n" +
        "        if select.select([0],[],[],0)[0]:\n" +
        "            with pathlib.Path(" + repr(str(received_input)) + ").open('ab') as stream: stream.write(os.read(0,4096))\n" +
        "        time.sleep(.02)\n" +
        "    print(config['marker']+'\\r\\n'+config['cursorMarker']+'\\r',flush=True)\n" +
        "    scrolled=False\n" +
        "    while not pathlib.Path(" + repr(str(exit_client)) + ").exists():\n" +
        "        if time.monotonic()>deadline: raise SystemExit(99)\n" +
        "        if select.select([0],[],[],0)[0]:\n" +
        "            with pathlib.Path(" + repr(str(received_input)) + ").open('ab') as stream: stream.write(os.read(0,4096))\n" +
        "        if not scrolled and pathlib.Path(" + repr(str(scroll_marker)) + ").exists():\n" +
        "            for index in range(100): print('Later output line '+str(index)+'\\r')\n" +
        "            sys.stdout.flush()\n" +
        "            pathlib.Path(" + repr(str(scrolled_marker)) + ").touch()\n" +
        "            scrolled=True\n" +
        "        time.sleep(.02)\n")
    fake.chmod(0o700)
    env = dict(os.environ, HOME=str(config_root), HARDCORE_RUNTIME_DIR=str(runtime),
               HARDCORE_CTL=str(root / "control"), HARDCORE_NO_ACTIVATE="1",
               HARDCORE_SHELL="/bin/bash", PATH=str(command_dir) + os.pathsep + os.environ.get("PATH", ""))
    source = subprocess.Popen(["/bin/sleep", "180"], start_new_session=True)
    sid = str(uuid.uuid4())
    listing.write_text(json.dumps([{"sessionId": sid, "kind": "interactive", "name": "Disposable source",
                                   "cwd": str(project), "pid": source.pid}]))
    owned = {}
    destination_process = None

    def remember(pid):
        identity = handoff.process_identity(pid)
        assert identity, f"own process {pid} was not alive"
        owned[pid] = identity
        return identity

    def stop_owned(pid):
        identity = owned.get(pid)
        if identity and handoff.process_identity(pid) == identity:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def cli(action, data, success=True):
        # A new group models a mod process.run supervisor. Only this group's
        # descendants may receive the later test signal.
        process = subprocess.Popen([str(executable), "claude", action], cwd=project, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, start_new_session=True)
        stdout, stderr = process.communicate(json.dumps({"protocol": 1, **data}), timeout=30)
        (root / (action + ".log")).write_text(stdout + stderr)
        result = json.loads(stdout)
        assert bool(result.get("ok")) == success, result
        assert (process.returncode == 0) == success, (process.returncode, result)
        return result, process.pid

    def prepare(snapshot=False):
        before = set(windows(runtime))
        data = {"sourceSessionId": sid, "cwd": str(project)}
        if snapshot:
            data['snapshot'] = {'draft': {'text': '', 'cursor': 0}, 'history': {
                'format': 'api', 'count': 0, 'digest': hashlib.sha256(b'[]').hexdigest(), 'complete': False}}
        result, launcher = cli("prepare", data)
        record_path = config_root / "Library/Caches/hardcore/claude-handoff" / (result["requestId"] + ".json")
        record = handoff.read_json(record_path)
        receiver = handoff.read_json(record_path.with_suffix(".receiver.json"))
        assert record["sourcePid"] == source.pid and record["state"] == "prepared"
        assert remember(receiver["pid"]) == receiver["identity"]
        window = wait_for(lambda: next((pid for pid in windows(runtime) if pid not in before), None))
        remember(window)
        wait_for(lambda: any(tab.get("cwd") == str(project) for tab in windows(runtime).get(window, {}).get("tabs", [])))
        assert os.getpgid(window) != launcher, "GUI remained in the temporary launcher's process group"
        assert os.getpgid(window) != os.getpgid(source.pid)
        try:
            os.killpg(launcher, signal.SIGTERM)
        except ProcessLookupError:
            pass
        time.sleep(.2)
        assert handoff.process_identity(window) == owned[window], "launch-group cleanup killed the receiving GUI"
        assert handoff.process_identity(receiver["pid"]) == receiver["identity"], "launch-group cleanup killed the waiting client"
        return result, record_path, receiver, window

    def receipt(window, request_id, client_pid, tty, action='receipt'):
        response = subprocess.run([str(executable), '--claude-' + action, str(window),
            request_id, str(client_pid), tty], env=env, capture_output=True, text=True, timeout=5)
        assert response.returncode in (0, 1), (response.returncode, response.stdout, response.stderr)
        result = json.loads(response.stdout)
        assert bool(result.get('ok')) == (response.returncode == 0), result
        return result

    def user_input(text):
        with (root / 'control').open('w') as stream:
            stream.write('type ' + text + '\n')

    try:
        result, path, receiver, window = prepare()
        print("PASS native receiver has an independent process group and survives launcher-group cleanup", flush=True)
        cli("cancel", {"requestId": result["requestId"], "sourceSessionId": sid})
        assert handoff.read_json(path)["state"] == "cancelled"
        wait_for(lambda: handoff.process_identity(receiver["pid"]) != receiver["identity"])
        assert source.poll() is None, "cancellation killed the source process"
        assert not unexpected.exists(), "cancelled handoff tried to attach"
        print("PASS cancellation exits the waiting client and preserves the original source PID", flush=True)
        stop_owned(window)
        wait_for(lambda: handoff.process_identity(window) != owned[window])

        result, path, receiver, window = prepare()
        stop_owned(receiver["pid"])
        wait_for(lambda: handoff.process_identity(receiver["pid"]) != receiver["identity"])
        refused, _ = cli("arm", {"requestId": result["requestId"], "sourceSessionId": sid}, success=False)
        assert "receiving client" in refused.get("error", ""), refused
        assert handoff.read_json(path)["state"] == "prepared"
        assert source.poll() is None, "receiver failure killed the source process"
        assert not unexpected.exists(), "failed receiver tried to attach"
        cli("cancel", {"requestId": result["requestId"], "sourceSessionId": sid})
        print("PASS dead receiver prevents arming; the original source PID remains alive", flush=True)
        stop_owned(window)
        wait_for(lambda: handoff.process_identity(window) != owned[window])

        result, path, receiver, window = prepare(snapshot=True)
        cli("arm", {"requestId": result["requestId"], "sourceSessionId": sid})
        destination_process = subprocess.Popen(["/bin/sleep", "180"], start_new_session=True)
        destination_sid = str(uuid.uuid4())
        target = {"sessionId": destination_sid, "kind": "background", "name": result["marker"],
                  "cwd": str(project), "id": destination_sid[:8], "pid": destination_process.pid, "status": "idle"}
        listing.write_text(json.dumps([target]))
        time.sleep(.3)
        assert not unexpected.exists(), "announced but unclaimed destination was attached"
        claimed, _ = cli("claim", {"sessionId": destination_sid, "cwd": str(project)})
        assert claimed["requestId"] == result["requestId"]
        target["name"] = claimed["originalName"]
        listing.write_text(json.dumps([target]))
        time.sleep(.3)
        assert not unexpected.exists(), "claimed destination was attached before its ready acknowledgement"
        attach_mode.write_text(json.dumps({'marker': 'HC-RECEIPT:' + result['requestId'],
                                          'cursorMarker': 'HC-CURSOR:' + result['requestId']}))
        cli("ready", {"requestId": result["requestId"], "sessionId": destination_sid})
        wait_for(unexpected.exists)
        assert json.loads(unexpected.read_text()) == [str(fake), "attach", destination_sid[:8]]
        record = handoff.read_json(path)
        assert record["state"] == "attaching" and record["destinationSession"] == destination_sid
        assert record["destinationPid"] == destination_process.pid and record["destinationReadyAt"] > 0
        assert source.poll() is None
        print("PASS native receiver attaches the exact job only after explicit destination readiness", flush=True)
        print("PASS recovery directory survives OSC7 encoding of spaces, semicolon, newline, and Unicode", flush=True)
        wait_for(attach_client.exists)
        client = json.loads(attach_client.read_text())
        assert client['pid'] == receiver['pid'], 'attach did not retain the expected client PID'
        remember(client['pid'])
        assert receipt(window, result['requestId'], client['pid'], client['tty'], 'guard')['inputHeld']
        assert not receipt(window, result['requestId'], client['pid'], client['tty'], 'cursor')['ok']
        user_input('HCPROBE_BLOCKED')
        wait_for(lambda: received_input.exists() and b'R' in received_input.read_bytes())
        time.sleep(.2)
        assert b'HCPROBE_BLOCKED' not in received_input.read_bytes()
        print('PASS restore guard blocks typed input while terminal query replies still reach the client', flush=True)
        assert not receipt(window, result['requestId'], client['pid'], client['tty'])['ok'], \
            'startup alone was accepted as a rendered attachment'
        for bad_request, bad_pid, bad_tty in [
            (uuid.uuid4().hex, client['pid'], client['tty']),
            (result['requestId'], client['pid'] + 1, client['tty']),
            (result['requestId'], client['pid'], '/dev/ttys999999')]:
            assert not receipt(window, bad_request, bad_pid, bad_tty)['ok']
        show_marker.touch()
        confirmed = wait_for(lambda: (answer if (answer := receipt(
            window, result['requestId'], client['pid'], client['tty']))['ok'] else None))
        assert confirmed == {'ok': True, 'requestId': result['requestId'],
            'windowPid': window, 'receiverPid': client['pid'], 'receiverTty': client['tty'], 'rendered': True}
        for bad_request, bad_pid, bad_tty in [
            (uuid.uuid4().hex, client['pid'], client['tty']),
            (result['requestId'], client['pid'] + 1, client['tty']),
            (result['requestId'], client['pid'], '/dev/ttys999999')]:
            assert not receipt(window, bad_request, bad_pid, bad_tty)['ok']
        print('PASS rendered receipt rejects startup, wrong request/PID/TTY, then accepts the matching PTY marker', flush=True)
        for _ in range(2):
            assert receipt(window, result['requestId'], client['pid'], client['tty'], 'cursor')['cursorSent']
        wait_for(lambda: b'\x1b[D' in received_input.read_bytes())
        assert received_input.read_bytes().count(b'\x1b[D') == 1
        assert not receipt(window, uuid.uuid4().hex, client['pid'], client['tty'], 'release')['ok']
        assert receipt(window, result['requestId'], client['pid'], client['tty'], 'guard')['inputHeld']
        print('PASS guarded cursor restoration emits exactly one Left event and rejects unrelated release', flush=True)
        scroll_marker.touch()
        wait_for(scrolled_marker.exists)
        assert handoff.process_identity(client['pid']) == owned[client['pid']]
        assert not receipt(window, result['requestId'], client['pid'], client['tty'])['ok'], \
            'a marker retained only in scrollback was accepted as visible attachment proof'
        print('PASS a marker scrolled outside the visible viewport cannot certify the rendered attachment', flush=True)
        assert receipt(window, result['requestId'], client['pid'], client['tty'], 'release')['released']
        assert not receipt(window, result['requestId'], client['pid'], client['tty'], 'guard')['ok']
        assert not receipt(window, result['requestId'], client['pid'], client['tty'], 'cursor')['ok']
        user_input('HCPROBE_RELEASED')
        wait_for(lambda: b'HCPROBE_RELEASED' in received_input.read_bytes())
        print('PASS verified release restores typing and cannot send a later cursor correction', flush=True)
        exit_client.touch()
        wait_for(lambda: handoff.process_identity(client['pid']) != owned[client['pid']])
        assert not receipt(window, result['requestId'], client['pid'], client['tty'])['ok'], \
            'a departed client left a reusable attachment receipt'
        print('PASS a stale rendered marker cannot certify a departed attachment client', flush=True)
    finally:
        for pid in list(owned):
            stop_owned(pid)
        if source.poll() is None:
            source.terminate()
        source.wait(timeout=5)
        if destination_process is not None:
            if destination_process.poll() is None:
                destination_process.terminate()
            destination_process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=REPO / "build/hardcore")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("this native window check requires macOS")
    root = Path(tempfile.mkdtemp(prefix="hc-claude-window-", dir="/tmp")).resolve()
    try:
        probe(args.executable.resolve(strict=True), root)
    except BaseException:
        print("Private diagnostics:", root, file=sys.stderr)
        raise
    else:
        shutil.rmtree(root)


if __name__ == "__main__":
    main()
