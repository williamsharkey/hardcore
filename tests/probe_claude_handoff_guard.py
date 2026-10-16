#!/usr/bin/env python3
"""Opt-in checks of Hardcore's Claude handoff guard.

Uses private Claude settings, a disposable PTY, and an empty project. Plugins
are installed only after Claude starts. No user session or Terminal.app UI is
controlled. Default checks send no model prompt; --model-turn sends one tiny
prompt to verify the real completion boundary. Credentials copied for onboarding
are removed even if the probe fails; diagnostics remain private under /tmp.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import plistlib
import pty
import re
import select
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time


ROOT = Path(__file__).resolve().parents[1]


def contains(screen, text):
    # Incremental terminal rendering often moves the cursor instead of writing
    # space bytes. Match content independently of those layout operations.
    return re.sub(r"\s+", "", text) in re.sub(r"\s+", "", screen)

OBSERVER = r'''
export function register(on) {
  let count = 0;
  let events = [];
  on('session.start', async ($, e, next) => {
    $.clock.every(150, async () => {
      const sessionId = await $.session.id();
      await $.fs.write('handoff-probe-state.json', JSON.stringify({
        sessionId, prompt: await $.prompt.read(), count: ++count,
      }));
      await $.fs.write('handoff-probe-api-' + sessionId + '.json', JSON.stringify(await $.session.messages({as: 'api'})));
    });
    return next(e);
  });
  on('command.run', {command: 'background'}, async ($, e, next) => {
    const before = {sessionId: await $.session.id(), command: e.command,
      origin: e.origin, prompt: await $.prompt.read()};
    const result = await next(e);
    await $.fs.write('handoff-probe-background.json', JSON.stringify({...before, result}));
    return result;
  });
  on('classic.Stop', async ($, e, next) => {
    const result = await next(e);
    events.push({event: 'Stop', data: e, result});
    await $.fs.write('handoff-probe-events.json', JSON.stringify(events));
    return result;
  });
  on('turn.complete', async ($, e, next) => {
    const result = await next(e);
    events.push({event: 'turn.complete', data: e});
    await $.fs.write('handoff-probe-events.json', JSON.stringify(events));
    return result;
  });
}
'''

# No production decision/state is changed. This hook only lets the same caller
# identity as the production guard issue an intentionally unprepared request,
# so the *other* production plugin's dispatch reaches its execution-time guard.
REQUEST_INJECTION = r'''
  on('command.run', {command: 'hc-probe-unprepared'}, async ($, e) => {
    $.clock.after(400, async () => {
      const result = await $.command.run({command: 'hardcore-slurp-dispatch',
        args: '00000000000040008000000000000001'});
      await $.fs.write('handoff-probe-dispatch.json', JSON.stringify(result));
    });
    return {text: 'HCPROBE: intentionally unprepared dispatch scheduled'};
  });
'''


class Probe:
    def __init__(self, root):
        self.root = root
        self.work = root / "work"
        self.work.mkdir()
        self.config = root / "config"
        self.config.mkdir(mode=0o700)
        (self.work / ".claude").mkdir()
        (self.work / ".claude/settings.json").write_text("{}")
        self.command = shutil.which("claude")
        assert self.command, "Claude Code is not installed"
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith("CLAUDE") and k not in (
                        "TMUX", "HARDCORE_WINDOW_PID", "ANTHROPIC_API_KEY")}
        self.env.update(TERM="xterm-256color", COLORTERM="truecolor",
                        CLAUDE_CONFIG_DIR=str(self.config), DISABLE_AUTOUPDATER="1",
                        CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC="1")
        self.proc = None
        self.master = None
        self.raw = bytearray()
        self.background_job = None
        self.receiver_identities = {}
        self.fixture_command = None

    @staticmethod
    def identity(pid):
        result = subprocess.run(["ps", "-p", str(pid), "-o", "lstart=", "-o", "args="],
                                capture_output=True, text=True, timeout=2)
        return result.stdout.strip() or None

    def isolate_receiver(self):
        self.home = self.root / "home"
        (self.home / "Library/Application Support").mkdir(parents=True)
        self.runtime = self.root / "run"
        self.control = self.root / "receiver.ctl"
        self.env.update(HOME=str(self.home), HARDCORE_RUNTIME_DIR=str(self.runtime),
                        HARDCORE_CTL=str(self.control), HARDCORE_NO_ACTIVATE="1",
                        HARDCORE_SHELL="/bin/bash")

    def foreground_work(self):
        job = self.work / "handoff-job.py"
        job.write_text("import os,time\nfrom pathlib import Path\n"
            "root=Path(__file__).parent\n"
            "with (root/'handoff-job-launches').open('a') as f:f.write(str(os.getpid())+'\\n')\n"
            "time.sleep(1.5)\n"
            "(root/'handoff-job-done').write_text(str(os.getpid()))\n"
            "print('HCPROBE_JOB_DONE',flush=True)\n")
        self.fixture_command = shlex.join([sys.executable, str(job)])

    def windows(self):
        found = []
        if not hasattr(self, "runtime"):
            return found
        for path in self.runtime.glob("*.sock"):
            try:
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(1)
                    connection.connect(str(path))
                    data = plistlib.dumps({"op": "list"}, fmt=plistlib.FMT_BINARY)
                    connection.sendall(struct.pack("!I", len(data)) + data)

                    def exact(count):
                        value = b""
                        while len(value) < count:
                            part = connection.recv(count - len(value))
                            if not part:
                                raise EOFError("receiver closed")
                            value += part
                        return value

                    info = plistlib.loads(exact(struct.unpack("!I", exact(4))[0]))
                    identity = self.identity(info["pid"])
                    if identity and str(self.root) in identity and "--claude-window" in identity:
                        self.receiver_identities[info["pid"]] = identity
                    found.append(info)
            except (OSError, EOFError):
                continue
        return found

    def receiver_command(self, text):
        fd = os.open(self.control, os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(fd, (text + "\n").encode())
        finally:
            os.close(fd)

    def start(self):
        original = Path.home() / ".claude.json"
        if original.exists():
            data = json.loads(original.read_text())
            keep = ("hasCompletedOnboarding", "theme", "lastOnboardingVersion",
                    "oauthAccount", "autoUpdates", "hasSeenTasksHint")
            path = self.config / ".claude.json"
            path.write_text(json.dumps({k: data[k] for k in keep if k in data}))
            path.chmod(0o600)
        source = Path.home() / ".claude/.credentials.json"
        secret = None
        if source.exists():
            secret = source.read_bytes()
        elif os.uname().sysname == "Darwin":
            result = subprocess.run(["/usr/bin/security", "find-generic-password",
                "-s", "Claude Code-credentials", "-w"], capture_output=True, timeout=10)
            if result.returncode == 0:
                secret = result.stdout.strip()
        if secret:
            fd = os.open(self.config / ".credentials.json",
                         os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "wb") as stream:
                stream.write(secret)
        del secret
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 110, 0, 0))

        def setup():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        try:
            arguments = [self.command, "--setting-sources", "project",
                "--strict-mcp-config", "--mcp-config", '{"mcpServers":{}}',
                "--no-chrome", "--tools", "Bash" if self.fixture_command else ""]
            if self.fixture_command:
                arguments.extend(["--allowedTools", "Bash(" + self.fixture_command + ")"])
            self.proc = subprocess.Popen(arguments, cwd=self.work, env=self.env,
                stdin=slave, stdout=slave, stderr=slave, preexec_fn=setup)
        finally:
            os.close(slave)
        self.pump(4)
        if "Yes,Itrustthisfolder" in re.sub(r"\s+", "", self.drain()):
            self.send(b"\x1b[B\r")
            self.pump(3)
        assert self.proc.poll() is None, "Claude exited during startup"

    def send(self, data):
        os.write(self.master, data)

    def drain(self):
        while select.select([self.master], [], [], 0)[0]:
            try:
                data = os.read(self.master, 65536)
            except OSError:
                break
            if not data:
                break
            self.raw.extend(data)
            for query, reply in [(b"\x1b[6n", b"\x1b[1;1R"),
                    (b"\x1b[c", b"\x1b[?1;2c"),
                    (b"\x1b]11;?", b"\x1b]11;rgb:0000/0000/0000\x1b\\")]:
                if query in data:
                    self.send(reply)
        return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", self.raw.decode(errors="replace"))

    def pump(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.drain()
            time.sleep(0.05)

    def command_line(self, value):
        assert value.startswith("/"), "This probe never sends model prompts"
        self.send(value.encode())
        self.pump(0.25)
        self.send(b"\r")

    def wait_json(self, path, predicate=lambda value: True, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump(0.1)
            try:
                value = json.loads(path.read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                continue
            if predicate(value):
                return value
            assert self.proc.poll() is None, "Source exited unexpectedly"
        raise AssertionError(f"Expected state was not recorded in {path}")

    def cli(self, arguments, log):
        result = subprocess.run([self.command, *arguments], cwd=self.work,
            env=self.env, text=True, capture_output=True, timeout=45)
        (self.root / log).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, f"{log} failed; see private diagnostics"
        return result

    def observer(self):
        market = self.root / "observer-market"
        plugin = market / "aaa-hc-handoff-probe"
        (market / ".claude-plugin").mkdir(parents=True)
        (plugin / ".claude-plugin").mkdir(parents=True)
        (plugin / "hooks").mkdir()
        (plugin / ".claude-plugin/plugin.json").write_text(json.dumps({
            "name": "aaa-hc-handoff-probe", "version": "0.0.1",
            "description": "Disposable read-only handoff observer"}))
        (plugin / "hooks/hooks.json").write_text('{"modules":["./register.js"]}')
        (plugin / "hooks/register.js").write_text(OBSERVER)
        (market / ".claude-plugin/marketplace.json").write_text(json.dumps({
            "name": "hc-handoff-observer", "owner": {"name": "Hardcore research"},
            "plugins": [{"name": "aaa-hc-handoff-probe", "source": "./aaa-hc-handoff-probe"}]}))
        self.cli(["plugin", "validate", str(plugin)], "observer-validate.log")
        self.cli(["plugin", "marketplace", "add", "--scope", "project", str(market)],
                 "observer-marketplace.log")
        self.cli(["plugin", "install", "--scope", "project",
                  "aaa-hc-handoff-probe@hc-handoff-observer"], "observer-install.log")

    def close(self):
        if hasattr(self, "control") and self.control.exists() and self.windows():
            # Capture an exited tab's error before cleanup removes the window.
            bitmap = self.root / "final-receiver.bmp"
            try:
                self.receiver_command("snap " + str(bitmap))
                deadline = time.monotonic() + 2
                while not bitmap.exists() and time.monotonic() < deadline:
                    time.sleep(0.05)
                if bitmap.exists():
                    subprocess.run(["/usr/bin/sips", "-s", "format", "png", str(bitmap),
                        "--out", str(bitmap.with_suffix(".png"))], capture_output=True, timeout=5)
            except (OSError, subprocess.SubprocessError):
                pass
        if self.background_job:
            try:
                self.cli(["stop", self.background_job], "cleanup-background.log")
            except (AssertionError, subprocess.SubprocessError):
                pass
        self.windows()
        for pid, identity in self.receiver_identities.items():
            try:
                if self.identity(pid) == identity:
                    os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        (self.root / "source.raw").write_bytes(self.raw)
        if self.proc is not None:
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.wait(timeout=10)
        if self.master is not None:
            os.close(self.master)
        (self.config / ".credentials.json").unlink(missing_ok=True)
        (self.config / ".claude.json").unlink(missing_ok=True)
        shutil.rmtree(self.config / "backups", ignore_errors=True)


def handoff(fixture, source_id, original_turn, already_requested=False, expected_draft=None, expected_cursor=None):
    start = len(fixture.raw)
    if not already_requested:
        fixture.command_line("/hardcore-slurp")
    request_root = fixture.home / "Library/Caches/hardcore/claude-handoff"
    deadline = time.monotonic() + 60
    record = None
    path = None
    progression = []
    last_record = None
    while time.monotonic() < deadline:
        fixture.pump(0.1)
        fixture.windows()
        for candidate in request_root.glob("*.json"):
            if not re.fullmatch(r"[0-9a-f]{32}\.json", candidate.name):
                continue
            value = json.loads(candidate.read_text())
            if value.get("sourceSessionId") == source_id:
                record, path = value, candidate
                if value != last_record:
                    progression.append({"observedAt": time.time(), "record": value})
                    (fixture.root / "handoff-progression.json").write_text(
                        json.dumps(progression, indent=2))
                    last_record = value
                if value.get("jobId"):
                    fixture.background_job = value["jobId"]
        if record and record.get("state") == "attached" and fixture.proc.poll() is not None:
            break
        if contains(fixture.raw[start:].decode(errors="replace"), "Hardcore handoff stopped"):
            raise AssertionError("Adapter refused handoff; inspect private source.raw and broker records")
    assert record and record.get("state") == "attached", record
    assert fixture.proc.poll() == 0, f"Original frontend exit: {fixture.proc.poll()}"
    assert record["destinationSession"] != source_id, "Expected documented cooperative replacement session"
    assert record["destinationReadyAt"] >= record["created"], "Receiver attached without destination acknowledgement"
    assert isinstance(record["destinationPid"], int) and record["destinationPid"] > 0
    assert record["destinationIdentity"], "Destination process identity was not bound before attach"
    assert record["snapshotVerifiedAt"] >= record["destinationReadyAt"]
    assert record["nativeReceipt"]["rendered"] is True
    expected_draft = expected_draft or ""
    if expected_cursor is None:
        expected_cursor = len(expected_draft.encode('utf-16-le')) // 2
    assert record["snapshot"]["draft"]["text"] == expected_draft
    assert record["snapshot"]["draft"]["cursor"] == expected_cursor
    assert record["snapshot"]["history"]["format"] == 'api'
    actual = json.loads((fixture.work / "handoff-probe-state.json").read_text())
    assert actual["sessionId"] == record["destinationSession"] and actual["prompt"]["text"] == expected_draft, actual
    assert actual["prompt"]["cursor"] == expected_cursor, actual
    assert record.get('inputReleasedAt'), 'Successful restoration left the receiver input guarded'
    receiver = json.loads(path.with_suffix(".receiver.json").read_text())
    windows = fixture.windows()
    assert any(tab["pid"] == receiver["pid"] for window in windows for tab in window["tabs"]), windows
    attached = subprocess.run(["ps", "-p", str(receiver["pid"]), "-o", "args="],
                              capture_output=True, text=True, check=True).stdout
    assert "attach" in attached and record["jobId"] in attached, attached

    # Ask the receiver to recall a fact from the original turn. Only our own
    # Hardcore test FIFO is used; the source PTY closes before the request.
    os.close(fixture.master)
    fixture.master = None
    time.sleep(2)
    if expected_draft:
        bitmap = fixture.root / "verified-draft.bmp"
        fixture.receiver_command("snap " + str(bitmap))
        deadline = time.monotonic() + 10
        while not bitmap.exists() and time.monotonic() < deadline:
            time.sleep(.1)
        assert bitmap.exists(), "Draft screenshot was not produced"
        subprocess.run(["/usr/bin/sips", "-s", "format", "png", str(bitmap),
                       "--out", str(bitmap.with_suffix('.png'))], check=True, capture_output=True, timeout=10)
        # Clear only our verified synthetic draft, without submitting it.
        # Move to the end first, including a restored middle cursor.
        fixture.receiver_command("type " + "\\e[C" * len(expected_draft))
        time.sleep(.3)
        fixture.receiver_command("type " + "\x7f" * len(expected_draft))
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                if json.loads((fixture.work / "handoff-probe-state.json").read_text())["prompt"]["text"] == "":
                    break
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            time.sleep(.1)
        else:
            raise AssertionError("Synthetic draft was not cleared before the recall test")
    recall_prompt = "What exact single token did I ask you to reply with before the move? Reply with that token only."
    fixture.receiver_command("type " + recall_prompt)
    # Observe the typed draft before Return. Sending both FIFO commands at once
    # can make Claude treat the combined bytes as a multiline paste.
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            draft = json.loads((fixture.work / "handoff-probe-state.json").read_text())
            if (draft.get("sessionId") == record["destinationSession"] and
                    draft.get("prompt", {}).get("text") == recall_prompt):
                break
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        time.sleep(0.1)
    else:
        raise AssertionError("Attached destination did not receive the recall draft")
    fixture.receiver_command("key 36 0 \\r")
    deadline = time.monotonic() + 90
    recalled = None
    while time.monotonic() < deadline:
        try:
            events = json.loads((fixture.work / "handoff-probe-events.json").read_text())
            completed = [item["data"] for item in events if item["event"] == "turn.complete"]
            stops = [item["data"] for item in events if item["event"] == "Stop"]
            if (completed and stops and completed[-1]["turnId"] != original_turn and
                    stops[-1]["session_id"] == record["destinationSession"]):
                recalled = completed[-1]
                break
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        time.sleep(0.5)
    else:
        raise AssertionError("Receiver did not recall the original conversation")
    assert recalled["reason"] == "answer" and recalled["answer"].strip() == "HCPROBE_READY", recalled
    fixture.cli(["logs", record["jobId"]], "receiver-claude.log")
    listed = json.loads(fixture.cli(["agents", "--json", "--all", "--cwd", str(fixture.work)],
                                   "receiver-agents.json").stdout)
    destinations = [item for item in listed if item.get("sessionId") == record["destinationSession"]]
    assert len(destinations) == 1, destinations
    assert destinations[0]["pid"] == record["destinationPid"], "Destination process changed after readiness"
    bitmap = fixture.root / "receiver.bmp"
    picture = fixture.root / "receiver.png"
    fixture.receiver_command("snap " + str(bitmap))
    deadline = time.monotonic() + 10
    while not bitmap.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    assert bitmap.exists(), "Receiver screenshot was not produced"
    subprocess.run(["/usr/bin/sips", "-s", "format", "png", str(bitmap), "--out", str(picture)],
                   check=True, capture_output=True, timeout=10)
    return {"request": record, "receiver": receiver, "windows": windows,
            "progression": progression,
            "draftVerified": True, "historyProjectionVerified": True,
            "renderedReceiverVerified": True, "unsentDraft": expected_draft,
            "draftCursor": expected_cursor,
            "sourceFrontendExited": True, "originalPtyClosed": True,
            "conversationRecall": True, "recallTurn": recalled,
            "attachedListing": destinations[0], "screenshot": str(picture)}


def probe(root, model_turn=False, executable=None, wait_next_turn=False, foreground_work=False, external_draft=False):
    fixture = Probe(root)
    try:
        if executable:
            fixture.isolate_receiver()
        if foreground_work:
            fixture.foreground_work()
        fixture.start()
        fixture.observer()
        # All actual adapter hooks and dispatcher code remain the bundled
        # production code; insert only the explicitly labeled request trigger.
        market = root / "adapter-market"
        shutil.copytree(ROOT / "plugins", market)
        if executable:
            manifest_path = market / "hardcore-slurp/.claude-plugin/plugin.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["userConfig"]["executable"]["default"] = executable
            manifest_path.write_text(json.dumps(manifest))
        entry = market / "hardcore-slurp/hooks/register.js"
        source = entry.read_text()
        marker = "export function register(on, options) {"
        assert source.count(marker) == 1, "Update probe injection for the new registration signature"
        source = source.replace(marker, marker + REQUEST_INJECTION, 1)
        start_marker = "on('session.start', async ($, e, next) => {"
        assert source.count(start_marker) == 1, "Update probe injection for new session handler"
        source = source.replace(start_marker, start_marker + "\n"
            "    await $.command.register({name: 'hc-probe-unprepared',\n"
            "      description: 'Disposable unprepared handoff request', immediate: true});\n", 1)
        entry.write_text(source)
        for name in ("hardcore-slurp", "hardcore-slurp-driver"):
            fixture.cli(["plugin", "validate", str(market / name)], f"{name}-validate.log")
        fixture.cli(["plugin", "marketplace", "add", "--scope", "project", str(market)],
                    "adapter-marketplace.log")
        for name in ("hardcore-slurp", "hardcore-slurp-driver"):
            fixture.cli(["plugin", "install", "--scope", "project", f"{name}@hardcore-slurp"],
                        f"{name}-install.log")
        fixture.command_line("/reload-plugins")
        status_path = fixture.work / "handoff-probe-state.json"
        before = fixture.wait_json(status_path)
        fixture.command_line("/hardcore-slurp")
        fixture.pump(2)
        unknown_screen = fixture.drain()
        assert contains(unknown_screen, "Hardcore handoff not started"), "Late-loaded unknown state was not refused"
        assert fixture.proc.poll() is None

        fixture.command_line("/hc-probe-unprepared")
        dispatch = fixture.wait_json(fixture.work / "handoff-probe-dispatch.json")
        fixture.pump(2)
        plain_screen = fixture.drain()
        assert contains(plain_screen, "Hardcore refused an unprepared background handoff"), (
            "Production dispatcher did not reach the separate production guard")
        assert fixture.proc.poll() is None, "Unprepared dispatch exited source"
        plain = fixture.wait_json(status_path, lambda value: value["count"] > before["count"])
        assert plain["sessionId"] == before["sessionId"], "Unprepared dispatch changed the session"

        event_path = fixture.work / "handoff-probe-background.json"
        event = json.loads(event_path.read_text()) if event_path.exists() else None
        if event is not None:
            assert event["origin"] == {"kind": "plugin", "name": "hardcore-slurp-driver"}
            assert event["result"]["text"] == "Hardcore refused an unprepared background handoff."

        # The request is intentionally delayed by our test trigger. Type the
        # synthetic draft immediately after its slash command, without Enter.
        (fixture.work / "handoff-probe-dispatch.json").unlink()
        start_count = plain["count"]
        draft = "HCPROBE UNSENT DRAFT: preserve exact text"
        fixture.command_line("/hc-probe-unprepared")
        fixture.pump(0.1)
        fixture.send(draft.encode())
        fixture.wait_json(fixture.work / "handoff-probe-dispatch.json")
        after = fixture.wait_json(status_path, lambda value:
            value["count"] > start_count and value["prompt"]["text"] == draft)
        fixture.pump(1)
        assert fixture.proc.poll() is None, "Draft test exited source"
        assert after["sessionId"] == before["sessionId"]
        assert re.sub(r"\s+", "", fixture.drain()).count("Hardcorerefusedanunpreparedbackgroundhandoff") >= 2
        result = {"sourcePid": fixture.proc.pid, "sourceStillRunning": True,
            "noLaunchTimePlugin": True, "noModelPromptSent": True,
            "testOnlyUnpreparedRequestTrigger": True,
            "sessionId": before["sessionId"], "dispatch": dispatch,
            "observedBackgroundEvent": event, "unsentDraft": after["prompt"],
            "limitation": "Unknown-state refusal with a draft; isolated draft-only veto is unit-tested."}
        if model_turn:
            fixture.send(b"\x15")
            fixture.pump(0.2)
            if wait_next_turn:
                fixture.command_line("/hardcore-slurp wait")
                fixture.pump(0.5)
            prompt = "Reply with exactly HCPROBE_READY and nothing else."
            if fixture.fixture_command:
                prompt = ("For this disposable test run exactly " + fixture.fixture_command +
                    " using Bash with run_in_background false. Do not launch anything else or rerun it. "
                    "After it completes, reply with exactly HCPROBE_READY and nothing else.")
            fixture.send(prompt.encode())
            fixture.pump(0.25)
            fixture.send(b"\r")
            events = fixture.wait_json(fixture.work / "handoff-probe-events.json",
                lambda value: any(item["event"] == "turn.complete" for item in value), timeout=90)
            completion = next(item["data"] for item in reversed(events)
                              if item["event"] == "turn.complete")
            assert completion["reason"] == "answer" and not completion["isAborted"], completion
            assert "HCPROBE_READY" in completion["answer"]
            stop_event = next(item["data"] for item in reversed(events) if item["event"] == "Stop")
            assert stop_event.get("background_tasks") == [], stop_event.get("background_tasks")
            assert stop_event.get("session_crons") == [], stop_event.get("session_crons")
            if fixture.fixture_command:
                launches = (fixture.work / "handoff-job-launches").read_text().splitlines()
                done = (fixture.work / "handoff-job-done").read_text()
                assert launches == [done], "Foreground tool did not finish exactly once"
                result["foregroundWork"] = {"pid": int(done), "launchCount": len(launches),
                                            "finishedBeforeHandoff": True}
            if not wait_next_turn:
                start = len(fixture.raw)
                fixture.command_line("/hardcore-slurp status")
                fixture.pump(2)
                status = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "",
                                fixture.raw[start:].decode(errors="replace"))
                assert contains(status, "Claude is settled and ready for Hardcore."), status[-3000:]
                fixture.command_line("/hardcore-slurp-dispatch 00000000000040008000000000000002")
                fixture.pump(2)
                assert fixture.proc.poll() is None
                final_state = fixture.wait_json(status_path)
                assert final_state["sessionId"] == before["sessionId"]
            result.update(noModelPromptSent=False, realModelTurns=1, readyVerified=True,
                          actualEvents=events)
            print("PASS real model turn produced Stop with empty task/cron lists, then normal completion and READY")
            if executable:
                expected_draft = None
                expected_cursor = None
                if external_draft:
                    expected_draft = "HCPROBE UNSENT: café 🚀 preserve exact text\nsecond line; $(do-not-run)"
                    fixture.send(b"\x1b[200~" + expected_draft.encode() + b"\x1b[201~")
                    fixture.wait_json(status_path, lambda value: value["prompt"]["text"] == expected_draft)
                    fixture.send(b'\x1b[D' * 7)
                    expected_cursor = len(expected_draft.encode('utf-16-le')) // 2 - 7
                    fixture.wait_json(status_path, lambda value: value["prompt"] == {'text': expected_draft, 'cursor': expected_cursor})
                    queued = subprocess.run([executable, "claude", "move", before["sessionId"],
                        "--marketplace", str(market), "--json"],
                        cwd=fixture.work, env=fixture.env, text=True, capture_output=True, timeout=20)
                    (root / "external-queue.json").write_text(queued.stdout + queued.stderr)
                    assert queued.returncode == 0 and json.loads(queued.stdout)["ok"], queued.stdout
                result["handoff"] = handoff(fixture, before["sessionId"], completion["turnId"],
                    wait_next_turn or external_draft, expected_draft, expected_cursor)
                result["realModelTurns"] = 2
                result["sourceStillRunning"] = False
                result["sourceSurvivedRefusedRequests"] = True
                result["waitNextTurn"] = wait_next_turn
                if fixture.fixture_command:
                    assert (fixture.work / "handoff-job-launches").read_text().splitlines() == [done]
                    assert (fixture.work / "handoff-job-done").stat().st_mtime <= result["handoff"]["request"]["created"]
                    print("PASS foreground Bash work completed exactly once before receiver preparation")
                print("PASS prepared native Hardcore receiver attached exact new background session after source exit")
                print("PASS conversation recall after original PTY closed")
        (root / "result.json").write_text(json.dumps(result, indent=2))
        print("PASS production adapter late-load refuses unknown session state")
        print("PASS production driver reaches separate execution-time guard and refuses an unprepared request")
        print("PASS source PID, session identity, and unsent draft survive both refused requests")
        if not executable:
            print("LIMITATION no actual handoff attempted")
    except BaseException as error:
        (root / "failure.json").write_text(json.dumps({"error": str(error)}, indent=2))
        if executable:
            (root / "failure-windows.json").write_text(json.dumps(fixture.windows(), indent=2))
            try:
                fixture.cli(["agents", "--json", "--all", "--cwd", str(fixture.work)],
                            "failure-agents.json")
                if fixture.background_job:
                    fixture.cli(["logs", fixture.background_job], "failure-background.log")
            except (AssertionError, subprocess.SubprocessError):
                pass
        raise
    finally:
        fixture.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-turn", action="store_true",
                        help="Send one tiny real model prompt to verify Stop/turn.complete and READY")
    parser.add_argument("--handoff", type=Path, metavar="HARDCORE",
                        help="Run actual cooperative handoff into this Hardcore binary (implies --model-turn)")
    parser.add_argument("--wait-next-turn", action="store_true",
                        help="With --handoff, arm wait before the model turn and move automatically on completion")
    parser.add_argument("--foreground-work", action="store_true",
                        help="With --wait-next-turn, run one precisely allowed foreground fixture before moving")
    parser.add_argument("--external-draft", action="store_true",
                        help="Queue the move from Hardcore while the settled source holds an unsent Unicode draft")
    args = parser.parse_args()
    if args.wait_next_turn and not args.handoff:
        parser.error("--wait-next-turn requires --handoff")
    if args.foreground_work and not args.wait_next_turn:
        parser.error("--foreground-work requires --wait-next-turn")
    if args.external_draft and (not args.handoff or args.wait_next_turn):
        parser.error("--external-draft requires --handoff and excludes --wait-next-turn")
    root = Path(tempfile.mkdtemp(prefix="hc-claude-handoff-guard-", dir="/tmp"))
    try:
        probe(root, args.model_turn or bool(args.handoff),
              str(args.handoff.resolve(strict=True)) if args.handoff else None, args.wait_next_turn,
              args.foreground_work, args.external_draft)
    finally:
        print("Private diagnostics:", root)


if __name__ == "__main__":
    main()
