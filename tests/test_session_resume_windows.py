#!/usr/bin/env python3
"""Claude sessions come back after a crash, a stopped WSL and a closed window;
a tab you close stays closed.

Run from WSL after ``make hardcore.exe``. Each phase starts its own copy of
hardcore.exe with a private config (HARDCORE_TEST_CONFIG) and its own session
folder, in a job object that ends with it. It never touches the installed
hardcore, its config or its saved tabs, and never stops WSL: a "stopped WSL"
is the tab's own bash killed with SIGKILL. A stand-in `claude` (put ahead of
the real one with HARDCORE_TEST_PATH) logs how it was started and sleeps.
"""
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/session-resume-windows"

DRIVER = r'''
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static DWORD owned_pid;
static HWND owned_window;
static BOOL CALLBACK find_owned(HWND window, LPARAM unused) {
    DWORD pid; wchar_t cls[32]; (void)unused;
    GetWindowThreadProcessId(window, &pid);
    if (pid == owned_pid && GetClassNameW(window, cls, 32) && !wcscmp(cls, L"hardcore")) owned_window = window;
    return TRUE;
}
static int copydata(ULONG_PTR tag, const char *text) {
    COPYDATASTRUCT data = {tag, (DWORD)strlen(text) + 1, (void *)text};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(owned_window, WM_COPYDATA, 0, (LPARAM)&data, SMTO_ABORTIFHUNG, 15000, &result) && result;
}
/* driver EXE CONFIG FAKE_LINUX_PATH */
int wmain(int argc, wchar_t **argv) {
    PROCESS_INFORMATION process = {0}; STARTUPINFOW startup = {sizeof(startup)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    wchar_t command[4096], wslenv[2048] = L""; char line[4096]; HANDLE job;
    if (argc != 4) return 2;
    SetEnvironmentVariableW(L"HARDCORE_TEST_CONFIG", argv[2]);
    SetEnvironmentVariableW(L"LOCALAPPDATA", argv[2]);
    SetEnvironmentVariableW(L"HARDCORE_SESSIONS", L"1");
    SetEnvironmentVariableW(L"HARDCORE_ADOPT", NULL);
    SetEnvironmentVariableW(L"HARDCORE_RUN", NULL);
    SetEnvironmentVariableW(L"HARDCORE_TEST_PATH", argv[3]);
    GetEnvironmentVariableW(L"WSLENV", wslenv, 1900);
    wcscat(wslenv, wslenv[0] ? L":HARDCORE_TEST_PATH" : L"HARDCORE_TEST_PATH");
    SetEnvironmentVariableW(L"WSLENV", wslenv);
    job = CreateJobObjectW(NULL, NULL);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 3;
    _snwprintf(command, 4096, L"\"%ls\" --no-activate", argv[1]);
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNOACTIVATE;
    if (!CreateProcessW(argv[1], command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &process)) return 4;
    if (!AssignProcessToJobObject(job, process.hProcess)) { TerminateProcess(process.hProcess, 1); return 5; }
    owned_pid = process.dwProcessId;
    ResumeThread(process.hThread); CloseHandle(process.hThread);
    for (int i = 0; i < 400 && !owned_window; i++) {
        if (WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT) break;
        EnumWindows(find_owned, 0); Sleep(50);
    }
    if (!owned_window) { TerminateJobObject(job, 1); return 6; }
    printf("OK %lu\n", owned_pid); fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
        int ok = 0; line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "K")) {            /* a crash: hardcore and everything under it gone at once */
            TerminateJobObject(job, 1);
            WaitForSingleObject(process.hProcess, 5000);
            puts("OK"); fflush(stdout); return 0;
        }
        if (!strcmp(line, "Q")) {            /* the window's close button; then the job ends what's left */
            PostMessageW(owned_window, WM_CLOSE, 0, 0);
            ok = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
            TerminateJobObject(job, 1);
            puts(ok ? "OK" : "FAIL"); fflush(stdout); return 0;
        }
        if (!strcmp(line, "W")) {            /* the window closed by itself? */
            ok = WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
            if (ok) TerminateJobObject(job, 1);
            puts(ok ? "OK" : "FAIL"); fflush(stdout); if (ok) return 0; continue;
        }
        if (!strcmp(line, "A")) ok = WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT;   /* still running */
        else if (line[0] == 'C' && line[1] == ' ') ok = copydata(0x48430002, line + 2);
        else if (line[0] == 'T' && line[1] == ' ' && strlen(line) + 1 < sizeof(line)) { strcat(line, "\n"); ok = copydata(0x48430001, line + 2); }
        puts(ok ? "OK" : "FAIL"); fflush(stdout);
    }
    TerminateJobObject(job, 1);
    return 0;
}
'''

FAKE_CLAUDE = r'''#!/bin/bash
# a stand-in for Claude Code: how it was started, a transcript like the real
# one's (so a restore finds it), then it waits like an idle session
here=$(cd "$(dirname "$0")" && pwd)
echo "$PWD|$*" >> "$here/calls.log"
id= prev=
for a in "$@"; do
  case $prev in --session-id|--resume) id=$a ;; esac
  prev=$a
done
cfg=${CLAUDE_CONFIG_DIR:-$HOME/.claude}
proj=$cfg/projects/$(printf '%s' "$PWD" | sed 's/[^A-Za-z0-9]/-/g')
mkdir -p "$proj" && touch "$proj/$id.jsonl"
# like Claude Code: sessions/PID.json names the session on screen (/resume changes it)
mkdir -p "$cfg/sessions"
printf '{"pid":%s,"sessionId":"%s","cwd":"%s","procStart":"%s"}' $$ "$id" "$PWD" "$(cut -d' ' -f22 /proc/$$/stat)" > "$cfg/sessions/$$.json"
echo "$$" > "$here/pid-$id"
echo "$PPID" > "$here/shell-$id"
echo "fake claude $id"
exec sleep 600
'''


def winpath(path):
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()


def wait_for(check, description, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError(f"timed out waiting for {description}")


class App:
    def __init__(self, helper, exe, config, fake):
        self.log = open(OUT / "driver.log", "a")
        env = dict(os.environ, CLAUDE_CONFIG_DIR=str(fake.parent / "claude-home"))
        env["WSLENV"] = (env.get("WSLENV", "") + ":CLAUDE_CONFIG_DIR").lstrip(":")
        self.proc = subprocess.Popen([str(helper), winpath(exe), winpath(config), str(fake)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log, env=env)
        self.read(40)

    def read(self, timeout=30):
        ready = select.select([self.proc.stdout], [], [], timeout)[0]
        assert ready, "driver did not answer"
        answer = self.proc.stdout.readline().decode().strip()
        assert answer.startswith("OK"), f"driver: {answer!r}"
        return answer

    def send(self, command, timeout=30):
        self.proc.stdin.write((command + "\n").encode())
        self.proc.stdin.flush()
        return self.read(timeout)

    def end(self, how):
        try:
            self.send(how, 20)
            self.proc.wait(timeout=10)
        finally:
            if self.proc.poll() is None:
                self.proc.kill()
            self.log.close()


def sessions(config):
    folder = config / "session"
    return sorted(p for p in folder.glob("*.txt") if "-tab" not in p.name) if folder.exists() else []


def saved_tabs(config):
    tabs = []
    for path in sessions(config):
        for block in path.read_text().split("[tab]")[1:]:
            tabs.append(dict(re.findall(r"^(\w+)=(.*)$", block, re.M)))
    return tabs


def calls(fake):
    log = fake / "calls.log"
    return log.read_text().splitlines() if log.exists() else []


def kill_shell(fake, sid):
    pid = (fake / f"shell-{sid}").read_text().strip()
    (fake / f"shell-{sid}").unlink()
    subprocess.run(["kill", "-9", pid], check=True)


def run(exe):
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "driver.log").write_text("")
    source, helper = OUT / "driver.c", OUT / "driver.exe"
    source.write_text(DRIVER)
    subprocess.run([os.environ.get("CC", "x86_64-w64-mingw32-gcc"), "-O2", "-Wall", "-municode", "-o", str(helper),
                    str(source), "-luser32"], check=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=OUT))       # Windows side: config, exe copy
    copied = work / "hardcore-session-test.exe"
    shutil.copy2(exe, copied)
    config = work / "config"
    config.mkdir()
    (config / "config.ini").write_text("shell=\nwidth=900\nheight=600\n")
    lin = Path(tempfile.mkdtemp(prefix="hcsess-"))                 # Linux side: the folders Claude runs in
    fake = lin / "bin"
    fake.mkdir()
    (fake / "claude").write_text(FAKE_CLAUDE)
    (fake / "claude").chmod(0o755)
    w1, w2 = lin / "w1", lin / "w2"
    w1.mkdir()
    w2.mkdir()
    home = lin / "claude-home"                                      # CLAUDE_CONFIG_DIR: never the real ~/.claude
    flags = "--dangerously-skip-permissions --model opus"
    try:
        # 1. two Claude tabs, one closed by the user, then a crash
        app = App(helper, copied, config, fake)
        app.send(f"T cd {w1} && claude {flags}")
        wait_for(lambda: len(calls(fake)) == 1, "the first claude")
        app.send("C newtab")
        app.send(f"T cd {w2} && claude --dangerously-skip-permissions")
        wait_for(lambda: len(calls(fake)) == 2, "the second claude")
        id1 = re.search(r"--session-id (\S+)", calls(fake)[0]).group(1)
        id2 = re.search(r"--session-id (\S+)", calls(fake)[1]).group(1)
        wait_for(lambda: {t.get("claude") for t in saved_tabs(config)} == {id1, id2}, "both sessions saved (autosave)")
        tabs = {t["claude"]: t for t in saved_tabs(config)}
        assert tabs[id1]["claude_trust"] == "1" and "--dangerously-skip-permissions" in tabs[id1]["claude_args"], tabs
        app.send("C closetab")                       # you closed the second tab: it's done
        wait_for(lambda: [t.get("claude") for t in saved_tabs(config)] == [id1], "the closed tab gone from the session", 5)
        app.end("K")                                 # crash: no WM_CLOSE, no save
        print("ok   autosave keeps open Claude tabs; a closed tab leaves the session at once")

        # 2. after the crash: the open tab resumes with the same flags, the closed one doesn't
        before = len(calls(fake))
        app = App(helper, copied, config, fake)
        wait_for(lambda: len(calls(fake)) > before, "the resumed claude")
        resumed = calls(fake)[before:]
        assert resumed == [f"{w1}|{flags} --resume {id1}"], resumed
        print("ok   after a crash Claude resumes in its folder with its flags; the closed tab stays closed")

        # 3. WSL stops under Claude (its shell killed): the tab stays, Enter resumes
        wait_for(lambda: (fake / f"shell-{id1}").exists(), "the resumed claude's shell")
        kill_shell(fake, id1)
        time.sleep(2)
        app.send("A")                                # the window didn't close with its only tab
        wait_for(lambda: [t.get("claude") for t in saved_tabs(config)] == [id1], "the stopped tab still saved", 6)
        before = len(calls(fake))
        app.send("T ")                               # Enter
        wait_for(lambda: len(calls(fake)) > before, "claude resumed by Enter")
        assert calls(fake)[before:] == [f"{w1}|{flags} --resume {id1}"], calls(fake)
        print("ok   a shell that stops under Claude keeps the tab; Enter resumes Claude")

        # 4. stopped again, and the window closed: the next start resumes it
        wait_for(lambda: (fake / f"shell-{id1}").exists(), "the shell again")
        kill_shell(fake, id1)
        time.sleep(2)
        app.end("Q")
        assert [t.get("claude") for t in saved_tabs(config)] == [id1], saved_tabs(config)
        before = len(calls(fake))
        app = App(helper, copied, config, fake)
        wait_for(lambda: len(calls(fake)) > before, "claude resumed after the window closed")
        assert calls(fake)[before:] == [f"{w1}|{flags} --resume {id1}"], calls(fake)
        print("ok   a window closed with a stopped Claude tab resumes it on the next start")

        # 5. you close the last tab: the window goes and nothing comes back
        app.send("C closetab")
        app.end("W")
        assert not sessions(config), sessions(config)
        before = len(calls(fake))
        app = App(helper, copied, config, fake)
        time.sleep(4)
        assert len(calls(fake)) == before, calls(fake)[before:]
        app.end("Q")
        print("ok   closing the last tab yourself ends the session")

        # 6. an older build's session.txt and a second window's session: both come back
        for p in (config / "session").glob("*"):
            p.unlink()
        (config / "session/session.txt").write_text(
            f"# old\nactive=0\n\n[tab]\nview=0\nkind=wsl\ndir={w1}\nclaude={id1}\n"
            f"claude_args=--model opus \ncmd=\ntext=tab0.txt\n")
        (config / "session/tab0.txt").write_text("old text\r\n")
        (config / "session/w00000001aaaaaaaa.txt").write_text(
            f"active=0\n\n[tab]\nview=0\nkind=wsl\ndir={w2}\nclaude={id2}\n"
            f"claude_args={flags} \nclaude_trust=1\ncmd=\ntext=w00000001aaaaaaaa-tab0.txt\n")
        before = len(calls(fake))
        app = App(helper, copied, config, fake)
        wait_for(lambda: len(calls(fake)) >= before + 2, "two windows resuming")
        got = sorted(calls(fake)[before:])
        assert got == sorted([f"{w1}|--model opus --resume {id1}", f"{w2}|{flags} --resume {id2}"]), got
        wait_for(lambda: not (config / "session/tab0.txt").exists(), "the old build's text file replaced", 8)
        app.end("K")
        print("ok   an older build's session and every other saved window come back (untrusted flags filtered)")

        # 7. /resume inside Claude picks another session: the restore resumes that one
        for p in (config / "session").glob("*"):
            p.unlink(missing_ok=True)
        before = len(calls(fake))
        app = App(helper, copied, config, fake)
        app.send(f"T cd {w1} && claude {flags}")
        wait_for(lambda: len(calls(fake)) > before, "a new claude")
        id4 = re.search(r"--session-id (\S+)", calls(fake)[before]).group(1)
        wait_for(lambda: [t.get("claude") for t in saved_tabs(config)] == [id4], "the new session saved")
        picked = "0b5e7c2a-1d34-4e8f-9a6b-5c4d3e2f1a0b"            # what /resume switches to
        pid = (fake / f"pid-{id4}").read_text().strip()
        state = home / "sessions" / f"{pid}.json"
        proj = home / "projects" / re.sub(r"[^A-Za-z0-9]", "-", str(w1))
        (proj / f"{picked}.jsonl").touch()
        state.write_text(state.read_text().replace(id4, picked))
        wait_for(lambda: [t.get("claude") for t in saved_tabs(config)] == [picked], "the picked session saved", 15)
        app.end("K")
        before = len(calls(fake))
        app = App(helper, copied, config, fake)
        wait_for(lambda: len(calls(fake)) > before, "the picked session resumed")
        assert calls(fake)[before:] == [f"{w1}|{flags} --resume {picked}"], calls(fake)[before:]
        app.end("K")
        print("ok   a session picked with /resume inside Claude is the one that comes back")
    finally:
        shutil.rmtree(lin, ignore_errors=True)


if __name__ == "__main__":
    run(ROOT / "hardcore.exe")
